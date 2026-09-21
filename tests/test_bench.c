/*
 * Manifest parsing and event scoring.
 *
 * The scoring cases matter more than they look: every one of them is a
 * way a benchmark can quietly flatter itself. A matcher that lets one
 * detection answer two events, or that drops unknown time out of the
 * denominator, produces numbers that pass a review and mean nothing.
 */
#include "gw_bench.h"
#include "test_support.h"

#include <string.h>

static struct gw_scene g_scene;

static const char *MINIMAL = "version 1\n"
                             "scene s\n"
                             "camera cam\n"
                             "match_window 45000\n"
                             "rule doorstep appeared yes 3 10000 15000\n"
                             "truth doorstep package_appeared 15000\n"
                             "sample 0 - doorstep=no\n"
                             "sample 5000 - doorstep=no\n"
                             "sample 10000 - doorstep=no\n"
                             "sample 15000 - doorstep=yes\n"
                             "sample 20000 - doorstep=yes\n"
                             "sample 25000 - doorstep=yes\n";

/* --- parsing -------------------------------------------------------- */

static void expect_parse_error(const char *text, const char *needle) {
    char err[256] = "";
    if (gw_scene_parse(text, sizeof err, err, &g_scene) == GW_OK) {
        fprintf(stderr, "  expected a parse error mentioning '%s'\n", needle);
        gw_test_failures += 1;
        return;
    }
    if (strstr(err, needle) == nullptr) {
        fprintf(stderr, "  error was '%s', expected it to mention '%s'\n", err, needle);
        gw_test_failures += 1;
    }
}

static void test_minimal_manifest(void) {
    char err[256] = "";
    CHECK_OK(gw_scene_parse(MINIMAL, sizeof err, err, &g_scene));
    CHECK_EQ_INT(g_scene.rule_count, 1);
    CHECK_EQ_INT(g_scene.truth_count, 1);
    CHECK_EQ_INT(g_scene.sample_count, 6);
    CHECK_EQ_INT(g_scene.split, GW_SPLIT_DEV);
    CHECK(strcmp(g_scene.rules[0].id, "doorstep") == 0);
    CHECK_EQ_INT(g_scene.rules[0].confirm_window_ns, 10000 * NS_MS);
    CHECK_EQ_INT(g_scene.truth[0].after_ns, 45000 * NS_MS);
    CHECK_EQ_INT(g_scene.tick_ns, 1000 * NS_MS); /* default */
}

/* An omitted rule is not the same as one labelled unknown: the first
 * feeds nothing, the second is an answer that resets evidence. */
static void test_omitted_label_is_not_unknown(void) {
    char err[256] = "";
    CHECK_OK(gw_scene_parse("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                            "rule a appeared yes 1 0 15000\n"
                            "rule b appeared yes 1 0 15000\n"
                            "sample 0 - a=yes\n",
                            sizeof err, err, &g_scene));
    CHECK(g_scene.samples[0].has_label[0]);
    CHECK(!g_scene.samples[0].has_label[1]);
}

static void test_parser_refuses_what_it_cannot_mean(void) {
    expect_parse_error("scene s\n", "version 1");
    expect_parse_error("version 1\nwibble x\n", "unknown keyword 'wibble'");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "sample 0 - b=yes\n",
                       "rule 'b', which is not declared");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "sample 0 - a=maybe\n",
                       "not yes, no or unknown");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "sample 0 - a=yes a=no\n",
                       "labelled twice");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "sample 5000 - a=yes\nsample 0 - a=yes\n",
                       "time order");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a sustained yes 1 0 15000\n",
                       "needs sustain_ms");
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000 300000\n",
                       "takes no sustain_ms");
    /* Truth without a window cannot be matched against anything. */
    expect_parse_error("version 1\nscene s\ncamera c\n"
                       "rule a appeared yes 1 0 15000\n"
                       "truth a x 1000\n",
                       "no match window");
}

/* A tick coarser than a rule's evidence window would step over an
 * outage, so the scene would score a gap the running system catches. */
