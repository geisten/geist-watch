/*
 * gw_report.c — parse a benchmark report and a criteria file, then grade.
 *
 * Strict in the same way the manifest parser is strict, and for the same
 * reason: a benchmark input that tolerates a typo produces a number nobody
 * can defend. A misspelled metric key would silently leave a criterion
 * unmeasured, and an unmeasured criterion that rendered as a pass is the one
 * failure mode this file exists to prevent.
 */
#include "gw_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const PROV_KEYS[GW_PROV_COUNT] = {
    [GW_PROV_MODEL_ID] = "model_id",
    [GW_PROV_MODEL_SHA256] = "model_sha256",
    [GW_PROV_MODEL_QUANTIZATION] = "model_quantization",
    [GW_PROV_ENGINE_COMMIT] = "engine_commit",
    [GW_PROV_COMPILER] = "compiler",
    [GW_PROV_COMPILER_FLAGS] = "compiler_flags",
    [GW_PROV_OS] = "os",
    [GW_PROV_HARDWARE] = "hardware",
    [GW_PROV_SOURCE] = "source",
    [GW_PROV_ROI] = "roi",
    [GW_PROV_RESOLUTION] = "resolution",
    [GW_PROV_SAMPLING] = "sampling",
    [GW_PROV_PROMPT_ID] = "prompt_id",
    [GW_PROV_MAX_TOKENS] = "max_tokens",
    [GW_PROV_START] = "start",
};

const char *gw_provenance_key(const enum gw_provenance_field f) {
    return (f >= 0 && f < GW_PROV_COUNT) ? PROV_KEYS[f] : "";
}

struct ctx {
    size_t err_cap;
    char *err;
    uint32_t line;
};

[[nodiscard]] static enum gw_status fail(struct ctx *c, const enum gw_status s, const char *fmt,
                                         ...) {
    va_list ap;
    va_start(ap, fmt);
    char detail[160];
    (void)vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (c->err != nullptr && c->err_cap > 0u) {
        (void)snprintf(c->err, c->err_cap, "line %u: %s", c->line, detail);
    }
    return s;
}

/* Locale-free decimal. strtod reads the C locale's decimal point, so on a
 * machine set to a decimal comma "0.95" would parse as 0 with unconsumed
 * junk — and a precision floor of 0 passes everything. */
[[nodiscard]] static bool parse_number(const char *tok, double *out) {
    if (tok == nullptr || *tok == '\0') {
        return false;
    }
    const char *p = tok;
    double sign = 1.0;
    if (*p == '-') {
        sign = -1.0;
        p += 1;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    double whole = 0.0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10.0 + (double)(*p - '0');
        p += 1;
    }
    double frac = 0.0, scale = 1.0;
    if (*p == '.') {
        p += 1;
        if (*p < '0' || *p > '9') {
            return false;
        }
        while (*p >= '0' && *p <= '9') {
            scale /= 10.0;
            frac += (double)(*p - '0') * scale;
            p += 1;
        }
    }
    if (*p != '\0') {
        return false;
    }
    *out = sign * (whole + frac);
    return true;
}

/* Split a line into its first token and the remainder, both trimmed. The
 * remainder is kept whole: compiler_flags and an OS name contain spaces, and
 * tokenising them would lose exactly the detail that makes a run
 * identifiable. */
static void split_line(char *line, char **key, char **rest) {
    while (*line == ' ' || *line == '\t') {
        line += 1;
    }
    *key = line;
    while (*line != '\0' && *line != ' ' && *line != '\t') {
        line += 1;
    }
    if (*line != '\0') {
        *line = '\0';
        line += 1;
        while (*line == ' ' || *line == '\t') {
            line += 1;
        }
    }
    *rest = line;
    size_t n = strlen(*rest);
    while (n > 0u &&
           ((*rest)[n - 1u] == ' ' || (*rest)[n - 1u] == '\t' || (*rest)[n - 1u] == '\r')) {
        (*rest)[n - 1u] = '\0';
        n -= 1u;
    }
}

/* Iterate a text buffer line by line into a mutable scratch line. */
#define FOR_EACH_LINE(text, buf, c)                                                                \
    for (const char *cursor_ = (text); *cursor_ != '\0';)                                          \
        for (bool once_ = ((c)->line += 1u, copy_line(&cursor_, (buf), sizeof(buf)), true); once_; \
             once_ = false)

