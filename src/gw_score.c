/*
 * gw_score.c — replay a scene, then score what came out of it.
 *
 * The scoring rules here are the ones the release criteria name, written
 * down in code so a published number can be re-derived rather than
 * trusted. Three of them are easy to get quietly wrong, so each is
 * implemented deliberately:
 *
 *   - Matching is one-to-one. A single detection cannot satisfy two
 *     ground-truth events, and two detections for one event make the
 *     second a false alarm rather than a second success.
 *   - Unknown phases stay in the denominator. Availability is reported
 *     next to precision and recall, because a run that spent half the
 *     scene blind and got the visible half right is not a perfect run.
 *   - Point estimates travel with an interval. On fifty sequences the
 *     difference between 92 % and 98 % is noise, and a bare percentage
 *     hides that.
 */
#include "gw_bench.h"

#include <math.h>

/* One walk of the scene, used for BOTH detections and time accounting.
 *
 * They were two loops once, and the second one silently ticked only at
 * sample timestamps — so a camera outage counted as fully observed and
 * availability came out at 0.97 where 0.21 was right. Two traversals of
 * the same timeline are two chances to disagree about it; there is now
 * one, and the callers differ only in what they ask it to record.
 *
 * `avail_rule` is the rule whose confirmed/unknown time is accumulated
 * into *observed_ns and *unknown_ns; pass nullptr for both to skip it. */
static enum gw_status walk(const struct gw_scene *scene, const uint32_t cap,
                           struct gw_detection *out, uint32_t *out_n, const uint32_t avail_rule,
                           int64_t *observed_ns, int64_t *unknown_ns) {
    struct gw_watch w;
    enum gw_status s = gw_init(&w, scene->camera);
    if (s != GW_OK) {
        return s;
    }
    for (uint32_t i = 0u; i < scene->rule_count; i += 1u) {
        uint32_t idx = 0u;
        s = gw_add_rule(&w, &scene->rules[i], &idx);
        if (s != GW_OK) {
            return s;
        }
    }

    /* Walk the scene on a fixed tick cadence and deliver samples as
     * their timestamps come due — rather than ticking once per sample.
     *
     * The difference is not cosmetic. Ticking only when a frame arrives
     * means the core is never asked about time in which no frame
     * arrived, so evidence expiry can never fire and a camera outage
     * becomes invisible: the first frame after the gap lands before the
     * tick that should have noticed the gap. The product loop ticks on
     * a scheduler regardless of capture, and the replay has to as well
     * or it measures a system nobody ships. */
    uint32_t n = 0u;
    uint32_t next = 0u;
    for (int64_t now = scene->samples[0].t_ns;; now += scene->tick_ns) {
        while (next < scene->sample_count && scene->samples[next].t_ns <= now) {
            const struct gw_sample *smp = &scene->samples[next];
            for (uint32_t r = 0u; r < scene->rule_count; r += 1u) {
                if (!smp->has_label[r]) {
                    continue;
                }
                const struct gw_observation o = {
                    /* Per-scene and strictly increasing, so a sample is
                     * never mistaken for a replay of an earlier one.
                     * Samples sharing a timestamp stay distinct frames. */
                    .frame_id = (uint64_t)next + 1u,
                    .observed_at_ns = smp->t_ns,
                    .value = smp->label[r],
                };
                const enum gw_status os = gw_observe(&w, r, &o);
                if (os != GW_OK && os != GW_E_DUPLICATE && os != GW_E_STALE) {
                    return os;
                }
            }
            next += 1u;
        }

        uint32_t fired[GW_MAX_RULES];
        uint32_t fired_n = 0u;
        s = gw_tick(&w, now, GW_MAX_RULES, fired, &fired_n);
        if (s != GW_OK) {
            return s;
        }
        for (uint32_t f = 0u; f < fired_n; f += 1u) {
            if (n < cap) {
                out[n] =
                    (struct gw_detection){.rule = fired[f], .at_ns = now, .matched = UINT32_MAX};
            }
            n += 1u;
        }

        const bool done =
            next >= scene->sample_count && now >= scene->samples[scene->sample_count - 1u].t_ns;

        /* Attribute this tick's interval. Measured at the cadence rather
         * than per sample, so the time between a lost camera and its
         * return lands in the unknown column where it belongs. */
        if (observed_ns != nullptr && unknown_ns != nullptr && !done) {
            struct gw_rule_state st;
            if (gw_get_state(&w, avail_rule, now, &st) == GW_OK) {
                if (st.confirmed == GW_UNKNOWN) {
                    *unknown_ns += scene->tick_ns;
                } else {
                    *observed_ns += scene->tick_ns;
                }
            }
        }

        if (done) {
            break;
        }
    }

    if (out_n != nullptr) {
        *out_n = n;
    }
    return (n > cap) ? GW_E_LIMIT : GW_OK;
}

