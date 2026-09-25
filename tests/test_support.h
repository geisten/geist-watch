/*
 * Minimal test scaffolding. No framework: the core has no dependencies,
 * and its tests should not introduce one.
 *
 * Every test drives a VIRTUAL clock. Nothing here sleeps, so the
 * five-minute door rule and its 299/300-second boundary are exercised in
 * microseconds — which is the only way the release criteria's boundary
 * cases can be checked at all.
 */
#ifndef GW_TEST_SUPPORT_H
#define GW_TEST_SUPPORT_H

#include "geist_watch.h"

#include <stdio.h>

#define NS_MS 1000000LL
#define NS_S 1000000000LL

static int gw_test_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);                    \
            gw_test_failures += 1;                                                                 \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                                             \
    do {                                                                                           \
        const long long a_ = (long long)(actual);                                                  \
        const long long e_ = (long long)(expected);                                                \
        if (a_ != e_) {                                                                            \
            fprintf(stderr, "  %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual,   \
                    a_, e_);                                                                       \
            gw_test_failures += 1;                                                                 \
        }                                                                                          \
    } while (0)

#define CHECK_OK(expr) CHECK_EQ_INT((expr), GW_OK)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        const int before_ = gw_test_failures;                                                      \
        fn();                                                                                      \
        if (gw_test_failures != before_) {                                                         \
            fprintf(stderr, "FAILED: %s\n", #fn);                                                  \
        }                                                                                          \
    } while (0)

#define TEST_MAIN (gw_test_failures == 0 ? 0 : 1)

/* Feed one observation, allocating a fresh frame id. Tests that need to
 * replay a frame id on purpose call gw_observe directly. */
static inline enum gw_status feed(struct gw_watch *w, uint32_t rule, enum gw_value v, int64_t at_ns,
                                  uint64_t *frame) {
    *frame += 1u;
    const struct gw_observation o = {.frame_id = *frame, .observed_at_ns = at_ns, .value = v};
    return gw_observe(w, rule, &o);
}

/* Tick and report how many events came due. */
static inline uint32_t tick_count(struct gw_watch *w, int64_t now_ns) {
    uint32_t fired[GW_MAX_RULES];
    uint32_t n = 0u;
    const enum gw_status s = gw_tick(w, now_ns, GW_MAX_RULES, fired, &n);
    if (s != GW_OK) {
        fprintf(stderr, "  tick failed: %s\n", gw_status_str(s));
        gw_test_failures += 1;
        return 0u;
    }
    return n;
}

#endif /* GW_TEST_SUPPORT_H */
