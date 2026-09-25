/*
 * Benchmark report parsing and grading.
 *
 * The cases that matter are the ones where a gate could quietly pass. A
 * criterion nobody measured, a report nobody can attribute to a build, a
 * limit read through the wrong locale — each produces a green verdict about
 * nothing, and a green verdict about nothing is worse than a red one,
 * because it is filed as evidence.
 */
#include "gw_report.h"
#include "test_support.h"

#include <string.h>

static struct gw_report g_report;
static struct gw_criteria g_criteria;

/* A report with every required provenance field, so individual tests can
 * vary one thing without tripping the provenance gate. */
static const char *FULL_PROVENANCE =
    "version 1\n"
    "model_id            smolvlm-500m\n"
    "model_sha256        0000000000000000000000000000000000000000000000000000000000000000\n"
    "model_quantization  decoder=Q4_K_M tower=F16\n"
    "engine_commit       f2fcd33211f0a7fe064fbdf55ac44717f1cbfec5\n"
    "compiler            gcc-14.2.0\n"
    "compiler_flags      -O2 -mcpu=cortex-a76\n"
    "os                  Debian GNU/Linux 13 aarch64\n"
    "hardware            Raspberry Pi 5 4GB\n"
    "source              cam-front\n"
    "roi                 0.15,0.35,0.70,0.60\n"
    "resolution          1280x720\n"
    "sampling            2fps capture, 8s semantic\n"
    "prompt_id           doorstep-package\n"
    "max_tokens          4\n"
    "start               cold\n";

static const char *CRITERIA = "version 1\n"
                              "profile test\n"
                              "require peak_rss_bytes    <= 1610612736  memory ceiling\n"
                              "require package_precision >= 0.95        on held-out scenes\n";

static void expect_report_error(const char *text, const char *needle) {
    char err[256] = "";
    if (gw_report_parse(text, sizeof err, err, &g_report) == GW_OK) {
        fprintf(stderr, "  expected a parse error mentioning '%s'\n", needle);
        gw_test_failures += 1;
        return;
    }
    if (strstr(err, needle) == nullptr) {
        fprintf(stderr, "  error was '%s', expected it to mention '%s'\n", err, needle);
        gw_test_failures += 1;
    }
}

/* --- parsing -------------------------------------------------------- */

static void test_a_full_report_parses(void) {
    char err[256] = "";
    static char text[4096];
    (void)snprintf(text, sizeof text, "%smetric peak_rss_bytes 1288490188\n", FULL_PROVENANCE);
    CHECK_OK(gw_report_parse(text, sizeof err, err, &g_report));
    CHECK_EQ_INT(g_report.metric_count, 1);
    CHECK(strcmp(g_report.metrics[0].key, "peak_rss_bytes") == 0);
    CHECK_NEAR(g_report.metrics[0].value, 1288490188.0, 0.5);
    /* Values with spaces survive whole: tokenising compiler_flags would
     * lose exactly what makes a run identifiable. */
    CHECK(strcmp(g_report.provenance[GW_PROV_COMPILER_FLAGS], "-O2 -mcpu=cortex-a76") == 0);
    CHECK(strcmp(g_report.provenance[GW_PROV_OS], "Debian GNU/Linux 13 aarch64") == 0);
}

static void test_report_refuses_what_it_cannot_mean(void) {
    expect_report_error("model_id x\n", "version 1");
    expect_report_error("version 2\n", "version must be 1");
    expect_report_error("version 1\nnonsense x\n", "unknown key 'nonsense'");
    expect_report_error("version 1\nmodel_id\n", "needs a value");
    expect_report_error("version 1\nmodel_id a\nmodel_id b\n", "given twice");
    expect_report_error("version 1\nmetric a\n", "needs a decimal value");
    expect_report_error("version 1\nmetric a 1\nmetric a 2\n", "reported twice");
    expect_report_error("version 1\nmetric a 1,5\n", "needs a decimal value");
}

/* A decimal comma must not be read as a number. strtod in a de_DE locale
 * would turn "0.95" into 0, and a precision floor of 0 passes everything. */
static void test_numbers_are_locale_free(void) {
    char err[256] = "";
    static char text[4096];
    (void)snprintf(text, sizeof text, "%smetric package_precision 0.95\n", FULL_PROVENANCE);
    CHECK_OK(gw_report_parse(text, sizeof err, err, &g_report));
    CHECK_NEAR(g_report.metrics[0].value, 0.95, 1e-9);
}

static void test_criteria_parse_and_refusals(void) {
    char err[256] = "";
    CHECK_OK(gw_criteria_parse(CRITERIA, sizeof err, err, &g_criteria));
    CHECK_EQ_INT(g_criteria.count, 2);
    CHECK_EQ_INT(g_criteria.items[0].cmp, GW_CMP_LE);
    CHECK_EQ_INT(g_criteria.items[1].cmp, GW_CMP_GE);
    CHECK(strcmp(g_criteria.items[1].about, "on held-out scenes") == 0);

    /* A gate that requires nothing passes everything. */
    CHECK(gw_criteria_parse("version 1\nprofile p\n", sizeof err, err, &g_criteria) != GW_OK);
    CHECK(strstr(err, "gates nothing") != nullptr);

    CHECK(gw_criteria_parse("version 1\nrequire a < 1\n", sizeof err, err, &g_criteria) != GW_OK);
    CHECK(strstr(err, "<= or >=") != nullptr);
    CHECK(gw_criteria_parse("version 1\nrequire a <= x\n", sizeof err, err, &g_criteria) != GW_OK);
    CHECK(strstr(err, "must be a decimal") != nullptr);
}

