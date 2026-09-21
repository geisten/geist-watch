/*
 * The package and door rules, against the scenarios the project plan
 * names in its release criteria. Every case runs on a virtual clock.
 */
#include "test_support.h"

/* The plan's worked example: three positives spread over at least ten
 * seconds, evidence good for fifteen. */
static struct gw_rule_config package_cfg(void) {
    struct gw_rule_config c = {
        .kind = GW_RULE_APPEARED,
        .target = GW_YES,
        .confirm_count = 3u,
        .confirm_window_ns = 10 * NS_S,
        .max_gap_ns = 15 * NS_S,
    };
    const char id[] = "doorstep";
    for (size_t i = 0u; i < sizeof id; i += 1u) {
        c.id[i] = id[i];
    }
    return c;
}

static struct gw_rule_config door_cfg(void) {
    struct gw_rule_config c = {
        .kind = GW_RULE_SUSTAINED,
        .target = GW_YES,
        .confirm_count = 2u,
        .confirm_window_ns = 2 * NS_S,
        .max_gap_ns = 15 * NS_S,
        .sustain_ns = 300 * NS_S,
    };
    const char id[] = "front-door-open";
    for (size_t i = 0u; i < sizeof id; i += 1u) {
        c.id[i] = id[i];
    }
    return c;
}

static uint32_t setup(struct gw_watch *w, const struct gw_rule_config *cfg) {
    uint32_t rule = 0u;
    CHECK_OK(gw_init(w, "cam-front"));
    CHECK_OK(gw_add_rule(w, cfg, &rule));
    return rule;
}

/* Confirm `v` from t0, one observation every `step`, enough of them to
 * satisfy both the count and the window. Returns the time of the last. */
static int64_t confirm(struct gw_watch *w, uint32_t rule, enum gw_value v, int64_t t0, int64_t step,
                       uint32_t n, uint64_t *frame) {
    int64_t t = t0;
    for (uint32_t i = 0u; i < n; i += 1u) {
        t = t0 + (int64_t)i * step;
        CHECK_OK(feed(w, rule, v, t, frame));
    }
    return t;
}

/* --- configuration ------------------------------------------------- */

static void test_rejects_unusable_configs(void) {
    struct gw_watch w;
    uint32_t rule = 0u;
    CHECK_OK(gw_init(&w, "cam"));

    struct gw_rule_config c = package_cfg();
    c.target = GW_UNKNOWN; /* could never confirm */
    CHECK_EQ_INT(gw_add_rule(&w, &c, &rule), GW_E_INVALID_ARG);

    c = package_cfg();
    c.confirm_count = 0u;
    CHECK_EQ_INT(gw_add_rule(&w, &c, &rule), GW_E_INVALID_ARG);

    c = package_cfg();
    c.sustain_ns = 60 * NS_S; /* an appearance has no duration */
    CHECK_EQ_INT(gw_add_rule(&w, &c, &rule), GW_E_INVALID_ARG);

    c = door_cfg();
    c.sustain_ns = 0; /* a sustained rule must have one */
    CHECK_EQ_INT(gw_add_rule(&w, &c, &rule), GW_E_INVALID_ARG);

    c = package_cfg();
    c.max_gap_ns = 0; /* evidence would never expire */
    CHECK_EQ_INT(gw_add_rule(&w, &c, &rule), GW_E_INVALID_ARG);
}

/* --- evidence ------------------------------------------------------- */

/* Three frames 200 ms apart are three views of one moment. The window is
 * what separates them from three views of a state. */
static void test_burst_within_window_does_not_confirm(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    confirm(&w, rule, GW_YES, 0, 200 * NS_MS, 3u, &frame);
    CHECK_OK(gw_get_state(&w, rule, 400 * NS_MS, &st));
    CHECK_EQ_INT(st.confirmed, GW_UNKNOWN);

    /* The same evidence, once it has actually spanned the window. */
    CHECK_OK(feed(&w, rule, GW_YES, 10 * NS_S, &frame));
    CHECK_OK(gw_get_state(&w, rule, 10 * NS_S, &st));
    CHECK_EQ_INT(st.confirmed, GW_YES);
    CHECK_EQ_INT(st.confirmations, 4);
}

static void test_replayed_frame_is_not_new_evidence(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    struct gw_rule_state st;

    const struct gw_observation o = {.frame_id = 7u, .observed_at_ns = 0, .value = GW_YES};
    CHECK_OK(gw_observe(&w, rule, &o));

    /* A stalled capture re-running the model over the held frame must
     * not talk the rule into a confirmation. */
    for (int i = 0; i < 5; i += 1) {
        CHECK_EQ_INT(gw_observe(&w, rule, &o), GW_E_DUPLICATE);
    }
    CHECK_OK(gw_get_state(&w, rule, 12 * NS_S, &st));
    CHECK_EQ_INT(st.confirmed, GW_UNKNOWN);
    CHECK_EQ_INT(st.confirmations, 0);
}

