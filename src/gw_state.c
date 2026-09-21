/*
 * gw_state.c — the confirmation engine.
 *
 * One machine drives both rules the plan specifies. An observation is
 * accepted or rejected, accumulated into a candidate, and promoted to
 * confirmed once it has enough agreeing observations spread over enough
 * time. What a rule does with a confirmed value is the only difference
 * between "a package appeared" and "the door has been open five minutes".
 *
 * Nothing here calls the clock, allocates, or touches the filesystem.
 */
#include "geist_watch.h"

#include <string.h>

/* Health degrades at max_gap and is called offline at this multiple of
 * it. The plan freezes real thresholds only after the first hardware
 * profile exists, so this is a placeholder with a name rather than a
 * measured value pretending to be one. */
enum { GW_OFFLINE_GAP_FACTOR = 10 };

const char *gw_status_str(const enum gw_status s) {
    switch (s) {
    case GW_OK:
        return "ok";
    case GW_E_INVALID_ARG:
        return "invalid_arg";
    case GW_E_LIMIT:
        return "limit";
    case GW_E_TIME:
        return "time_went_backwards";
    case GW_E_DUPLICATE:
        return "duplicate_frame";
    case GW_E_STALE:
        return "stale_observation";
    }
    return "unknown_status";
}

const char *gw_value_str(const enum gw_value v) {
    switch (v) {
    case GW_UNKNOWN:
        return "unknown";
    case GW_NO:
        return "no";
    case GW_YES:
        return "yes";
    }
    return "unknown";
}

static enum gw_value opposite_of(const enum gw_value v) {
    switch (v) {
    case GW_YES:
        return GW_NO;
    case GW_NO:
        return GW_YES;
    case GW_UNKNOWN:
        break;
    }
    return GW_UNKNOWN;
}

/* Copy a NUL-terminated id into a fixed field, refusing anything that
 * would be truncated. A silently shortened rule id would produce events
 * attributed to the wrong rule, so this fails instead. */
static enum gw_status copy_id(char dst[static GW_ID_CAP], const char *src) {
    if (src == nullptr) {
        return GW_E_INVALID_ARG;
    }
    const size_t n = strlen(src);
    if (n == 0u || n + 1u > (size_t)GW_ID_CAP) {
        return GW_E_LIMIT;
    }
    memcpy(dst, src, n + 1u);
    return GW_OK;
}

enum gw_status gw_init(struct gw_watch *w, const char *camera_id) {
    if (w == nullptr) {
        return GW_E_INVALID_ARG;
    }
    /* A restart is this function. Everything below — confirmed values,
     * arming, and any open sustained run — goes back to nothing, which
     * is what the plan means by "restarts inherit no duration". */
    *w = (struct gw_watch){};
    return copy_id(w->camera_id, camera_id);
}

enum gw_status gw_add_rule(struct gw_watch *w, const struct gw_rule_config *cfg,
                           uint32_t *out_rule) {
    if (w == nullptr || cfg == nullptr || out_rule == nullptr) {
        return GW_E_INVALID_ARG;
    }
    if (w->rule_count >= (uint32_t)GW_MAX_RULES) {
        return GW_E_LIMIT;
    }
    /* A rule that cannot ever confirm, or cannot ever fire, is a
     * configuration bug that would otherwise sit silently in a running
     * watch reporting "unknown" forever. */
    if (cfg->target != GW_YES && cfg->target != GW_NO) {
        return GW_E_INVALID_ARG;
    }
    if (cfg->confirm_count == 0u || cfg->confirm_window_ns < 0 || cfg->max_gap_ns <= 0) {
        return GW_E_INVALID_ARG;
    }
    switch (cfg->kind) {
    case GW_RULE_APPEARED:
        if (cfg->sustain_ns != 0) {
            return GW_E_INVALID_ARG;
        }
        break;
    case GW_RULE_SUSTAINED:
        if (cfg->sustain_ns <= 0) {
            return GW_E_INVALID_ARG;
        }
        break;
    default:
        return GW_E_INVALID_ARG;
    }

    struct gw_rule_runtime *r = &w->rules[w->rule_count];
    *r = (struct gw_rule_runtime){};
    r->cfg = *cfg;
    const enum gw_status s = copy_id(r->cfg.id, cfg->id);
    if (s != GW_OK) {
        return s;
    }
    /* Deliberately disarmed. A watch started while a package is already
     * on the doorstep will confirm GW_YES and record it as the initial
     * state; it must not announce a delivery it never saw happen. */
    r->armed = false;
    *out_rule = w->rule_count;
    w->rule_count += 1u;
    return GW_OK;
}

/* Drop accumulated evidence without touching the confirmed value. */
static void reset_candidate(struct gw_rule_runtime *r) {
    r->candidate = GW_UNKNOWN;
    r->candidate_count = 0u;
    r->candidate_first_ns = 0;
}

/* Break an open sustained run and let it fire again next time. */
static void break_run(struct gw_rule_runtime *r) {
    r->run_open = false;
    r->run_start_ns = 0;
    r->fired = false;
}

/* Move the confirmed value, and let the rule react to the edge. */
static void set_confirmed(struct gw_rule_runtime *r, const enum gw_value v, const int64_t at_ns) {
    if (r->confirmed == v) {
        return;
    }
    r->confirmed = v;

    switch (r->cfg.kind) {
    case GW_RULE_APPEARED:
        if (v == opposite_of(r->cfg.target)) {
            /* Confirmed absence is the ONLY thing that re-arms. A
             * cooldown would re-announce a package that simply stayed
             * where it was. */
            r->armed = true;
            r->fired = false;
        } else if (v == r->cfg.target && r->armed) {
            r->pending = true;
            r->fired = true;
            r->armed = false;
        }
        break;
    case GW_RULE_SUSTAINED:
        if (v == r->cfg.target) {
            r->run_open = true;
            r->run_start_ns = at_ns;
            r->fired = false;
        } else {
            break_run(r);
        }
        break;
    }
}

