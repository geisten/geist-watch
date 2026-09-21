/*
 * gw_event.c — versioned JSONL rendering.
 *
 * One function, and it is pure: same event in, same bytes out, on any
 * host, in any locale, at any time of day. That is deliberate. Events
 * are the product's only output, they are compared against ground truth
 * in the release criteria, and a renderer that consulted the clock or
 * the environment could not be held to either.
 *
 * Writes into a caller buffer and never allocates. A record that does
 * not fit is refused whole rather than truncated — a half-written JSON
 * line is worse than a missing one, because a consumer will parse it.
 */
#include "geist_watch.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Days from 1970-01-01 to the civil date, by Howard Hinnant's algorithm.
 * Used instead of gmtime_r so rendering stays a pure function of its
 * arguments: gmtime_r is thread-safe but still reaches into the C
 * library, and on some platforms into the timezone database. */
static void civil_from_days(int64_t z, int *out_y, unsigned *out_m, unsigned *out_d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = (uint64_t)(z - era * 146097); /* [0, 146096] */
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365]   */
    const uint64_t mp = (5 * doy + 2) / 153;                      /* [0, 11]    */
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;              /* [1, 31]    */
    const uint64_t m = mp < 10 ? mp + 3 : mp - 9;                 /* [1, 12]    */

    *out_y = (int)(y + (m <= 2 ? 1 : 0));
    *out_m = (unsigned)m;
    *out_d = (unsigned)d;
}

/* RFC 3339 UTC with millisecond precision, e.g. 2026-09-21T07:13:42.115Z.
 * Milliseconds rather than nanoseconds because that is the resolution
 * the events actually carry — a confirmation is pinned to a frame, and
 * frames arrive a few per second. Printing nine digits would imply a
 * precision the capture path does not have. */
static void format_rfc3339(int64_t unix_ns, size_t cap, char buf[static 1]) {
    int64_t secs = unix_ns / 1000000000;
    int64_t rem = unix_ns % 1000000000;
    if (rem < 0) { /* C truncates toward zero; calendars do not. */
        rem += 1000000000;
        secs -= 1;
    }
    const int64_t days = (secs >= 0) ? secs / 86400 : (secs - 86399) / 86400;
    int64_t sod = secs - days * 86400; /* [0, 86399] */

    int y = 0;
    unsigned mo = 0, d = 0;
    civil_from_days(days, &y, &mo, &d);

    (void)snprintf(buf, cap, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", y, mo, d,
                   (unsigned)(sod / 3600), (unsigned)((sod / 60) % 60), (unsigned)(sod % 60),
                   (unsigned)(rem / 1000000));
}

/* Append `src` to `buf` as a JSON string body, escaping what RFC 8259
 * requires. Ids come from operator configuration rather than from the
 * model, but an unescaped quote would still produce a line no consumer
 * can parse, so this is not optional. Always advances *len by the space
 * the complete text needs, whether or not it fit. */
static void append_escaped(size_t cap, char *buf, size_t *len, const char *src) {
    for (const char *p = src; *p != '\0'; p += 1) {
        char esc[8];
        const char *out = esc;
        size_t n = 1u;
        switch (*p) {
        case '"':
            memcpy(esc, "\\\"", 2u);
            n = 2u;
            break;
        case '\\':
            memcpy(esc, "\\\\", 2u);
            n = 2u;
            break;
        case '\n':
            memcpy(esc, "\\n", 2u);
            n = 2u;
            break;
        case '\r':
            memcpy(esc, "\\r", 2u);
            n = 2u;
            break;
        case '\t':
            memcpy(esc, "\\t", 2u);
            n = 2u;
            break;
        default:
            if ((unsigned char)*p < 0x20u) {
                n = (size_t)snprintf(esc, sizeof esc, "\\u%04x", (unsigned)(unsigned char)*p);
            } else {
                esc[0] = *p;
            }
            break;
        }
        for (size_t i = 0u; i < n; i += 1u) {
            if (*len + 1u < cap) {
                buf[*len] = out[i];
            }
            *len += 1u;
        }
    }
}

static void append_text(size_t cap, char *buf, size_t *len, const char *src) {
    for (const char *p = src; *p != '\0'; p += 1) {
        if (*len + 1u < cap) {
            buf[*len] = *p;
        }
        *len += 1u;
    }
}

static void append_field_str(size_t cap, char *buf, size_t *len, const char *key, const char *val) {
    append_text(cap, buf, len, key);
    append_escaped(cap, buf, len, val);
    append_text(cap, buf, len, "\"");
}

enum gw_status gw_event_render(const size_t cap, char *buf, const struct gw_event *ev,
                               size_t *out_required) {
    if (ev == nullptr || out_required == nullptr || (cap > 0u && buf == nullptr)) {
        return GW_E_INVALID_ARG;
    }
    if (ev->camera_id == nullptr || ev->rule_id == nullptr || ev->event_type == nullptr) {
        return GW_E_INVALID_ARG;
    }

    char observed[40];
    char emitted[40];
    format_rfc3339(ev->observed_at_unix_ns, sizeof observed, observed);
    format_rfc3339(ev->emitted_at_unix_ns, sizeof emitted, emitted);

    char scratch[160];
    size_t len = 0u;

    (void)snprintf(scratch, sizeof scratch, "{\"schema_version\":%d,\"event_id\":\"",
                   GW_EVENT_SCHEMA_VERSION);
    append_text(cap, buf, &len, scratch);
    /* event_id is camera:rule:sequence — stable, and derivable by a
     * consumer from the fields that follow, so deduplication needs no
     * side channel. */
    append_escaped(cap, buf, &len, ev->camera_id);
    append_text(cap, buf, &len, ":");
    append_escaped(cap, buf, &len, ev->rule_id);
    (void)snprintf(scratch, sizeof scratch, ":%llu", (unsigned long long)ev->sequence);
    append_text(cap, buf, &len, scratch);
    append_text(cap, buf, &len, "\"");

    append_field_str(cap, buf, &len, ",\"camera_id\":\"", ev->camera_id);
    append_field_str(cap, buf, &len, ",\"rule_id\":\"", ev->rule_id);
    append_field_str(cap, buf, &len, ",\"event_type\":\"", ev->event_type);
    append_field_str(cap, buf, &len, ",\"observed_at\":\"", observed);
    append_field_str(cap, buf, &len, ",\"emitted_at\":\"", emitted);

    (void)snprintf(scratch, sizeof scratch, ",\"confirmations\":%u,\"sustained_ms\":%lld}",
                   ev->confirmations, (long long)(ev->sustained_ns / 1000000));
    append_text(cap, buf, &len, scratch);

    *out_required = len + 1u;
    if (*out_required > cap) {
        /* Refuse whole. The caller sizes up and retries; it never ships
         * a truncated line. */
        if (cap > 0u) {
            buf[0] = '\0';
        }
        return GW_E_LIMIT;
    }
    buf[len] = '\0';
    return GW_OK;
}