static void test_out_of_order_observation_is_refused(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;

    CHECK_OK(feed(&w, rule, GW_YES, 10 * NS_S, &frame));
    CHECK_EQ_INT(feed(&w, rule, GW_YES, 5 * NS_S, &frame), GW_E_STALE);
}

/* --- the package rule ---------------------------------------------- */

/* A watch that starts up looking at a package already on the doorstep
 * records the state and invents no delivery. */
static void test_package_present_at_startup_is_state_not_delivery(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    const int64_t t = confirm(&w, rule, GW_YES, 0, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 0);
    CHECK_OK(gw_get_state(&w, rule, t, &st));
    CHECK_EQ_INT(st.confirmed, GW_YES);
    CHECK(!st.fired);
}

static void test_delivery_fires_once_and_does_not_repeat(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;

    /* Confirmed absence is what arms the rule. */
    int64_t t = confirm(&w, rule, GW_NO, 0, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 0);

    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 1);

    /* The package stays. A cooldown would re-announce it; confirmed
     * absence is the only thing that re-arms, so nothing more fires. */
    for (int i = 0; i < 20; i += 1) {
        t += 5 * NS_S;
        CHECK_OK(feed(&w, rule, GW_YES, t, &frame));
        CHECK_EQ_INT(tick_count(&w, t), 0);
    }
}

static void test_pickup_then_redelivery_fires_again(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;

    int64_t t = confirm(&w, rule, GW_NO, 0, 5 * NS_S, 3u, &frame);
    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 1);

    t = confirm(&w, rule, GW_NO, t + 5 * NS_S, 5 * NS_S, 3u, &frame); /* picked up */
    CHECK_EQ_INT(tick_count(&w, t), 0);

    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame); /* second parcel */
    CHECK_EQ_INT(tick_count(&w, t), 1);
}

/* A camera that stops answering must not leave a stale certainty behind. */
static void test_camera_failure_expires_evidence(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    const int64_t t = confirm(&w, rule, GW_YES, 0, 5 * NS_S, 3u, &frame);
    CHECK_OK(gw_get_state(&w, rule, t, &st));
    CHECK_EQ_INT(st.confirmed, GW_YES);
    CHECK_EQ_INT(st.health, GW_HEALTHY);

    CHECK_EQ_INT(tick_count(&w, t + 16 * NS_S), 0);
    CHECK_OK(gw_get_state(&w, rule, t + 16 * NS_S, &st));
    CHECK_EQ_INT(st.confirmed, GW_UNKNOWN);
    CHECK_EQ_INT(st.health, GW_DEGRADED);

    CHECK_OK(gw_get_state(&w, rule, t + 200 * NS_S, &st));
    CHECK_EQ_INT(st.health, GW_OFFLINE);
}

/* Coming back from an outage to a package that is simply there cannot be
 * told apart from watching one arrive, so it is not announced as a
 * delivery. The plan accepts the missed event inside the gap as a
 * documented sampling limit, and prefers it to a fabricated one. */
static void test_occlusion_does_not_manufacture_a_delivery(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;

    int64_t t = confirm(&w, rule, GW_NO, 0, 5 * NS_S, 3u, &frame); /* armed */

    /* Lens covered: no observations for well past max_gap. */
    CHECK_EQ_INT(tick_count(&w, t + 60 * NS_S), 0);

    /* Sight returns, and a package is there. */
    t = confirm(&w, rule, GW_YES, t + 60 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 0);

    /* Re-confirming absence arms it again, and the next arrival counts. */
    t = confirm(&w, rule, GW_NO, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 1);
}

static void test_restart_clears_everything(void) {
    struct gw_watch w;
    const struct gw_rule_config c = package_cfg();
    uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    int64_t t = confirm(&w, rule, GW_NO, 0, 5 * NS_S, 3u, &frame);
    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 1);

    rule = setup(&w, &c); /* a new process */
    CHECK_OK(gw_get_state(&w, rule, t, &st));
    CHECK_EQ_INT(st.confirmed, GW_UNKNOWN);
    CHECK_EQ_INT(st.health, GW_OFFLINE);
    CHECK(!st.fired);

    /* And it is disarmed, so the package still sitting there is state. */
    t = confirm(&w, rule, GW_YES, t + 5 * NS_S, 5 * NS_S, 3u, &frame);
    CHECK_EQ_INT(tick_count(&w, t), 0);
}

/* --- the door rule -------------------------------------------------- */

/* Drive `rule` with `v` every 10 s across [from, to], keeping evidence
 * inside max_gap, ticking as it goes and counting what fires. */
static uint32_t drive(struct gw_watch *w, uint32_t rule, enum gw_value v, int64_t from, int64_t to,
                      uint64_t *frame) {
    uint32_t events = 0u;
    for (int64_t t = from; t <= to; t += 10 * NS_S) {
        CHECK_OK(feed(w, rule, v, t, frame));
        events += tick_count(w, t);
    }
    return events;
}