static void copy_line(const char **cursor, char *buf, const size_t cap) {
    const char *p = *cursor;
    size_t n = 0u;
    while (*p != '\0' && *p != '\n') {
        if (n + 1u < cap) {
            buf[n] = *p;
            n += 1u;
        }
        p += 1;
    }
    buf[n] = '\0';
    *cursor = (*p == '\n') ? p + 1 : p;
}

enum gw_status gw_report_parse(const char *text, const size_t err_cap, char *err,
                               struct gw_report *out) {
    if (text == nullptr || out == nullptr) {
        return GW_E_INVALID_ARG;
    }
    *out = (struct gw_report){};
    struct ctx c = {.err_cap = err_cap, .err = err, .line = 0u};
    bool saw_version = false;
    char buf[512];

    FOR_EACH_LINE(text, buf, &c) {
        char *key = nullptr, *rest = nullptr;
        split_line(buf, &key, &rest);
        if (*key == '\0' || *key == '#') {
            continue;
        }
        if (strcmp(key, "version") == 0) {
            if (strcmp(rest, "1") != 0) {
                return fail(&c, GW_E_INVALID_ARG, "report version must be 1, got '%s'", rest);
            }
            saw_version = true;
            continue;
        }
        if (!saw_version) {
            return fail(&c, GW_E_INVALID_ARG, "report must declare 'version 1' first");
        }
        if (strcmp(key, "metric") == 0) {
            char *mkey = nullptr, *mval = nullptr;
            split_line(rest, &mkey, &mval);
            if (*mkey == '\0') {
                return fail(&c, GW_E_INVALID_ARG, "metric needs a name and a value");
            }
            if (strlen(mkey) + 1u > (size_t)GW_REPORT_KEY_CAP) {
                return fail(&c, GW_E_LIMIT, "metric name '%s' is too long", mkey);
            }
            double v = 0.0;
            if (!parse_number(mval, &v)) {
                return fail(&c, GW_E_INVALID_ARG, "metric '%s' needs a decimal value, got '%s'",
                            mkey, mval);
            }
            for (uint32_t i = 0u; i < out->metric_count; i += 1u) {
                if (strcmp(out->metrics[i].key, mkey) == 0) {
                    return fail(&c, GW_E_INVALID_ARG, "metric '%s' reported twice", mkey);
                }
            }
            if (out->metric_count >= (uint32_t)GW_REPORT_MAX_METRICS) {
                return fail(&c, GW_E_LIMIT, "more than %d metrics", GW_REPORT_MAX_METRICS);
            }
            struct gw_metric *m = &out->metrics[out->metric_count];
            (void)snprintf(m->key, sizeof m->key, "%s", mkey);
            m->value = v;
            out->metric_count += 1u;
            continue;
        }
        bool matched = false;
        for (int f = 0; f < GW_PROV_COUNT; f += 1) {
            if (strcmp(key, PROV_KEYS[f]) == 0) {
                if (*rest == '\0') {
                    return fail(&c, GW_E_INVALID_ARG, "'%s' needs a value", key);
                }
                if (out->has_provenance[f]) {
                    return fail(&c, GW_E_INVALID_ARG, "'%s' given twice", key);
                }
                (void)snprintf(out->provenance[f], GW_REPORT_VALUE_CAP, "%s", rest);
                out->has_provenance[f] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return fail(&c, GW_E_INVALID_ARG, "unknown key '%s'", key);
        }
    }
    if (!saw_version) {
        return fail(&c, GW_E_INVALID_ARG, "report is empty or has no version");
    }
    return GW_OK;
}

enum gw_status gw_criteria_parse(const char *text, const size_t err_cap, char *err,
                                 struct gw_criteria *out) {
    if (text == nullptr || out == nullptr) {
        return GW_E_INVALID_ARG;
    }
    *out = (struct gw_criteria){};
    struct ctx c = {.err_cap = err_cap, .err = err, .line = 0u};
    bool saw_version = false;
    char buf[512];

    FOR_EACH_LINE(text, buf, &c) {
        char *key = nullptr, *rest = nullptr;
        split_line(buf, &key, &rest);
        if (*key == '\0' || *key == '#') {
            continue;
        }
        if (strcmp(key, "version") == 0) {
            if (strcmp(rest, "1") != 0) {
                return fail(&c, GW_E_INVALID_ARG, "criteria version must be 1, got '%s'", rest);
            }
            saw_version = true;
            continue;
        }
        if (!saw_version) {
            return fail(&c, GW_E_INVALID_ARG, "criteria must declare 'version 1' first");
        }
        if (strcmp(key, "profile") == 0) {
            (void)snprintf(out->profile, sizeof out->profile, "%s", rest);
            continue;
        }
        if (strcmp(key, "require") != 0) {
            return fail(&c, GW_E_INVALID_ARG, "unknown key '%s'", key);
        }
        char *mkey = nullptr, *tail = nullptr;
        split_line(rest, &mkey, &tail);
        char *op = nullptr, *tail2 = nullptr;
        split_line(tail, &op, &tail2);
        char *limit = nullptr, *about = nullptr;
        split_line(tail2, &limit, &about);

        if (*mkey == '\0' || *op == '\0' || *limit == '\0') {
            return fail(&c, GW_E_INVALID_ARG, "require needs <metric> <=|>= <limit> [note]");
        }
        enum gw_compare cmp;
        if (strcmp(op, "<=") == 0) {
            cmp = GW_CMP_LE;
        } else if (strcmp(op, ">=") == 0) {
            cmp = GW_CMP_GE;
        } else {
            return fail(&c, GW_E_INVALID_ARG, "comparison must be <= or >=, got '%s'", op);
        }
        double lim = 0.0;
        if (!parse_number(limit, &lim)) {
            return fail(&c, GW_E_INVALID_ARG, "limit must be a decimal, got '%s'", limit);
        }
        if (strlen(mkey) + 1u > (size_t)GW_REPORT_KEY_CAP) {
            return fail(&c, GW_E_LIMIT, "metric name '%s' is too long", mkey);
        }
        for (uint32_t i = 0u; i < out->count; i += 1u) {
            if (strcmp(out->items[i].key, mkey) == 0) {
                return fail(&c, GW_E_INVALID_ARG, "criterion '%s' declared twice", mkey);
            }
        }
        if (out->count >= (uint32_t)GW_REPORT_MAX_CRITERIA) {
            return fail(&c, GW_E_LIMIT, "more than %d criteria", GW_REPORT_MAX_CRITERIA);
        }
        struct gw_criterion *it = &out->items[out->count];
        (void)snprintf(it->key, sizeof it->key, "%s", mkey);
        it->cmp = cmp;
        it->limit = lim;
        (void)snprintf(it->about, sizeof it->about, "%s", about);
        out->count += 1u;
    }
    if (!saw_version) {
        return fail(&c, GW_E_INVALID_ARG, "criteria file is empty or has no version");
    }
    if (out->count == 0u) {
        return fail(&c, GW_E_INVALID_ARG,
                    "criteria file requires nothing; a gate that gates "
                    "nothing passes everything");
    }
    return GW_OK;
}

enum gw_status gw_report_grade(const struct gw_report *report, const struct gw_criteria *criteria,
                               const size_t err_cap, char *err, struct gw_grade *out_grades,
                               uint32_t *out_failed) {
    if (report == nullptr || criteria == nullptr || out_grades == nullptr ||
        out_failed == nullptr) {
        return GW_E_INVALID_ARG;
    }
    /* Refuse before grading: a verdict about a run nobody can identify is
     * worse than no verdict, because it looks like evidence. */
    for (int f = 0; f < GW_PROV_COUNT; f += 1) {
        if (!report->has_provenance[f]) {
            if (err != nullptr && err_cap > 0u) {
                (void)snprintf(err, err_cap,
                               "report is missing '%s'; a measurement without it cannot be "
                               "compared to any other run, including its own successor",
                               PROV_KEYS[f]);
            }
            return GW_E_INVALID_ARG;
        }
    }

    *out_failed = 0u;
    for (uint32_t i = 0u; i < criteria->count; i += 1u) {
        const struct gw_criterion *it = &criteria->items[i];
        const struct gw_metric *found = nullptr;
        for (uint32_t m = 0u; m < report->metric_count; m += 1u) {
            if (strcmp(report->metrics[m].key, it->key) == 0) {
                found = &report->metrics[m];
                break;
            }
        }
        if (found == nullptr) {
            /* Not the same as failing, and never the same as passing: the
             * cheapest way through a gate is to not run the case. */
            out_grades[i] = (struct gw_grade){.verdict = GW_VERDICT_NOT_MEASURED, .measured = 0.0};
            *out_failed += 1u;
            continue;
        }
        const bool ok =
            (it->cmp == GW_CMP_LE) ? (found->value <= it->limit) : (found->value >= it->limit);
        out_grades[i] = (struct gw_grade){
            .verdict = ok ? GW_VERDICT_PASS : GW_VERDICT_FAIL,
            .measured = found->value,
        };
        if (!ok) {
            *out_failed += 1u;
        }
    }
    return GW_OK;
}
