/*
 * geist_watch.h — model-free temporal core for local visual events.
 *
 * This header is the whole of libgeist_watch.a. It turns a stream of
 * tri-state observations ("is a package visible?", "is the door open?")
 * into confirmed states and events, and it does so without knowing
 * anything about cameras, models or files.
 *
 * Three properties make that possible, and all three are load-bearing:
 *
 *   1. The caller owns every byte. `struct gw_watch` is a complete value
 *      with no pointers into anything the core allocated; there is no
 *      malloc in this library. Persisting a watch is writing the struct,
 *      restarting is zeroing it.
 *
 *   2. The clock is an argument, never a syscall. Every entry point that
 *      needs "now" is given it. The five-minute door rule is therefore
 *      testable in microseconds, which is the only reason the release
 *      criteria can ask for a 299/300-second boundary test at all.
 *
 *   3. `unknown` is a value, not an error. A covered lens, a stale frame
 *      and a malformed model reply all produce GW_UNKNOWN, and the rules
 *      treat it as "evidence stopped", never as "nothing changed".
 *
 * Time is int64_t nanoseconds on a monotonic clock. The core compares and
 * subtracts these and nothing else — it never formats them, and it never
 * asks what wall-clock instant they correspond to. UTC belongs to the
 * event log, which is a separate concern (gw_event.h).
 */
#ifndef GEIST_WATCH_H
#define GEIST_WATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Limits and status                                                    */
/* ------------------------------------------------------------------ */

enum {
    /* Rules per watch. A doorway scene in the plan needs two; the cap is
     * generous enough for one camera's worth and small enough that the
     * whole watch stays a few hundred bytes. */
    GW_MAX_RULES = 8,
    /* Rule and camera identifiers, including the terminator. */
    GW_ID_CAP = 32,
};

enum gw_status {
    GW_OK = 0,
    GW_E_INVALID_ARG, /* null pointer, unknown rule, or a malformed config */
    GW_E_LIMIT,       /* GW_MAX_RULES exceeded, or an id that does not fit */
    GW_E_TIME,        /* time moved backwards on a monotonic clock */
    GW_E_DUPLICATE,   /* this frame was already counted for this rule */
    GW_E_STALE,       /* observation older than one already accepted */
};

/* Human-readable form of a status, for diagnostics. Never null. */
const char *gw_status_str(enum gw_status s);

/* ------------------------------------------------------------------ */
/* Observations                                                         */
/* ------------------------------------------------------------------ */

/* The entire vocabulary a model may contribute. Free caption text is
 * deliberately not representable: the plan forbids it as a decision
 * input, so the type forbids it too. */
enum gw_value {
    GW_UNKNOWN = 0, /* zero, so a zeroed watch starts out knowing nothing */
    GW_NO,
    GW_YES,
};

const char *gw_value_str(enum gw_value v);

/* One answer about one region at one instant.
 *
 * `frame_id` identifies the captured frame the answer came from. It exists
 * because re-running the model over a frame already counted would let a
 * stalled capture manufacture confirmations out of one image — the plan's
 * "repeated evaluation of the same frame never counts as additional
 * evidence". Any strictly increasing per-camera counter works; the core
 * only ever compares it for equality with the last one it accepted.
 *
 * `observed_at_ns` is when the FRAME was captured, not when the model
 * finished. Confirmation windows measure the world, not the inference
 * queue. */
struct gw_observation {
    uint64_t frame_id;
    int64_t observed_at_ns;
    enum gw_value value;
};

/* ------------------------------------------------------------------ */
/* Rules                                                                */
/* ------------------------------------------------------------------ */

/* Both rules the plan specifies are the same machine with different
 * triggers, so the core implements one confirmation engine and lets the
 * rule choose when it fires.
 *
 * GW_RULE_APPEARED — fires once when the confirmed state reaches
 *   `target` after having been confirmed at its opposite. "Package left
 *   at the door": three positives over at least ten seconds, one event,
 *   and no second event until absence is confirmed again. A cooldown is
 *   explicitly NOT how this re-arms — a cooldown re-reports a package
 *   that simply stayed there.
 *
 * GW_RULE_SUSTAINED — fires once when the confirmed state has held at
 *   `target` continuously for `sustain_ns`. "Door open for five minutes".
 */
