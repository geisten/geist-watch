/*
 * replay — drive scenes through the core and report what it found.
 *
 * Deterministic by construction: the core takes its clock from the
 * sample timestamps, so the same manifest always produces the same
 * numbers on every machine. That is what makes a published result
 * re-derivable rather than something to be taken on trust.
 *
 *   replay [--json] [--min-precision P] [--min-recall R] scene...
 *
 * Exits non-zero when a floor is given and missed, so a regression gate
 * is one flag rather than a script that parses this output.
 */
#include "gw_bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FILE_CAP = 1u << 20 };

static const char *split_name(const enum gw_split s) {
    return s == GW_SPLIT_TEST ? "test" : "dev";
}

[[nodiscard]] static char *slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return nullptr;
    }
    const size_t n = fread(buf, 1u, cap - 1u, f);
    const bool   too_big = !feof(f);
    (void)fclose(f);
    if (too_big) {
        fprintf(stderr, "replay: %s is larger than %u bytes\n", path, (unsigned)cap - 1u);
        return nullptr;
    }
    buf[n] = '\0';
    return buf;
}

/* A rule with nothing to measure against is not a rule that scored zero.
 * Printing 0.000 for both would read as total failure, which is exactly
 * backwards for a negative scene: there, emitting nothing is the pass. */
static bool measured(const struct gw_metrics *m) {
    return (m->true_positives + m->false_positives + m->false_negatives) > 0u;
}

static void report_human(const struct gw_scene *sc, uint32_t rule, const struct gw_metrics *m) {
    printf("  rule %-18s TP %-3u FP %-3u FN %-3u", sc->rules[rule].id, m->true_positives,
           m->false_positives, m->false_negatives);
    if (measured(m)) {
        printf("  precision %.3f [%.3f-%.3f]", m->precision, m->precision_lo, m->precision_hi);
        printf("  recall %.3f [%.3f-%.3f]", m->recall, m->recall_lo, m->recall_hi);
    } else {
        printf("  %-46s", "(negative scene: no events expected, none emitted)");
    }
    printf("  availability %.3f\n", m->availability);
}

static void report_json(const struct gw_scene *sc, uint32_t rule, const struct gw_metrics *m) {
    printf("{\"scene\":\"%s\",\"split\":\"%s\",\"rule\":\"%s\"", sc->name, split_name(sc->split),
           sc->rules[rule].id);
    printf(",\"measured\":%s", measured(m) ? "true" : "false");
    printf(",\"tp\":%u,\"fp\":%u,\"fn\":%u", m->true_positives, m->false_positives,
           m->false_negatives);
    printf(",\"precision\":%.6f,\"precision_lo\":%.6f,\"precision_hi\":%.6f", m->precision,
           m->precision_lo, m->precision_hi);
    printf(",\"recall\":%.6f,\"recall_lo\":%.6f,\"recall_hi\":%.6f", m->recall, m->recall_lo,
           m->recall_hi);
    printf(",\"availability\":%.6f,\"observed_ms\":%lld,\"unknown_ms\":%lld}\n", m->availability,
           (long long)(m->observed_ns / 1000000), (long long)(m->unknown_ns / 1000000));
}

int main(int argc, char **argv) {
    bool   json = false;
    double min_precision = -1.0, min_recall = -1.0;
    int    first = 1;

    for (; first < argc; first += 1) {
        if (strcmp(argv[first], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[first], "--min-precision") == 0 && first + 1 < argc) {
            first += 1;
            min_precision = atof(argv[first]);
        } else if (strcmp(argv[first], "--min-recall") == 0 && first + 1 < argc) {
            first += 1;
            min_recall = atof(argv[first]);
        } else if (strncmp(argv[first], "--", 2u) == 0) {
            fprintf(stderr, "replay: unknown option %s\n", argv[first]);
            return 2;
        } else {
            break;
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: replay [--json] [--min-precision P] [--min-recall R] scene...\n");
        return 2;
    }

    static char            text[FILE_CAP];
    static struct gw_scene scene;
    static struct gw_detection dets[GW_BENCH_MAX_DETECTIONS];
    int                        failures = 0;

    for (int i = first; i < argc; i += 1) {
        if (slurp(argv[i], text, sizeof text) == nullptr) {
            return 1;
        }
        char                 err[256];
        const enum gw_status ps = gw_scene_parse(text, sizeof err, err, &scene);
        if (ps != GW_OK) {
            fprintf(stderr, "replay: %s: %s\n", argv[i], err);
            return 1;
        }

        uint32_t             n = 0u;
        const enum gw_status rs = gw_replay(&scene, GW_BENCH_MAX_DETECTIONS, dets, &n);
        if (rs != GW_OK) {
            fprintf(stderr, "replay: %s: %s\n", argv[i], gw_status_str(rs));
            return 1;
        }

        if (!json) {
            printf("%s (%s, %u samples, %u truth events)\n", scene.name, split_name(scene.split),
                   scene.sample_count, scene.truth_count);
        }
        for (uint32_t r = 0u; r < scene.rule_count; r += 1u) {
            struct gw_metrics m;
            if (gw_score(&scene, n, dets, r, &m) != GW_OK) {
                fprintf(stderr, "replay: scoring failed for rule %u\n", r);
                return 1;
            }
            if (json) {
                report_json(&scene, r, &m);
            } else {
                report_human(&scene, r, &m);
            }
            /* Floors apply only where there is something to measure. A
             * negative scene that stays silent has nothing to fail; one
             * that fires has a false positive, which makes it measured
             * and drives precision to zero — so the floor catches it. */
            const bool has_numbers = measured(&m);
            if (has_numbers && min_precision >= 0.0 && m.precision < min_precision) {
                fprintf(stderr, "replay: %s/%s precision %.3f below floor %.3f\n", scene.name,
                        scene.rules[r].id, m.precision, min_precision);
                failures += 1;
            }
            if (has_numbers && min_recall >= 0.0 && m.recall < min_recall) {
                fprintf(stderr, "replay: %s/%s recall %.3f below floor %.3f\n", scene.name,
                        scene.rules[r].id, m.recall, min_recall);
                failures += 1;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