static void test_door_fires_only_after_the_full_duration(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    /* Confirmed open at t = 2 s; the run is measured from there, never
     * from the first unconfirmed glimpse. */
    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 2 * NS_S, &frame));
    CHECK_OK(gw_get_state(&w, rule, 2 * NS_S, &st));
    CHECK_EQ_INT(st.confirmed, GW_YES);

    /* One second short of the threshold: nothing. */
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 10 * NS_S, 300 * NS_S, &frame), 0);
    CHECK_EQ_INT(tick_count(&w, 301 * NS_S), 0);
    CHECK_OK(gw_get_state(&w, rule, 301 * NS_S, &st));
    CHECK_EQ_INT(st.sustained_ns, 299 * NS_S);

    /* And at the threshold: exactly one, then no repeats. */
    CHECK_EQ_INT(tick_count(&w, 302 * NS_S), 1);
    CHECK_EQ_INT(tick_count(&w, 303 * NS_S), 0);
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 310 * NS_S, 600 * NS_S, &frame), 0);
}

/* A single fresh contrary observation ends the run — it does not have to
 * be confirmed first. Ending a five-minute claim early is cheap; carrying
 * one through a closed door is not. */
static void test_door_run_breaks_on_one_fresh_closed(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 2 * NS_S, &frame));
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 10 * NS_S, 200 * NS_S, &frame), 0);

    CHECK_OK(feed(&w, rule, GW_NO, 205 * NS_S, &frame));
    CHECK_OK(gw_get_state(&w, rule, 205 * NS_S, &st));
    CHECK_EQ_INT(st.sustained_ns, 0);

    /* The clock keeps running, but the evidence restarted. */
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 210 * NS_S, 500 * NS_S, &frame), 0);
}

static void test_door_run_breaks_on_one_fresh_unknown(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 2 * NS_S, &frame));
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 10 * NS_S, 250 * NS_S, &frame), 0);

    CHECK_OK(feed(&w, rule, GW_UNKNOWN, 255 * NS_S, &frame));
    CHECK_OK(gw_get_state(&w, rule, 255 * NS_S, &st));
    CHECK_EQ_INT(st.sustained_ns, 0);
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 260 * NS_S, 500 * NS_S, &frame), 0);
}

/* The core ticks without frames, so a camera that goes quiet invalidates
 * the evidence rather than letting the duration accrue in the dark. */
static void test_door_missing_frames_invalidate_the_run(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 2 * NS_S, &frame));

    /* No frames at all for five minutes. Without the gap check the run
     * would mature and fire. */
    CHECK_EQ_INT(tick_count(&w, 310 * NS_S), 0);
    CHECK_OK(gw_get_state(&w, rule, 310 * NS_S, &st));
    CHECK_EQ_INT(st.confirmed, GW_UNKNOWN);
    CHECK_EQ_INT(st.sustained_ns, 0);
}

static void test_tick_refuses_time_going_backwards(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    const uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    (void)rule;

    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_EQ_INT(tick_count(&w, 10 * NS_S), 0);
    CHECK_EQ_INT(gw_tick(&w, 9 * NS_S, 0u, nullptr, nullptr), GW_E_TIME);
}

static void test_door_restart_inherits_no_duration(void) {
    struct gw_watch w;
    const struct gw_rule_config c = door_cfg();
    uint32_t rule = setup(&w, &c);
    uint64_t frame = 0u;
    struct gw_rule_state st;

    CHECK_OK(feed(&w, rule, GW_YES, 0, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 2 * NS_S, &frame));
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 10 * NS_S, 290 * NS_S, &frame), 0);

    /* Restart just before the threshold. The new process has no claim on
     * the 288 seconds the old one observed. */
    rule = setup(&w, &c);
    CHECK_OK(gw_get_state(&w, rule, 290 * NS_S, &st));
    CHECK_EQ_INT(st.sustained_ns, 0);

    CHECK_OK(feed(&w, rule, GW_YES, 292 * NS_S, &frame));
    CHECK_OK(feed(&w, rule, GW_YES, 294 * NS_S, &frame));
    CHECK_EQ_INT(drive(&w, rule, GW_YES, 300 * NS_S, 593 * NS_S, &frame), 0);
    CHECK_EQ_INT(tick_count(&w, 594 * NS_S), 1);
}

int main(void) {
    RUN(test_rejects_unusable_configs);
    RUN(test_burst_within_window_does_not_confirm);
    RUN(test_replayed_frame_is_not_new_evidence);
    RUN(test_out_of_order_observation_is_refused);
    RUN(test_package_present_at_startup_is_state_not_delivery);
    RUN(test_delivery_fires_once_and_does_not_repeat);
    RUN(test_pickup_then_redelivery_fires_again);
    RUN(test_camera_failure_expires_evidence);
    RUN(test_occlusion_does_not_manufacture_a_delivery);
    RUN(test_restart_clears_everything);
    RUN(test_door_fires_only_after_the_full_duration);
    RUN(test_door_run_breaks_on_one_fresh_closed);
    RUN(test_door_run_breaks_on_one_fresh_unknown);
    RUN(test_door_missing_frames_invalidate_the_run);
    RUN(test_tick_refuses_time_going_backwards);
    RUN(test_door_restart_inherits_no_duration);
    return TEST_MAIN;
}