enum gw_rule_kind {
    GW_RULE_APPEARED = 0,
    GW_RULE_SUSTAINED,
};

struct gw_rule_config {
    char id[GW_ID_CAP]; /* stable identity, appears in events */
    enum gw_rule_kind kind;
    enum gw_value target; /* GW_YES or GW_NO; GW_UNKNOWN is rejected */

    /* Confirmation: `confirm_count` observations agreeing on a value,
     * spanning at least `confirm_window_ns`, promote it to confirmed.
     * Both bounds matter — three frames 200 ms apart are three views of
     * one moment, not three views of a state. */
    uint32_t confirm_count;
    int64_t confirm_window_ns;

    /* Evidence expiry. When the newest accepted observation is older
     * than this, the confirmed state falls back to GW_UNKNOWN even
     * though no contrary observation arrived. This is what makes a dead
     * camera different from an unchanging scene. */
    int64_t max_gap_ns;

    /* GW_RULE_SUSTAINED only: how long `target` must hold. Ignored by
     * GW_RULE_APPEARED, which must leave it zero. */
    int64_t sustain_ns;
};

/* ------------------------------------------------------------------ */
/* Watch state                                                          */
/* ------------------------------------------------------------------ */

enum gw_health {
    GW_HEALTHY = 0, /* a fresh observation within max_gap */
    GW_DEGRADED,    /* evidence expired, but the rule is still being driven */
    GW_OFFLINE,     /* nothing has arrived for a long multiple of max_gap */
};

/* What a rule currently believes, and how sure it is allowed to sound.
 * Returned by value: the caller cannot hold a pointer into the watch and
 * then be surprised by the next tick. */
struct gw_rule_state {
    enum gw_value confirmed;
    enum gw_health health;
    /* Age of the newest accepted observation at the time of the query,
     * or INT64_MAX when none has ever arrived. Reported so a consumer can
     * show "as of N seconds ago" instead of implying live knowledge. */
    int64_t age_ns;
    /* Observations backing the current confirmed value. Events carry this
     * so a reader can tell a three-frame confirmation from a lucky one. */
    uint32_t confirmations;
    /* GW_RULE_SUSTAINED: how long the target has held continuously, or 0
     * when the run is broken. Never carried across a restart. */
    int64_t sustained_ns;
    /* True once this rule has fired and not yet re-armed. */
    bool fired;
};

struct gw_rule_runtime {
    struct gw_rule_config cfg;

    enum gw_value confirmed;
    enum gw_value candidate;
    uint32_t candidate_count;
    int64_t candidate_first_ns;

    int64_t last_obs_ns;
    uint64_t last_frame_id;
    bool has_obs;

    /* GW_RULE_APPEARED: an event may fire only from an armed rule, and a
     * rule arms only by confirming the OPPOSITE of its target. A watch
     * that starts up looking at a package already on the doorstep
     * confirms GW_YES while disarmed, so it records a state and invents
     * no delivery. */
    bool armed;
    bool fired;

    /* An event becomes due the moment its condition is met, which for an
     * appearance is inside gw_observe. It is latched here and drained by
     * the next gw_tick, so a caller collects events from one place
     * instead of two. */
    bool pending;

    /* GW_RULE_SUSTAINED: when the current unbroken run at `target` began.
     * Cleared by any accepted observation that is not `target`, and by
     * evidence expiry. */
    int64_t run_start_ns;
    bool run_open;
};

struct gw_watch {
    char camera_id[GW_ID_CAP];
    struct gw_rule_runtime rules[GW_MAX_RULES];
    uint32_t rule_count;
    int64_t last_tick_ns;
    bool has_ticked;
};

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Zero the watch and set its camera. Calling this on a live watch is how
 * a restart is expressed: every rule returns to GW_UNKNOWN, disarmed,
 * with no sustained duration. The plan requires exactly that — a restart
 * must not inherit a monotonic duration from an earlier process. */
