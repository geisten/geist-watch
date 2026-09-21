/*
 * The JSONL renderer. Pure function, so every case here is a fixed
 * input and a fixed expected string.
 */
#include "test_support.h"

#include <string.h>

static struct gw_event base_event(void) {
    return (struct gw_event){
        .camera_id = "cam-front",
        .rule_id = "doorstep",
        .event_type = "package_appeared",
        .sequence = 7u,
        /* 2026-09-21T07:13:42.115Z */
        .observed_at_unix_ns = 1789974822115000000LL,
        .emitted_at_unix_ns = 1789974823250000000LL,
        .confirmations = 3u,
    };
}

static void check_render(const struct gw_event *ev, const char *expect) {
    char buf[512];
    size_t need = 0u;
    const enum gw_status s = gw_event_render(sizeof buf, buf, ev, &need);
    CHECK_OK(s);
    if (s != GW_OK) {
        return;
    }
    if (strcmp(buf, expect) != 0) {
        fprintf(stderr, "  got      %s\n  expected %s\n", buf, expect);
        gw_test_failures += 1;
    }
    CHECK_EQ_INT(need, strlen(expect) + 1u);
}

static void test_appearance_record(void) {
    const struct gw_event ev = base_event();
    check_render(&ev, "{\"schema_version\":1,\"event_id\":\"cam-front:doorstep:7\""
                      ",\"camera_id\":\"cam-front\""
                      ",\"rule_id\":\"doorstep\""
                      ",\"event_type\":\"package_appeared\""
                      ",\"observed_at\":\"2026-09-21T07:13:42.115Z\""
                      ",\"emitted_at\":\"2026-09-21T07:13:43.250Z\""
                      ",\"confirmations\":3,\"sustained_ms\":0}");
}

static void test_sustained_record_carries_duration(void) {
    struct gw_event ev = base_event();
    ev.rule_id = "front-door-open";
    ev.event_type = "door_open_sustained";
    ev.sustained_ns = 300 * NS_S;
    ev.confirmations = 31u;
    check_render(&ev, "{\"schema_version\":1,\"event_id\":\"cam-front:front-door-open:7\""
                      ",\"camera_id\":\"cam-front\""
                      ",\"rule_id\":\"front-door-open\""
                      ",\"event_type\":\"door_open_sustained\""
                      ",\"observed_at\":\"2026-09-21T07:13:42.115Z\""
                      ",\"emitted_at\":\"2026-09-21T07:13:43.250Z\""
                      ",\"confirmations\":31,\"sustained_ms\":300000}");
}

/* The civil-date arithmetic is the part most likely to be subtly wrong,
 * so it gets the dates that break naive implementations. */
static void test_calendar_edges(void) {
    struct gw_event ev = base_event();
    struct {
        int64_t ns;
        const char *iso;
    } cases[] = {
        {0LL, "1970-01-01T00:00:00.000Z"},
        {951782400000000000LL, "2000-02-29T00:00:00.000Z"}, /* century leap year */
        {1709164800000000000LL, "2024-02-29T00:00:00.000Z"},
        {1735689599999000000LL, "2024-12-31T23:59:59.999Z"},
        {1740787200000000000LL, "2025-03-01T00:00:00.000Z"}, /* non-leap year */
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        char buf[512];
        size_t need = 0u;
        ev.observed_at_unix_ns = cases[i].ns;
        ev.emitted_at_unix_ns = cases[i].ns;
        CHECK_OK(gw_event_render(sizeof buf, buf, &ev, &need));
        if (strstr(buf, cases[i].iso) == nullptr) {
            fprintf(stderr, "  %s missing from %s\n", cases[i].iso, buf);
            gw_test_failures += 1;
        }
    }
}

/* A half-written JSON line is worse than a missing one, because a
 * consumer will happily parse the prefix. */
static void test_short_buffer_refuses_whole_record(void) {
    const struct gw_event ev = base_event();
    char buf[32];
    size_t need = 0u;

    CHECK_EQ_INT(gw_event_render(sizeof buf, buf, &ev, &need), GW_E_LIMIT);
    CHECK(need > sizeof buf);
    CHECK_EQ_INT(buf[0], '\0');

    /* And the reported size is the one that works. */
    char big[512];
    CHECK(need <= sizeof big);
    CHECK_OK(gw_event_render(need, big, &ev, &need));
}

static void test_quotes_and_controls_are_escaped(void) {
    struct gw_event ev = base_event();
    ev.rule_id = "odd\"rule\\name\n";
    char buf[512];
    size_t need = 0u;
    CHECK_OK(gw_event_render(sizeof buf, buf, &ev, &need));
    CHECK(strstr(buf, "\\\"rule\\\\name\\n") != nullptr);
}

static void test_rejects_missing_fields(void) {
    struct gw_event ev = base_event();
    char buf[512];
    size_t need = 0u;

    ev.camera_id = nullptr;
    CHECK_EQ_INT(gw_event_render(sizeof buf, buf, &ev, &need), GW_E_INVALID_ARG);

    ev = base_event();
    ev.event_type = nullptr;
    CHECK_EQ_INT(gw_event_render(sizeof buf, buf, &ev, &need), GW_E_INVALID_ARG);
}

int main(void) {
    RUN(test_appearance_record);
    RUN(test_sustained_record_carries_duration);
    RUN(test_calendar_edges);
    RUN(test_short_buffer_refuses_whole_record);
    RUN(test_quotes_and_controls_are_escaped);
    RUN(test_rejects_missing_fields);
    return TEST_MAIN;
}
