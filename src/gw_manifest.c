/*
 * gw_manifest.c — scene manifest parsing.
 *
 * Strict on purpose. A benchmark input that tolerates typos produces a
 * number nobody can defend: a misspelled rule id in a `sample` line
 * would silently drop that rule's labels and report a recall of zero
 * that looks like a detection failure. Every unrecognised token is an
 * error with a line number.
 */
#include "gw_bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_MS 1000000LL

struct parser {
    struct gw_scene *scene;
    size_t err_cap;
    char *err;
    uint32_t line;
    bool saw_version;
    int64_t default_after_ns;
    int64_t default_before_ns;
};

[[nodiscard]] static enum gw_status fail(struct parser *p, enum gw_status s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char detail[160];
    (void)vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (p->err != nullptr && p->err_cap > 0u) {
        (void)snprintf(p->err, p->err_cap, "line %u: %s", p->line, detail);
    }
    return s;
}

/* strtok_r over a mutable copy: the manifest is small and read once, so
 * clarity beats a zero-copy tokenizer here. */
static char *next_token(char **cursor) {
    return strtok_r(nullptr, " \t", cursor);
}

[[nodiscard]] static bool parse_i64(const char *tok, int64_t *out) {
    if (tok == nullptr) {
        return false;
    }
    char *end = nullptr;
    const long long v = strtoll(tok, &end, 10);
    if (end == tok || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

[[nodiscard]] static bool parse_u32(const char *tok, uint32_t *out) {
    int64_t v = 0;
    if (!parse_i64(tok, &v) || v < 0 || v > (int64_t)UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

[[nodiscard]] static bool parse_value(const char *tok, enum gw_value *out) {
    if (tok == nullptr) {
        return false;
    }
    if (strcmp(tok, "yes") == 0) {
        *out = GW_YES;
    } else if (strcmp(tok, "no") == 0) {
        *out = GW_NO;
    } else if (strcmp(tok, "unknown") == 0) {
        *out = GW_UNKNOWN;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] static bool copy_bounded(char *dst, size_t cap, const char *src) {
    const size_t n = strlen(src);
    if (n == 0u || n + 1u > cap) {
        return false;
    }
    memcpy(dst, src, n + 1u);
    return true;
}

[[nodiscard]] static bool find_rule(const struct gw_scene *s, const char *id, uint32_t *out) {
    for (uint32_t i = 0u; i < s->rule_count; i += 1u) {
        if (strcmp(s->rules[i].id, id) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

[[nodiscard]] static enum gw_status parse_rule(struct parser *p, char **cur) {
    struct gw_scene *s = p->scene;
    if (s->rule_count >= (uint32_t)GW_MAX_RULES) {
        return fail(p, GW_E_LIMIT, "more than %d rules", GW_MAX_RULES);
    }
    struct gw_rule_config c = {};

    const char *id = next_token(cur);
    if (id == nullptr || !copy_bounded(c.id, sizeof c.id, id)) {
        return fail(p, GW_E_INVALID_ARG, "rule needs an id of at most %d bytes", GW_ID_CAP - 1);
    }
    uint32_t dup = 0u;
    if (find_rule(s, c.id, &dup)) {
        return fail(p, GW_E_INVALID_ARG, "rule '%s' declared twice", c.id);
    }

    const char *kind = next_token(cur);
    if (kind == nullptr) {
        return fail(p, GW_E_INVALID_ARG, "rule '%s' needs a kind", c.id);
    }
    if (strcmp(kind, "appeared") == 0) {
        c.kind = GW_RULE_APPEARED;
    } else if (strcmp(kind, "sustained") == 0) {
        c.kind = GW_RULE_SUSTAINED;
    } else {
        return fail(p, GW_E_INVALID_ARG, "unknown rule kind '%s'", kind);
    }

    if (!parse_value(next_token(cur), &c.target) || c.target == GW_UNKNOWN) {
        return fail(p, GW_E_INVALID_ARG, "rule '%s' needs target yes or no", c.id);
    }

    int64_t window_ms = 0, gap_ms = 0;
    if (!parse_u32(next_token(cur), &c.confirm_count) || !parse_i64(next_token(cur), &window_ms) ||
        !parse_i64(next_token(cur), &gap_ms)) {
        return fail(p, GW_E_INVALID_ARG, "rule '%s' needs count, window_ms and gap_ms", c.id);
    }
    c.confirm_window_ns = window_ms * NS_PER_MS;
    c.max_gap_ns = gap_ms * NS_PER_MS;

    const char *sustain = next_token(cur);
    if (c.kind == GW_RULE_SUSTAINED) {
        int64_t sustain_ms = 0;
        if (!parse_i64(sustain, &sustain_ms)) {
            return fail(p, GW_E_INVALID_ARG, "sustained rule '%s' needs sustain_ms", c.id);
        }
        c.sustain_ns = sustain_ms * NS_PER_MS;
    } else if (sustain != nullptr) {
        return fail(p, GW_E_INVALID_ARG, "appeared rule '%s' takes no sustain_ms", c.id);
    }

    s->rules[s->rule_count] = c;
    s->rule_count += 1u;
    return GW_OK;
}

[[nodiscard]] static enum gw_status parse_truth(struct parser *p, char **cur) {
    struct gw_scene *s = p->scene;
    if (s->truth_count >= (uint32_t)GW_BENCH_MAX_TRUTH) {
        return fail(p, GW_E_LIMIT, "more than %d truth events", GW_BENCH_MAX_TRUTH);
    }
    struct gw_truth t = {.before_ns = p->default_before_ns, .after_ns = p->default_after_ns};

    const char *rule_id = next_token(cur);
    if (rule_id == nullptr || !find_rule(s, rule_id, &t.rule)) {
        return fail(p, GW_E_INVALID_ARG, "truth names rule '%s', which is not declared",
                    rule_id == nullptr ? "(missing)" : rule_id);
    }
    const char *type = next_token(cur);
    if (type == nullptr || !copy_bounded(t.event_type, sizeof t.event_type, type)) {
        return fail(p, GW_E_INVALID_ARG, "truth needs an event_type");
    }
    int64_t at_ms = 0;
    if (!parse_i64(next_token(cur), &at_ms)) {
        return fail(p, GW_E_INVALID_ARG, "truth needs at_ms");
    }
    t.at_ns = at_ms * NS_PER_MS;

    const char *after = next_token(cur);
    if (after != nullptr) {
        int64_t after_ms = 0;
        if (!parse_i64(after, &after_ms) || after_ms < 0) {
            return fail(p, GW_E_INVALID_ARG, "truth after_ms must be a non-negative integer");
        }
        t.after_ns = after_ms * NS_PER_MS;
    }
    if (t.after_ns <= 0) {
        return fail(p, GW_E_INVALID_ARG,
                    "truth has no match window; set match_window or give after_ms");
    }
    /* Truth must be ordered so the greedy matcher's "earliest eligible"
     * is well defined without sorting behind the author's back. */
    if (s->truth_count > 0u && t.at_ns < s->truth[s->truth_count - 1u].at_ns) {
        return fail(p, GW_E_INVALID_ARG, "truth events must be in time order");
    }
    s->truth[s->truth_count] = t;
    s->truth_count += 1u;
    return GW_OK;
}

[[nodiscard]] static enum gw_status parse_sample(struct parser *p, char **cur) {
    struct gw_scene *s = p->scene;
    if (s->rule_count == 0u) {
        return fail(p, GW_E_INVALID_ARG, "sample before any rule is declared");
    }
    if (s->sample_count >= (uint32_t)GW_BENCH_MAX_SAMPLES) {
        return fail(p, GW_E_LIMIT, "more than %d samples", GW_BENCH_MAX_SAMPLES);
    }
    /* Zeroed: has_label stays false for every rule this sample does not
     * mention, so replay feeds those rules nothing at all rather than
     * inventing an observation the scene never contained. */
    struct gw_sample smp = {};

    int64_t t_ms = 0;
    if (!parse_i64(next_token(cur), &t_ms)) {
        return fail(p, GW_E_INVALID_ARG, "sample needs t_ms");
    }
    smp.t_ns = t_ms * NS_PER_MS;
    if (s->sample_count > 0u && smp.t_ns < s->samples[s->sample_count - 1u].t_ns) {
        return fail(p, GW_E_INVALID_ARG, "samples must be in time order");
    }

    const char *frame = next_token(cur);
    if (frame == nullptr) {
        return fail(p, GW_E_INVALID_ARG, "sample needs a frame path or '-'");
    }
    if (strcmp(frame, "-") != 0 && !copy_bounded(smp.frame, sizeof smp.frame, frame)) {
        return fail(p, GW_E_LIMIT, "frame path longer than %d bytes", GW_BENCH_PATH_CAP - 1);
    }

    for (char *tok = next_token(cur); tok != nullptr; tok = next_token(cur)) {
        char *eq = strchr(tok, '=');
        if (eq == nullptr) {
            return fail(p, GW_E_INVALID_ARG, "expected <rule>=<value>, got '%s'", tok);
        }
        *eq = '\0';
        uint32_t rule = 0u;
        if (!find_rule(s, tok, &rule)) {
            return fail(p, GW_E_INVALID_ARG, "sample labels rule '%s', which is not declared", tok);
        }
        if (smp.has_label[rule]) {
            return fail(p, GW_E_INVALID_ARG, "rule '%s' labelled twice in one sample", tok);
        }
        if (!parse_value(eq + 1, &smp.label[rule])) {
            return fail(p, GW_E_INVALID_ARG, "'%s' is not yes, no or unknown", eq + 1);
        }
        smp.has_label[rule] = true;
    }

    s->samples[s->sample_count] = smp;
    s->sample_count += 1u;
    return GW_OK;
}

enum gw_status gw_scene_parse(const char *text, const size_t err_cap, char *err,
                              struct gw_scene *out) {
    if (text == nullptr || out == nullptr) {
        return GW_E_INVALID_ARG;
    }
    *out = (struct gw_scene){};
    struct parser p = {.scene = out, .err_cap = err_cap, .err = err};
    if (err != nullptr && err_cap > 0u) {
        err[0] = '\0';
    }

    const char *line_start = text;
    char line[1024];

    while (*line_start != '\0') {
        const char *nl = strchr(line_start, '\n');
        const size_t n = (nl != nullptr) ? (size_t)(nl - line_start) : strlen(line_start);
        p.line += 1u;
        if (n + 1u > sizeof line) {
            return fail(&p, GW_E_LIMIT, "line longer than %zu bytes", sizeof line - 1u);
        }
        memcpy(line, line_start, n);
        line[n] = '\0';
        line_start = (nl != nullptr) ? nl + 1 : line_start + n;

        char *cur = nullptr;
        const char *kw = strtok_r(line, " \t", &cur);
        if (kw == nullptr || kw[0] == '#') {
            continue;
        }

        enum gw_status s = GW_OK;
        if (strcmp(kw, "version") == 0) {
            int64_t v = 0;
            if (!parse_i64(next_token(&cur), &v) || v != 1) {
                return fail(&p, GW_E_INVALID_ARG, "unsupported manifest version");
            }
            p.saw_version = true;
        } else if (!p.saw_version) {
            return fail(&p, GW_E_INVALID_ARG, "manifest must start with 'version 1'");
        } else if (strcmp(kw, "scene") == 0) {
            const char *v = next_token(&cur);
            if (v == nullptr || !copy_bounded(out->name, sizeof out->name, v)) {
                return fail(&p, GW_E_INVALID_ARG, "scene needs a name");
            }
        } else if (strcmp(kw, "camera") == 0) {
            const char *v = next_token(&cur);
            if (v == nullptr || !copy_bounded(out->camera, sizeof out->camera, v)) {
                return fail(&p, GW_E_INVALID_ARG, "camera needs an id");
            }
        } else if (strcmp(kw, "split") == 0) {
            const char *v = next_token(&cur);
            if (v == nullptr) {
                return fail(&p, GW_E_INVALID_ARG, "split needs dev or test");
            }
            if (strcmp(v, "dev") == 0) {
                out->split = GW_SPLIT_DEV;
            } else if (strcmp(v, "test") == 0) {
                out->split = GW_SPLIT_TEST;
            } else {
                return fail(&p, GW_E_INVALID_ARG, "split is dev or test, not '%s'", v);
            }
        } else if (strcmp(kw, "match_window") == 0) {
            int64_t after_ms = 0, before_ms = 0;
            if (!parse_i64(next_token(&cur), &after_ms) || after_ms < 0) {
                return fail(&p, GW_E_INVALID_ARG, "match_window needs after_ms");
            }
            const char *before = next_token(&cur);
            if (before != nullptr && (!parse_i64(before, &before_ms) || before_ms < 0)) {
                return fail(&p, GW_E_INVALID_ARG, "match_window before_ms must be non-negative");
            }
            p.default_after_ns = after_ms * NS_PER_MS;
            p.default_before_ns = before_ms * NS_PER_MS;
        } else if (strcmp(kw, "tick_ms") == 0) {
            int64_t ms = 0;
            if (!parse_i64(next_token(&cur), &ms) || ms <= 0) {
                return fail(&p, GW_E_INVALID_ARG, "tick_ms must be a positive integer");
            }
            out->tick_ns = ms * NS_PER_MS;
        } else if (strcmp(kw, "rule") == 0) {
            s = parse_rule(&p, &cur);
        } else if (strcmp(kw, "truth") == 0) {
            s = parse_truth(&p, &cur);
        } else if (strcmp(kw, "restart") == 0) {
            if (out->restart_count >= (uint32_t)GW_BENCH_MAX_RESTARTS) {
                return fail(&p, GW_E_LIMIT, "more than %d restarts", GW_BENCH_MAX_RESTARTS);
            }
            int64_t at_ms = 0;
            if (!parse_i64(next_token(&cur), &at_ms) || at_ms < 0) {
                return fail(&p, GW_E_INVALID_ARG, "restart needs a non-negative t_ms");
            }
            const int64_t at_ns = at_ms * NS_PER_MS;
            if (out->restart_count > 0u && at_ns < out->restarts[out->restart_count - 1u]) {
                return fail(&p, GW_E_INVALID_ARG, "restarts must be in time order");
            }
            out->restarts[out->restart_count] = at_ns;
            out->restart_count += 1u;
        } else if (strcmp(kw, "sample") == 0) {
            s = parse_sample(&p, &cur);
        } else {
            return fail(&p, GW_E_INVALID_ARG, "unknown keyword '%s'", kw);
        }
        if (s != GW_OK) {
            return s;
        }
    }

    if (!p.saw_version) {
        return fail(&p, GW_E_INVALID_ARG, "empty manifest");
    }
    if (out->name[0] == '\0' || out->camera[0] == '\0') {
        return fail(&p, GW_E_INVALID_ARG, "manifest needs both scene and camera");
    }
    if (out->rule_count == 0u) {
        return fail(&p, GW_E_INVALID_ARG, "manifest declares no rules");
    }
    if (out->sample_count == 0u) {
        return fail(&p, GW_E_INVALID_ARG, "manifest has no samples");
    }
    if (out->tick_ns == 0) {
        out->tick_ns = 1000 * NS_PER_MS;
    }
    /* A tick coarser than a rule's evidence window could step straight
     * over an outage, so the scene would score a gap the running system
     * would have caught. Refuse rather than quietly measure something
     * else. */
    for (uint32_t i = 0u; i < out->rule_count; i += 1u) {
        if (out->tick_ns > out->rules[i].max_gap_ns) {
            p.line = 0u;
            return fail(&p, GW_E_INVALID_ARG,
                        "tick_ms is coarser than rule '%s' max_gap; a gap could pass unseen",
                        out->rules[i].id);
        }
    }
    return GW_OK;
}