enum gw_status gw_replay(const struct gw_scene *scene, const uint32_t cap, struct gw_detection *out,
                         uint32_t *out_n) {
    if (scene == nullptr || out_n == nullptr || (cap > 0u && out == nullptr)) {
        return GW_E_INVALID_ARG;
    }
    return walk(scene, cap, out, out_n, 0u, nullptr, nullptr);
}

/* Wilson score interval, the standard choice for a proportion from few
 * trials. The normal approximation would put the upper bound of a
 * perfect 12/12 at exactly 1.0 and its lower bound at 1.0 as well, which
 * claims certainty no twelve samples can support. */
static void wilson(const uint32_t k, const uint32_t n, double *p, double *lo, double *hi) {
    if (n == 0u) {
        *p = 0.0;
        *lo = 0.0;
        *hi = 0.0;
        return;
    }
    const double z = 1.959963985; /* 95 % */
    const double nn = (double)n;
    const double phat = (double)k / nn;
    const double denom = 1.0 + z * z / nn;
    const double centre = (phat + z * z / (2.0 * nn)) / denom;
    const double half = (z / denom) * sqrt(phat * (1.0 - phat) / nn + z * z / (4.0 * nn * nn));

    *p = phat;
    *lo = centre - half < 0.0 ? 0.0 : centre - half;
    *hi = centre + half > 1.0 ? 1.0 : centre + half;
}

/* Time the rule held a confirmed value, and time it did not, over the
 * same walk the detections come from. */
static void availability(const struct gw_scene *scene, const uint32_t rule, struct gw_metrics *m) {
    struct gw_detection discard[1];
    (void)walk(scene, 0u, discard, nullptr, rule, &m->observed_ns, &m->unknown_ns);
    const int64_t total = m->observed_ns + m->unknown_ns;
    m->availability = (total > 0) ? (double)m->observed_ns / (double)total : 0.0;
}

enum gw_status gw_score(const struct gw_scene *scene, const uint32_t n_det,
                        struct gw_detection *detections, const uint32_t rule,
                        struct gw_metrics *out) {
    if (scene == nullptr || out == nullptr || (n_det > 0u && detections == nullptr) ||
        rule >= scene->rule_count) {
        return GW_E_INVALID_ARG;
    }
    *out = (struct gw_metrics){};

    bool truth_taken[GW_BENCH_MAX_TRUTH] = {};

    /* Detections arrive in time order from gw_replay, and truth is
     * required to be in time order by the parser, so one forward pass
     * implements "each detection takes the earliest still-unmatched
     * event whose window contains it". */
    for (uint32_t d = 0u; d < n_det; d += 1u) {
        if (detections[d].rule != rule) {
            continue;
        }
        detections[d].matched = UINT32_MAX;
        for (uint32_t t = 0u; t < scene->truth_count; t += 1u) {
            if (truth_taken[t] || scene->truth[t].rule != rule) {
                continue;
            }
            const int64_t at = scene->truth[t].at_ns;
            if (detections[d].at_ns < at - scene->truth[t].before_ns) {
                /* Truth is ordered, so every later event is further
                 * away still: this detection matches nothing. */
                break;
            }
            if (detections[d].at_ns <= at + scene->truth[t].after_ns) {
                truth_taken[t] = true;
                detections[d].matched = t;
                out->true_positives += 1u;
                break;
            }
        }
        if (detections[d].matched == UINT32_MAX) {
            out->false_positives += 1u;
        }
    }

    for (uint32_t t = 0u; t < scene->truth_count; t += 1u) {
        if (scene->truth[t].rule == rule && !truth_taken[t]) {
            out->false_negatives += 1u;
        }
    }

    wilson(out->true_positives, out->true_positives + out->false_positives, &out->precision,
           &out->precision_lo, &out->precision_hi);
    wilson(out->true_positives, out->true_positives + out->false_negatives, &out->recall,
           &out->recall_lo, &out->recall_hi);

    availability(scene, rule, out);
    return GW_OK;
}