static void test_tick_coarser_than_gap_is_refused(void) {
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\ntick_ms 20000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "sample 0 - a=yes\n",
                       "coarser than rule 'a' max_gap");
}

static void test_error_carries_a_line_number(void) {
    expect_parse_error("version 1\nscene s\ncamera c\n\n# a comment\nwibble\n", "line 6:");
}

/* --- scoring -------------------------------------------------------- */

/* Build a scene with `n` truth events at fixed times, then score a set
 * of detections handed in directly, so matching is tested without
 * depending on what the core happens to emit. */
static void score_with(const char *manifest, uint32_t n_det, struct gw_detection *dets,
                       struct gw_metrics *out) {
    char err[256] = "";
    CHECK_OK(gw_scene_parse(manifest, sizeof err, err, &g_scene));
    CHECK_OK(gw_score(&g_scene, n_det, dets, 0u, out));
}

static const char *TWO_TRUTHS = "version 1\nscene s\ncamera c\nmatch_window 10000\n"
                                "rule a appeared yes 1 0 15000\n"
                                "truth a x 100000\n"
                                "truth a x 200000\n"
                                "sample 0 - a=no\n";

static void test_matching_is_one_to_one(void) {
    /* Two detections inside the same window: the first answers the
     * event, the second is a false alarm — not a second success. */
    struct gw_detection d[] = {
        {.rule = 0u, .at_ns = 101000 * NS_MS, .matched = UINT32_MAX},
        {.rule = 0u, .at_ns = 102000 * NS_MS, .matched = UINT32_MAX},
    };
    struct gw_metrics m;
    score_with(TWO_TRUTHS, 2u, d, &m);
    CHECK_EQ_INT(m.true_positives, 1);
    CHECK_EQ_INT(m.false_positives, 1);
    CHECK_EQ_INT(m.false_negatives, 1); /* the 200 s event went unanswered */
    CHECK_EQ_INT(d[0].matched, 0);
    CHECK_EQ_INT(d[1].matched, UINT32_MAX);
}

static void test_detection_outside_the_window_is_a_false_alarm(void) {
    struct gw_detection d[] = {
        {.rule = 0u, .at_ns = 115000 * NS_MS, .matched = UINT32_MAX}, /* 5 s too late */
    };
    struct gw_metrics m;
    score_with(TWO_TRUTHS, 1u, d, &m);
    CHECK_EQ_INT(m.true_positives, 0);
    CHECK_EQ_INT(m.false_positives, 1);
    CHECK_EQ_INT(m.false_negatives, 2);
}

/* A detection BEFORE its event is not a prediction. */
static void test_detection_before_the_event_does_not_count(void) {
    struct gw_detection d[] = {
        {.rule = 0u, .at_ns = 99000 * NS_MS, .matched = UINT32_MAX},
    };
    struct gw_metrics m;
    score_with(TWO_TRUTHS, 1u, d, &m);
    CHECK_EQ_INT(m.true_positives, 0);
    CHECK_EQ_INT(m.false_positives, 1);
}

static void test_both_events_answered(void) {
    struct gw_detection d[] = {
        {.rule = 0u, .at_ns = 105000 * NS_MS, .matched = UINT32_MAX},
        {.rule = 0u, .at_ns = 205000 * NS_MS, .matched = UINT32_MAX},
    };
    struct gw_metrics m;
    score_with(TWO_TRUTHS, 2u, d, &m);
    CHECK_EQ_INT(m.true_positives, 2);
    CHECK_EQ_INT(m.false_positives, 0);
    CHECK_EQ_INT(m.false_negatives, 0);
    CHECK_NEAR(m.precision, 1.0, 1e-9);
    CHECK_NEAR(m.recall, 1.0, 1e-9);
}

/* A perfect run over few trials must not claim certainty. Wilson's
 * interval for 2/2 starts well below 1, which is the whole reason the
 * report carries one. */