/* --- grading -------------------------------------------------------- */

static uint32_t grade(const char *metrics, struct gw_grade *out, char *err, size_t err_cap) {
    static char text[4096];
    (void)snprintf(text, sizeof text, "%s%s", FULL_PROVENANCE, metrics);
    CHECK_OK(gw_report_parse(text, err_cap, err, &g_report));
    CHECK_OK(gw_criteria_parse(CRITERIA, err_cap, err, &g_criteria));
    uint32_t failed = 0u;
    CHECK_OK(gw_report_grade(&g_report, &g_criteria, err_cap, err, out, &failed));
    return failed;
}

static void test_a_met_criterion_passes(void) {
    char err[256] = "";
    struct gw_grade g[GW_REPORT_MAX_CRITERIA];
    const uint32_t failed = grade(
        "metric peak_rss_bytes 1288490188\nmetric package_precision 0.97\n", g, err, sizeof err);
    CHECK_EQ_INT(failed, 0);
    CHECK_EQ_INT(g[0].verdict, GW_VERDICT_PASS);
    CHECK_EQ_INT(g[1].verdict, GW_VERDICT_PASS);
}

static void test_a_missed_criterion_fails_and_reports_the_value(void) {
    char err[256] = "";
    struct gw_grade g[GW_REPORT_MAX_CRITERIA];
    const uint32_t failed = grade(
        "metric peak_rss_bytes 2000000000\nmetric package_precision 0.90\n", g, err, sizeof err);
    CHECK_EQ_INT(failed, 2);
    CHECK_EQ_INT(g[0].verdict, GW_VERDICT_FAIL);
    CHECK_NEAR(g[0].measured, 2000000000.0, 0.5);
    CHECK_EQ_INT(g[1].verdict, GW_VERDICT_FAIL);
}

/* The boundary is inclusive on both sides: a criterion written "<= 1.5 GiB"
 * is met at exactly 1.5 GiB, and a reader should not have to guess. */
static void test_the_boundary_is_inclusive(void) {
    char err[256] = "";
    struct gw_grade g[GW_REPORT_MAX_CRITERIA];
    const uint32_t failed = grade(
        "metric peak_rss_bytes 1610612736\nmetric package_precision 0.95\n", g, err, sizeof err);
    CHECK_EQ_INT(failed, 0);
}

/* The one that matters most: the cheapest way through a gate is to not run
 * the case. Unmeasured must never render as passed. */
static void test_an_unmeasured_criterion_is_not_a_pass(void) {
    char err[256] = "";
    struct gw_grade g[GW_REPORT_MAX_CRITERIA];
    const uint32_t failed = grade("metric peak_rss_bytes 1000\n", g, err, sizeof err);
    CHECK_EQ_INT(failed, 1);
    CHECK_EQ_INT(g[0].verdict, GW_VERDICT_PASS);
    CHECK_EQ_INT(g[1].verdict, GW_VERDICT_NOT_MEASURED);
}

/* A verdict about a run nobody can identify looks like evidence and is not.
 * Each required field, removed in turn, must stop the grade. */
static void test_missing_provenance_refuses_the_grade(void) {
    for (int f = 0; f < GW_PROV_COUNT; f += 1) {
        static char text[4096];
        size_t n = 0u;
        n += (size_t)snprintf(text + n, sizeof text - n, "version 1\n");
        for (int k = 0; k < GW_PROV_COUNT; k += 1) {
            if (k == f) {
                continue;
            }
            n += (size_t)snprintf(text + n, sizeof text - n, "%s placeholder\n",
                                  gw_provenance_key((enum gw_provenance_field)k));
        }
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "metric peak_rss_bytes 1\nmetric package_precision 1\n");

        char err[256] = "";
        CHECK_OK(gw_report_parse(text, sizeof err, err, &g_report));
        CHECK_OK(gw_criteria_parse(CRITERIA, sizeof err, err, &g_criteria));
        struct gw_grade g[GW_REPORT_MAX_CRITERIA];
        uint32_t failed = 0u;
        err[0] = '\0';
        if (gw_report_grade(&g_report, &g_criteria, sizeof err, err, g, &failed) == GW_OK) {
            fprintf(stderr, "  grading succeeded without '%s'\n",
                    gw_provenance_key((enum gw_provenance_field)f));
            gw_test_failures += 1;
            continue;
        }
        if (strstr(err, gw_provenance_key((enum gw_provenance_field)f)) == nullptr) {
            fprintf(stderr, "  refusal '%s' does not name the missing field '%s'\n", err,
                    gw_provenance_key((enum gw_provenance_field)f));
            gw_test_failures += 1;
        }
    }
}

int main(void) {
    RUN(test_a_full_report_parses);
    RUN(test_report_refuses_what_it_cannot_mean);
    RUN(test_numbers_are_locale_free);
    RUN(test_criteria_parse_and_refusals);
    RUN(test_a_met_criterion_passes);
    RUN(test_a_missed_criterion_fails_and_reports_the_value);
    RUN(test_the_boundary_is_inclusive);
    RUN(test_an_unmeasured_criterion_is_not_a_pass);
    RUN(test_missing_provenance_refuses_the_grade);
    return TEST_MAIN;
}
