/*
 * gw_report.h — the benchmark report, and the gate it must pass.
 *
 * Written before the first measurement exists, which is the entire point.
 * The plan freezes thresholds BEFORE the final test precisely so they cannot
 * be chosen to fit whatever numbers turned up; a grader written afterwards,
 * against results already in hand, is not a gate but a rationalisation.
 *
 * Two files, two jobs:
 *
 *   A REPORT is what one run produced. It carries provenance — which model,
 *   which engine, which flags, which hardware — and the measurements. The
 *   provenance is not decoration: a latency without the quantisation and
 *   the image profile it was measured at cannot be compared to anything,
 *   including itself next month.
 *
 *   A CRITERIA file is what v0.1 requires. It is committed, reviewed, and
 *   read-only to the grader.
 *
 * Three rules make the grade mean something:
 *
 *   1. A required metric that is absent FAILS. "Not measured" and "passed"
 *      must never render the same, and the cheapest way to pass a gate is
 *      to quietly not run the case.
 *   2. A report missing any required provenance field is not graded at all.
 *      Grading it would produce a verdict about a run nobody can identify.
 *   3. Every criterion is reported, met or not. The plan says missed targets
 *      are reported visibly; a grader that prints only failures lets a
 *      shrinking suite look like progress.
 */
#ifndef GW_REPORT_H
#define GW_REPORT_H

#include "geist_watch.h"

enum {
    GW_REPORT_MAX_METRICS = 64,
    GW_REPORT_MAX_CRITERIA = 64,
    GW_REPORT_KEY_CAP = 40,
    GW_REPORT_VALUE_CAP = 96,
};

/* The provenance a report must carry to be gradable at all. Each is here
 * because a number is uninterpretable without it, not to be thorough:
 * quantisation and the image profile move latency by more than the
 * difference between passing and failing, and an engine commit is the only
 * way to tell a regression from a different program. */
enum gw_provenance_field {
    GW_PROV_MODEL_ID = 0,
    GW_PROV_MODEL_SHA256,
    GW_PROV_MODEL_QUANTIZATION,
    GW_PROV_ENGINE_COMMIT,
    GW_PROV_COMPILER,
    GW_PROV_COMPILER_FLAGS,
    GW_PROV_OS,
    GW_PROV_HARDWARE,
    GW_PROV_SOURCE, /* camera id, or the scene set for a replay */
    GW_PROV_ROI,
    GW_PROV_RESOLUTION,
    GW_PROV_SAMPLING,
    GW_PROV_PROMPT_ID,
    GW_PROV_MAX_TOKENS,
    GW_PROV_START, /* cold or warm: a warm number is not a start-up number */
    GW_PROV_COUNT,
};

/* Field name as it appears in a report file. Never null. */
const char *gw_provenance_key(enum gw_provenance_field f);

struct gw_metric {
    char key[GW_REPORT_KEY_CAP];
    double value;
};

struct gw_report {
    char provenance[GW_PROV_COUNT][GW_REPORT_VALUE_CAP];
    bool has_provenance[GW_PROV_COUNT];
    struct gw_metric metrics[GW_REPORT_MAX_METRICS];
    uint32_t metric_count;
};

enum gw_compare {
    GW_CMP_LE = 0, /* measured <= limit */
    GW_CMP_GE,     /* measured >= limit */
};

struct gw_criterion {
    char key[GW_REPORT_KEY_CAP];
    enum gw_compare cmp;
    double limit;
    /* Free text naming what this criterion is for, printed with the
     * verdict so a reader need not hold the plan in their head. */
    char about[GW_REPORT_VALUE_CAP];
};

struct gw_criteria {
    char profile[GW_REPORT_KEY_CAP];
    struct gw_criterion items[GW_REPORT_MAX_CRITERIA];
    uint32_t count;
};

/* Parse. Both write a one-line reason (with the offending line number)
 * into `err` on failure. */
[[nodiscard]] enum gw_status gw_report_parse(const char *text, size_t err_cap, char *err,
                                             struct gw_report *out);
[[nodiscard]] enum gw_status gw_criteria_parse(const char *text, size_t err_cap, char *err,
                                               struct gw_criteria *out);

enum gw_verdict {
    GW_VERDICT_PASS = 0,
    GW_VERDICT_FAIL,
    GW_VERDICT_NOT_MEASURED, /* distinct from FAIL: nothing was run */
};

struct gw_grade {
    enum gw_verdict verdict;
    double measured; /* meaningless when NOT_MEASURED */
};

/* Grade one report against one criteria file.
 *
 * Returns GW_E_INVALID_ARG, with the missing field named in `err`, when the
 * report lacks any required provenance — a verdict about an unidentifiable
 * run is worse than no verdict.
 *
 * `out_grades` receives one entry per criterion, in the criteria file's
 * order. `out_failed` counts FAIL and NOT_MEASURED together: both mean the
 * release criterion is not demonstrated. */
[[nodiscard]] enum gw_status gw_report_grade(const struct gw_report *report,
                                             const struct gw_criteria *criteria, size_t err_cap,
                                             char *err, struct gw_grade *out_grades,
                                             uint32_t *out_failed);

#endif /* GW_REPORT_H */