static void test_perfect_score_still_has_an_interval(void) {
    struct gw_detection d[] = {
        {.rule = 0u, .at_ns = 105000 * NS_MS, .matched = UINT32_MAX},
        {.rule = 0u, .at_ns = 205000 * NS_MS, .matched = UINT32_MAX},
    };
    struct gw_metrics m;
    score_with(TWO_TRUTHS, 2u, d, &m);
    CHECK(m.precision_lo < 0.4);
    CHECK_NEAR(m.precision_hi, 1.0, 1e-9);
}

/* The published Wilson interval for 10/10 at 95 % is [0.7225, 1.0]. */
static void test_wilson_matches_the_published_value(void) {
    char err[256] = "";
    char manifest[4096];
    int len = snprintf(manifest, sizeof manifest,
                       "version 1\nscene s\ncamera c\nmatch_window 10000\n"
                       "rule a appeared yes 1 0 15000\n");
    for (int i = 0; i < 10; i += 1) {
        len += snprintf(manifest + len, sizeof manifest - (size_t)len, "truth a x %d\n",
                        100000 + i * 100000);
    }
    (void)snprintf(manifest + len, sizeof manifest - (size_t)len, "sample 0 - a=no\n");
    CHECK_OK(gw_scene_parse(manifest, sizeof err, err, &g_scene));

    struct gw_detection d[10];
    for (int i = 0; i < 10; i += 1) {
        d[i] = (struct gw_detection){
            .rule = 0u, .at_ns = (100000 + i * 100000 + 1000) * NS_MS, .matched = UINT32_MAX};
    }
    struct gw_metrics m;
    CHECK_OK(gw_score(&g_scene, 10u, d, 0u, &m));
    CHECK_EQ_INT(m.true_positives, 10);
    CHECK_NEAR(m.recall_lo, 0.7225, 0.001);
    CHECK_NEAR(m.recall_hi, 1.0, 1e-9);
}

/* Time spent blind stays in the denominator. */
static void test_unknown_time_counts_against_availability(void) {
    char err[256] = "";
    /* Ten seconds confirmed, then a 60 s outage with no samples. */
    CHECK_OK(gw_scene_parse("version 1\nscene s\ncamera c\nmatch_window 1000\ntick_ms 1000\n"
                            "rule a appeared yes 2 2000 15000\n"
                            "sample 0 - a=yes\n"
                            "sample 2000 - a=yes\n"
                            "sample 10000 - a=yes\n"
                            "sample 70000 - a=yes\n",
                            sizeof err, err, &g_scene));
    struct gw_detection d[GW_BENCH_MAX_DETECTIONS];
    uint32_t n = 0u;
    CHECK_OK(gw_replay(&g_scene, GW_BENCH_MAX_DETECTIONS, d, &n));
    struct gw_metrics m;
    CHECK_OK(gw_score(&g_scene, n, d, 0u, &m));

    /* Exact, not "greater than zero". An earlier version of this test
     * asked only that some unknown time existed, and passed on the two
     * seconds before the first confirmation while silently counting the
     * entire 60-second outage as observed. Pinning the arithmetic is
     * what makes the regression visible:
     *
     *   0-1 s    unconfirmed at startup          2 s unknown
     *   2-25 s   confirmed, evidence good for
     *            15 s past the 10 s frame       24 s observed
     *   26-69 s  evidence expired               44 s unknown
     */
    CHECK_EQ_INT(m.observed_ns, 24000 * NS_MS);
    CHECK_EQ_INT(m.unknown_ns, 46000 * NS_MS);
    CHECK_NEAR(m.availability, 24.0 / 70.0, 1e-6);
}

/* The regression the synthetic scenes caught during development: if the
 * replay only ticks when a frame arrives, the outage is invisible and a
 * disarmed rule re-arms itself. */