enum gw_status gw_observe(struct gw_watch *w, const uint32_t rule,
                          const struct gw_observation *obs) {
    if (w == nullptr || obs == nullptr || rule >= w->rule_count) {
        return GW_E_INVALID_ARG;
    }
    if (obs->value != GW_UNKNOWN && obs->value != GW_NO && obs->value != GW_YES) {
        return GW_E_INVALID_ARG;
    }
    struct gw_rule_runtime *r = &w->rules[rule];

    if (r->has_obs) {
        /* The same frame evaluated twice is one view of the world, not
         * two. Counting it again is how a stalled capture would talk a
         * rule into a confirmation it never earned. */
        if (obs->frame_id == r->last_frame_id) {
            return GW_E_DUPLICATE;
        }
        if (obs->observed_at_ns < r->last_obs_ns) {
            return GW_E_STALE;
        }
    }

    r->last_obs_ns = obs->observed_at_ns;
    r->last_frame_id = obs->frame_id;
    r->has_obs = true;

    if (r->candidate_count == 0u || obs->value != r->candidate) {
        r->candidate = obs->value;
        r->candidate_count = 1u;
        r->candidate_first_ns = obs->observed_at_ns;
    } else if (r->candidate_count < UINT32_MAX) {
        r->candidate_count += 1u;
    }

    /* A sustained run breaks on a single fresh contrary observation,
     * without waiting for it to be confirmed. The asymmetry is on
     * purpose: ending a five-minute claim early is cheap, extending one
     * through a closed door is not. */
    if (r->cfg.kind == GW_RULE_SUSTAINED && obs->value != r->cfg.target) {
        break_run(r);
    }

    if (r->candidate_count >= r->cfg.confirm_count &&
        obs->observed_at_ns - r->candidate_first_ns >= r->cfg.confirm_window_ns) {
        set_confirmed(r, r->candidate, obs->observed_at_ns);
    }
    return GW_OK;
}

/* True when the newest accepted observation is older than max_gap at
 * `now_ns`. Shared by the tick (which acts on it) and the state query
 * (which must report the same view without mutating). */
static bool evidence_expired(const struct gw_rule_runtime *r, const int64_t now_ns) {
    if (!r->has_obs) {
        return true;
    }
    return now_ns - r->last_obs_ns > r->cfg.max_gap_ns;
}

enum gw_status gw_tick(struct gw_watch *w, const int64_t now_ns, const uint32_t cap,
                       uint32_t *out_fired, uint32_t *out_n) {
    if (w == nullptr || (cap > 0u && out_fired == nullptr)) {
        return GW_E_INVALID_ARG;
    }
    if (w->has_ticked && now_ns < w->last_tick_ns) {
        /* The core is specified on a monotonic clock. Rather than
         * silently computing a negative duration, say so. */
        return GW_E_TIME;
    }

    uint32_t n = 0u;
    for (uint32_t i = 0u; i < w->rule_count; i += 1u) {
        struct gw_rule_runtime *r = &w->rules[i];

        if (evidence_expired(r, now_ns)) {
            if (r->confirmed != GW_UNKNOWN) {
                r->confirmed = GW_UNKNOWN;
            }
            reset_candidate(r);
            if (r->cfg.kind == GW_RULE_SUSTAINED) {
                break_run(r);
            } else {
                /* Losing sight disarms. Coming back to a package after a
                 * gap cannot be distinguished from watching one arrive,
                 * and the plan is explicit that an unclear arrival must
                 * not be reported as a delivery. The cost is a delivery
                 * missed inside the gap, which the plan accepts as a
                 * documented sampling limit. */
                r->armed = false;
            }
        } else if (r->cfg.kind == GW_RULE_SUSTAINED && r->run_open && !r->fired &&
                   now_ns - r->run_start_ns >= r->cfg.sustain_ns) {
            r->pending = true;
            r->fired = true;
        }

        if (r->pending) {
            r->pending = false;
            if (n < cap) {
                out_fired[n] = i;
            }
            n += 1u;
        }
    }

    w->last_tick_ns = now_ns;
    w->has_ticked = true;
    if (out_n != nullptr) {
        *out_n = n;
    }
    return (n > cap) ? GW_E_LIMIT : GW_OK;
}

enum gw_status gw_get_state(const struct gw_watch *w, const uint32_t rule, const int64_t now_ns,
                            struct gw_rule_state *out) {
    if (w == nullptr || out == nullptr || rule >= w->rule_count) {
        return GW_E_INVALID_ARG;
    }
    const struct gw_rule_runtime *r = &w->rules[rule];
    const bool expired = evidence_expired(r, now_ns);

    *out = (struct gw_rule_state){
        .confirmed = expired ? GW_UNKNOWN : r->confirmed,
        .age_ns = r->has_obs ? now_ns - r->last_obs_ns : INT64_MAX,
        .fired = r->fired,
    };

    if (!r->has_obs) {
        out->health = GW_OFFLINE;
    } else if (!expired) {
        out->health = GW_HEALTHY;
    } else if (out->age_ns <= r->cfg.max_gap_ns * (int64_t)GW_OFFLINE_GAP_FACTOR) {
        out->health = GW_DEGRADED;
    } else {
        out->health = GW_OFFLINE;
    }

    if (!expired && r->candidate == r->confirmed) {
        out->confirmations = r->candidate_count;
    }
    if (!expired && r->run_open && now_ns >= r->run_start_ns) {
        out->sustained_ns = now_ns - r->run_start_ns;
    }
    return GW_OK;
}
