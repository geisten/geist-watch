/*
 * report_check — grade a benchmark report against the frozen criteria.
 *
 *   report_check --criteria benchmarks/criteria/v0.1.criteria run.report
 *
 * Every criterion is printed, met or not: the plan asks for missed targets
 * to be reported visibly, and a tool that printed only failures would let a
 * shrinking test suite read as progress. Exits non-zero when anything is
 * unmet OR unmeasured, which are different words for the same conclusion —
 * the criterion is not demonstrated.
 */
#include "gw_report.h"

#include <stdio.h>
#include <string.h>

enum { FILE_CAP = 1u << 18 };

[[nodiscard]] static char *slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "report_check: cannot open %s\n", path);
        return nullptr;
    }
    const size_t n = fread(buf, 1u, cap - 1u, f);
    const bool too_big = !feof(f);
    (void)fclose(f);
    if (too_big) {
        fprintf(stderr, "report_check: %s is larger than %u bytes\n", path, (unsigned)cap - 1u);
        return nullptr;
    }
    buf[n] = '\0';
    return buf;
}

static const char *verdict_str(const enum gw_verdict v) {
    switch (v) {
    case GW_VERDICT_PASS:
        return "PASS";
    case GW_VERDICT_FAIL:
        return "FAIL";
    case GW_VERDICT_NOT_MEASURED:
        return "NOT MEASURED";
    }
    return "?";
}

int main(int argc, char **argv) {
    const char *criteria_path = nullptr;
    const char *report_path = nullptr;

    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--criteria") == 0 && i + 1 < argc) {
            i += 1;
            criteria_path = argv[i];
        } else if (strncmp(argv[i], "--", 2u) == 0) {
            fprintf(stderr, "report_check: unknown option %s\n", argv[i]);
            return 2;
        } else if (report_path == nullptr) {
            report_path = argv[i];
        } else {
            fprintf(stderr, "report_check: one report at a time\n");
            return 2;
        }
    }
    if (criteria_path == nullptr || report_path == nullptr) {
        fprintf(stderr, "usage: report_check --criteria <file> <report>\n");
        return 2;
    }

    static char ctext[FILE_CAP], rtext[FILE_CAP];
    static struct gw_criteria criteria;
    static struct gw_report report;
    char err[256] = "";

    if (slurp(criteria_path, ctext, sizeof ctext) == nullptr) {
        return 1;
    }
    if (gw_criteria_parse(ctext, sizeof err, err, &criteria) != GW_OK) {
        fprintf(stderr, "report_check: %s: %s\n", criteria_path, err);
        return 1;
    }
    if (slurp(report_path, rtext, sizeof rtext) == nullptr) {
        return 1;
    }
    if (gw_report_parse(rtext, sizeof err, err, &report) != GW_OK) {
        fprintf(stderr, "report_check: %s: %s\n", report_path, err);
        return 1;
    }

    static struct gw_grade grades[GW_REPORT_MAX_CRITERIA];
    uint32_t failed = 0u;
    if (gw_report_grade(&report, &criteria, sizeof err, err, grades, &failed) != GW_OK) {
        fprintf(stderr, "report_check: %s\n", err);
        return 1;
    }

    printf("profile %s\n", criteria.profile);
    for (int f = 0; f < GW_PROV_COUNT; f += 1) {
        printf("  %-20s %s\n", gw_provenance_key((enum gw_provenance_field)f),
               report.provenance[f]);
    }
    printf("\n");
    for (uint32_t i = 0u; i < criteria.count; i += 1u) {
        const struct gw_criterion *it = &criteria.items[i];
        const struct gw_grade *g = &grades[i];
        printf("  %-12s %-32s %s %.6g", verdict_str(g->verdict), it->key,
               it->cmp == GW_CMP_LE ? "<=" : ">=", it->limit);
        if (g->verdict != GW_VERDICT_NOT_MEASURED) {
            printf("  (measured %.6g)", g->measured);
        }
        if (it->about[0] != '\0') {
            printf("  — %s", it->about);
        }
        printf("\n");
    }
    printf("\n%u of %u criteria not demonstrated\n", failed, criteria.count);
    return failed == 0u ? 0 : 1;
}