static void test_replay_ticks_through_an_outage(void) {
    char err[256] = "";
    CHECK_OK(gw_scene_parse("version 1\nscene s\ncamera c\nmatch_window 45000\ntick_ms 1000\n"
                            "rule a appeared yes 3 10000 15000\n"
                            "sample 0 - a=no\n"
                            "sample 5000 - a=no\n"
                            "sample 10000 - a=no\n"
                            /* armed, then blind for a minute */
                            "sample 80000 - a=yes\n"
                            "sample 85000 - a=yes\n"
                            "sample 90000 - a=yes\n",
                            sizeof err, err, &g_scene));
    struct gw_detection d[GW_BENCH_MAX_DETECTIONS];
    uint32_t n = 0u;
    CHECK_OK(gw_replay(&g_scene, GW_BENCH_MAX_DETECTIONS, d, &n));
    CHECK_EQ_INT(n, 0); /* the outage disarmed it; no delivery is claimed */
}

/* --- restarts ------------------------------------------------------- */

static void test_restart_is_parsed_in_order(void) {
    char err[256] = "";
    CHECK_OK(gw_scene_parse("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                            "rule a appeared yes 1 0 15000\n"
                            "restart 5000\nrestart 9000\n"
                            "sample 0 - a=yes\n",
                            sizeof err, err, &g_scene));
    CHECK_EQ_INT(g_scene.restart_count, 2);
    CHECK_EQ_INT(g_scene.restarts[0], 5000 * NS_MS);
    expect_parse_error("version 1\nscene s\ncamera c\nmatch_window 1000\n"
                       "rule a appeared yes 1 0 15000\n"
                       "restart 9000\nrestart 5000\n"
                       "sample 0 - a=yes\n",
                       "restarts must be in time order");
}

/* A restart mid-scene discards the confirmed absence that armed the
 * rule, so the parcel the new process finds is an initial state rather
 * than a delivery. Without the restart the same label stream fires —
 * which is what makes this scene a test rather than a tautology. */
static void test_restart_disarms_the_rule(void) {
    static const char *WITH = "version 1\nscene s\ncamera c\nmatch_window 45000\ntick_ms 1000\n"
                              "rule a appeared yes 3 10000 15000\n"
                              "sample 0 - a=no\nsample 5000 - a=no\nsample 10000 - a=no\n"
                              "restart 12000\n"
                              "sample 15000 - a=yes\nsample 20000 - a=yes\nsample 25000 - a=yes\n";
    static const char *WITHOUT =
        "version 1\nscene s\ncamera c\nmatch_window 45000\ntick_ms 1000\n"
        "rule a appeared yes 3 10000 15000\n"
        "sample 0 - a=no\nsample 5000 - a=no\nsample 10000 - a=no\n"
        "sample 15000 - a=yes\nsample 20000 - a=yes\nsample 25000 - a=yes\n";
    char err[256] = "";
    struct gw_detection d[GW_BENCH_MAX_DETECTIONS];
    uint32_t n = 0u;

    CHECK_OK(gw_scene_parse(WITH, sizeof err, err, &g_scene));
    CHECK_OK(gw_replay(&g_scene, GW_BENCH_MAX_DETECTIONS, d, &n));
    CHECK_EQ_INT(n, 0);

    CHECK_OK(gw_scene_parse(WITHOUT, sizeof err, err, &g_scene));
    CHECK_OK(gw_replay(&g_scene, GW_BENCH_MAX_DETECTIONS, d, &n));
    CHECK_EQ_INT(n, 1);
}

int main(void) {
    RUN(test_minimal_manifest);
    RUN(test_omitted_label_is_not_unknown);
    RUN(test_parser_refuses_what_it_cannot_mean);
    RUN(test_tick_coarser_than_gap_is_refused);
    RUN(test_error_carries_a_line_number);
    RUN(test_matching_is_one_to_one);
    RUN(test_detection_outside_the_window_is_a_false_alarm);
    RUN(test_detection_before_the_event_does_not_count);
    RUN(test_both_events_answered);
    RUN(test_perfect_score_still_has_an_interval);
    RUN(test_wilson_matches_the_published_value);
    RUN(test_unknown_time_counts_against_availability);
    RUN(test_replay_ticks_through_an_outage);
    RUN(test_restart_is_parsed_in_order);
    RUN(test_restart_disarms_the_rule);
    return TEST_MAIN;
}
