/*
 * gw_bench.h — scene manifests and event scoring.
 *
 * Tool-side, not product runtime: none of this is in libgeist_watch.a.
 * It exists so a run over recorded material is repeatable and so the
 * numbers the release criteria ask for are produced by code that can
 * itself be tested, rather than by a spreadsheet nobody re-derives.
 *
 * A manifest is a plain line-oriented text file. No JSON, because the
 * project carries no parser and a benchmark input is not worth acquiring
 * one for; no binary, because a human has to be able to read a scene's
 * ground truth and see that it says what they meant.
 *
 *   # comments and blank lines are ignored
 *   version      1
 *   scene        doorstep-01
 *   split        dev | test
 *   camera       cam-front
 *   match_window <after_ms> [<before_ms>]
 *   tick_ms      <ms>                      (default 1000)
 *   rule         <id> appeared|sustained yes|no <count> <window_ms> <gap_ms> [<sustain_ms>]
 *   truth        <rule_id> <event_type> <at_ms> [<after_ms>]
 *   restart      <t_ms>
 *   sample       <t_ms> <frame|-> <rule_id>=<yes|no|unknown> ...
 *
 * `sample` carries a label per rule so the harness runs today, with no
 * engine and no model: the labels stand in for what a VLM will answer
 * once one is chosen. The `frame` column is the path that model will be
 * given, and is carried through untouched until then — a scene authored
 * now stays valid when the model arrives.
 */
#ifndef GW_BENCH_H
#define GW_BENCH_H

#include "geist_watch.h"

enum {
    GW_BENCH_MAX_SAMPLES = 8192,
    GW_BENCH_MAX_TRUTH = 256,
    GW_BENCH_MAX_DETECTIONS = 256,
    GW_BENCH_PATH_CAP = 128,
    GW_BENCH_TYPE_CAP = 32,
    GW_BENCH_MAX_RESTARTS = 8,
};

enum gw_split {
    GW_SPLIT_DEV = 0,
    GW_SPLIT_TEST,
};

/* One labelled moment. `label[r]` is the answer for rule r, valid only
 * where `has_label[r]`. A sample that does not mention a rule feeds it
 * nothing — distinct from labelling it `unknown`, which IS an answer and
 * does reset that rule's evidence. Collapsing the two would let a scene
 * that simply omits a rule look like one that kept reporting on it. */
struct gw_sample {
    int64_t       t_ns;
    char          frame[GW_BENCH_PATH_CAP];
    enum gw_value label[GW_MAX_RULES];
    bool          has_label[GW_MAX_RULES];
};

struct gw_truth {
    uint32_t rule;
    char     event_type[GW_BENCH_TYPE_CAP];
    int64_t  at_ns;
    /* A detection counts for this event when it lands in
     * [at_ns - before_ns, at_ns + after_ns]. Fixed in the manifest
     * BEFORE a run, never tuned against results — that is the whole
     * point of writing it down next to the ground truth. */
    int64_t before_ns;
    int64_t after_ns;
};

struct gw_scene {
    char          name[GW_BENCH_TYPE_CAP];
    char          camera[GW_ID_CAP];
    enum gw_split split;

    struct gw_rule_config rules[GW_MAX_RULES];
    uint32_t              rule_count;

    struct gw_sample samples[GW_BENCH_MAX_SAMPLES];
    uint32_t         sample_count;

    struct gw_truth truth[GW_BENCH_MAX_TRUTH];
    uint32_t        truth_count;

    /* How often the replay ticks the core, independent of samples. The
     * product's scheduler runs whether or not a frame arrived, and so
     * must the replay — otherwise evidence expiry never fires and a
     * camera outage cannot be measured. Must be no coarser than the
     * smallest rule's max_gap, or a gap could pass between two ticks
     * unnoticed; the parser checks that. */
    int64_t tick_ns;

    /* Process restarts, in time order. At each one the watch is
     * re-initialised mid-scene: every rule returns to unknown, disarmed,
     * with no sustained run. The plan lists a restart among the
     * sequences a scene must contain, and it is the one event that
     * cannot be expressed as a label — nothing the camera sees says the
     * process died. */
    int64_t  restarts[GW_BENCH_MAX_RESTARTS];
    uint32_t restart_count;
};

/* Parse a manifest. On failure writes a one-line reason into `err`
 * (including the offending line number) and returns non-OK. */
[[nodiscard]] enum gw_status gw_scene_parse(const char *text, size_t err_cap, char *err,
                                            struct gw_scene *out);

/* What a run produced. */
struct gw_detection {
    uint32_t rule;
    int64_t  at_ns;    /* observation time that completed it, not tick time */
    uint32_t matched;  /* index into scene truth, or UINT32_MAX */
};

struct gw_metrics {
    uint32_t true_positives;
    uint32_t false_positives;
    uint32_t false_negatives;

    /* Wilson score intervals at 95 %. Reported next to the point
     * estimate because on 50 sequences the difference between 92 % and
     * 98 % precision is noise, and a bare percentage hides that. */
    double precision, precision_lo, precision_hi;
    double recall, recall_lo, recall_hi;

    /* Fraction of scene time in which the rule held a confirmed value.
     * Unknown phases stay in the denominator: a run that spent half the
     * scene blind and got the other half right is not a 100 % run. */
    double  availability;
    int64_t observed_ns;
    int64_t unknown_ns;
};

/* Replay a scene through the core and collect detections. Deterministic:
 * the same scene always produces the same detections, because the core
 * takes its clock from the sample timestamps. */
[[nodiscard]] enum gw_status gw_replay(const struct gw_scene *scene, uint32_t cap,
                                       struct gw_detection *out, uint32_t *out_n);

/* Match detections against ground truth and compute the metrics.
 *
 * Matching is one-to-one and greedy in time order: each detection takes
 * the earliest still-unmatched truth event whose window contains it.
 * Greedy rather than optimal-bipartite on purpose — it is explainable
 * from the output, and the two agree whenever events are further apart
 * than the match window, which is every scene the plan describes. Where
 * they differ, greedy scores no higher, so it cannot flatter a run.
 *
 * `detections` is modified: each entry's `matched` is filled in, so a
 * report can show which detection answered which event. */
[[nodiscard]] enum gw_status gw_score(const struct gw_scene *scene, uint32_t n_det,
                                      struct gw_detection *detections, uint32_t rule,
                                      struct gw_metrics *out);

#endif /* GW_BENCH_H */