[[nodiscard]] enum gw_status gw_init(struct gw_watch *w, const char *camera_id);

/* Append a rule. Returns GW_E_LIMIT past GW_MAX_RULES or on an id that
 * does not fit, GW_E_INVALID_ARG on a configuration that cannot confirm
 * anything (zero confirm_count, a GW_UNKNOWN target, a sustained rule
 * with no duration, an appeared rule with one). On success writes the
 * rule's index through `out_rule`. */
[[nodiscard]] enum gw_status gw_add_rule(struct gw_watch *w, const struct gw_rule_config *cfg,
                                         uint32_t *out_rule);

/* Feed one observation to one rule.
 *
 * Rejects, without changing any state:
 *   GW_E_DUPLICATE  the frame already counted for this rule,
 *   GW_E_STALE      an observation older than the newest accepted one.
 *
 * Neither is a failure of the caller so much as a fact worth recording:
 * both mean the evidence did not grow, and a caller that treats them as
 * success would over-count. */
[[nodiscard]] enum gw_status gw_observe(struct gw_watch *w, uint32_t rule,
                                        const struct gw_observation *obs);

/* Advance time without new observations, and collect any events that
 * become due. This is where evidence expires and where a sustained rule
 * fires: a door that has been open for five minutes produces its event
 * from the passage of time, not from a frame arriving.
 *
 * `now_ns` must not go backwards; GW_E_TIME if it does.
 *
 * Events are written into `out_fired` (caller-provided, `cap` entries) as
 * rule indices, and their number into `out_n`. Pass cap 0 / nullptr to
 * tick without collecting. */
[[nodiscard]] enum gw_status gw_tick(struct gw_watch *w, int64_t now_ns, uint32_t cap,
                                     uint32_t *out_fired, uint32_t *out_n);

/* Current belief about one rule, as of `now_ns`. Pure: it reads the
 * watch and computes freshness, and changes nothing. */
[[nodiscard]] enum gw_status gw_get_state(const struct gw_watch *w, uint32_t rule, int64_t now_ns,
                                          struct gw_rule_state *out);

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/* Wire format version. Bump on any change to the field set; a consumer
 * that does not recognise the version must refuse the line rather than
 * guess at it. */
enum { GW_EVENT_SCHEMA_VERSION = 1 };

/* An event as it goes to the log. Two timestamps, because they answer
 * different questions and confusing them is how a five-second detection
 * gets reported as instantaneous:
 *
 *   observed_at — when the frame that completed the confirmation was
 *                 captured; what actually happened in the world.
 *   emitted_at  — when this process decided to say so.
 *
 * Both are Unix epoch nanoseconds, UTC. They are wall-clock values used
 * for the record only: no rule in the core reads them, so a clock
 * correction can never move a confirmation window or a door duration.
 *
 * The strings are borrowed for the duration of the call. */
struct gw_event {
    const char *camera_id;
    const char *rule_id;
    const char *event_type;

    /* Per-camera monotonic counter. With camera_id it forms the stable
     * event id a consumer deduplicates on — MQTT and any later outbox
     * may deliver twice, and local event formation and transport
     * delivery are different guarantees. */
    uint64_t sequence;

    int64_t observed_at_unix_ns;
    int64_t emitted_at_unix_ns;
    uint32_t confirmations;
    /* Confirmed duration for a sustained event, 0 for an appearance. */
    int64_t sustained_ns;
};

/* Render one JSONL record (no trailing newline) into a caller buffer.
 *
 * Writes at most `cap` bytes including the terminator, and reports the
 * size a complete record needs through `out_required` — so a caller can
 * size a buffer once and then know, per record, whether it fit. Returns
 * GW_E_LIMIT without writing a partial record when it does not.
 *
 * Pure: no allocation, no clock, no locale. */
[[nodiscard]] enum gw_status gw_event_render(size_t cap, char *buf, const struct gw_event *ev,
                                             size_t *out_required);

#ifdef __cplusplus
}
#endif

#endif /* GEIST_WATCH_H */
