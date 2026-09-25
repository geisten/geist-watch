/*
 * gw_rules.c — sentence in, validated rule out.
 *
 * The matching here is intentionally small and dumb: a fixed set of
 * templates, each a handful of required word groups, matched per
 * language. That is the whole mechanism. It is not a stemmer, it is not
 * a grammar, and it does not try to be, because the failure mode of a
 * clever matcher is a rule that looks right and watches for the wrong
 * thing — and nothing downstream can detect that, since every
 * confirmation the core produces afterwards is perfectly correct for
 * whatever rule it was given.
 *
 * So the rules of engagement are:
 *
 *   - A term matches as a PREFIX at a word start. That covers German
 *     inflection ("Pakete", "Türen") and English plurals without a
 *     stemmer. It does not cross into a compound's interior, so German
 *     compounds ("Haustür") are listed explicitly rather than guessed
 *     at; a short honest list beats substring matching that would let
 *     "Nebentürchen" arm a front-door rule.
 *   - A sentence that matches two templates, or the same template in
 *     both languages, is refused. Picking one would be a coin toss over
 *     which fixed message a person later reads.
 *   - Negation is refused outright. No v0.1 template expresses "when
 *     there is NO package", and silently watching for the opposite is
 *     the worst available outcome.
 */
#include "gw_rule.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NS_S 1000000000LL

enum {
    SENTENCE_CAP = 512, /* normalised copy; a rule sentence is one line */
    MAX_DURATION_S = 24 * 3600,
};

[[nodiscard]] static enum gw_status fail(size_t cap, char *err, enum gw_status s, const char *fmt,
                                         ...) {
    if (err != nullptr && cap > 0u) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Regions                                                              */
/* ------------------------------------------------------------------ */

/* One normalised decimal into parts per million. Hand-rolled rather than
 * strtod: strtod reads the C locale's decimal point, and on a machine
 * running de_DE that turns "0.15" into 0 followed by unparsed junk — or,
 * worse for a consumer that ignores the end pointer, into a silently
 * wrong region. A region is not worth a locale dependency. */
[[nodiscard]] static const char *decimal_ppm(const char *p, uint32_t *out) {
    if (*p < '0' || *p > '9') {
        return nullptr;
    }
    uint32_t whole = 0u;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10u + (uint32_t)(*p - '0');
        if (whole > 1u) {
            return nullptr; /* a normalised coordinate cannot exceed 1 */
        }
        p += 1;
    }

    uint32_t frac = 0u;
    uint32_t scale = GW_PPM;
    if (*p == '.') {
        p += 1;
        if (*p < '0' || *p > '9') {
            return nullptr;
        }
        while (*p >= '0' && *p <= '9') {
            if (scale == 1u) {
                return nullptr; /* a seventh digit: refuse, do not round */
            }
            scale /= 10u;
            frac += (uint32_t)(*p - '0') * scale;
            p += 1;
        }
    }
    if (whole == 1u && frac != 0u) {
        return nullptr;
    }
    *out = whole == 1u ? GW_PPM : frac;
    return p;
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p += 1;
    }
    return p;
}

enum gw_status gw_region_parse(const char *text, const size_t err_cap, char *err,
                               struct gw_region *out) {
    if (text == nullptr || out == nullptr) {
        return GW_E_INVALID_ARG;
    }
    uint32_t v[4] = {0u, 0u, 0u, 0u};
    const char *p = skip_spaces(text);
    for (int i = 0; i < 4; i += 1) {
        p = decimal_ppm(p, &v[i]);
        if (p == nullptr) {
            return fail(err_cap, err, GW_E_INVALID_ARG,
                        "region wants four numbers between 0 and 1, as x,y,width,height "
                        "(for example 0.15,0.35,0.70,0.60)");
        }
        p = skip_spaces(p);
        if (i < 3) {
            if (*p != ',') {
                return fail(err_cap, err, GW_E_INVALID_ARG,
                            "region needs four values, comma "
                            "separated: x,y,width,height");
            }
            p = skip_spaces(p + 1);
        }
    }
    if (*p != '\0') {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "region has trailing text after the fourth "
                    "value");
    }
    if (v[2] == 0u || v[3] == 0u) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "region width and height must be greater "
                    "than zero");
    }
    if (v[0] + v[2] > GW_PPM || v[1] + v[3] > GW_PPM) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "region leaves the frame: x+width and y+height must be at most 1");
    }
    *out = (struct gw_region){.x_ppm = v[0], .y_ppm = v[1], .w_ppm = v[2], .h_ppm = v[3]};
    return GW_OK;
}

enum gw_status gw_region_pixels(const struct gw_region *r, const uint32_t width,
                                const uint32_t height, struct gw_pixel_rect *out) {
    if (r == nullptr || out == nullptr || width == 0u || height == 0u) {
        return GW_E_INVALID_ARG;
    }
    const uint64_t px = (uint64_t)r->x_ppm * width / GW_PPM;
    const uint64_t py = (uint64_t)r->y_ppm * height / GW_PPM;
    uint32_t x = (uint32_t)(px >= width ? width - 1u : px);
    uint32_t y = (uint32_t)(py >= height ? height - 1u : py);

    uint64_t pw = (uint64_t)r->w_ppm * width / GW_PPM;
    uint64_t ph = (uint64_t)r->h_ppm * height / GW_PPM;
    /* A region smaller than a pixel still has to be croppable. Rounding
     * it to nothing would hand the model an empty image and produce a
     * confident `unknown` for a reason nobody could see. */
    if (pw == 0u) {
        pw = 1u;
    }
    if (ph == 0u) {
        ph = 1u;
    }
    if (x + pw > width) {
        pw = width - x;
    }
    if (y + ph > height) {
        ph = height - y;
    }
    *out = (struct gw_pixel_rect){.x = x, .y = y, .w = (uint32_t)pw, .h = (uint32_t)ph};
    return GW_OK;
}

/* ------------------------------------------------------------------ */
/* Sentence normalisation                                               */
/* ------------------------------------------------------------------ */

/* Lowercase ASCII, fold the three German capital umlauts, and reduce
 * everything that is not a letter or digit to a single space, with a
 * leading and trailing space so that " term" is a word-start test and
 * " term " is a whole-word test.
 *
 * UTF-8 continuation bytes are copied untouched: they are all >= 0x80,
 * so the ASCII case fold cannot corrupt them, and the terms in the table
 * are written in the same encoding as the source file. Uppercase ß and
 * the rarer accented forms are out of scope — the ASCII spellings
 * ("tuer", "fuenf") are in the table for anyone whose keyboard or
 * terminal produced them. */
[[nodiscard]] static bool normalise(const char *in, char *out, size_t cap) {
    size_t n = 0u;
    if (cap < 3u) {
        return false;
    }
    out[n++] = ' ';
    bool pending_space = false;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p += 1) {
        unsigned char c = *p;
        if (c == 0xC3u && p[1] != '\0') {
            const unsigned char next = p[1];
            unsigned char lower = next;
            if (next == 0x84u || next == 0x96u || next == 0x9Cu) {
                lower = (unsigned char)(next + 0x20u); /* Ä Ö Ü -> ä ö ü */
            }
            if (n + 3u >= cap) {
                return false;
            }
            if (pending_space) {
                out[n++] = ' ';
                pending_space = false;
            }
            out[n++] = (char)c;
            out[n++] = (char)lower;
            p += 1;
            continue;
        }
        if (c >= 0x80u) {
            if (n + 2u >= cap) {
                return false;
            }
            if (pending_space) {
                out[n++] = ' ';
                pending_space = false;
            }
            out[n++] = (char)c;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word) {
            pending_space = n > 1u;
            continue;
        }
        if (n + 2u >= cap) {
            return false;
        }
        if (pending_space) {
            out[n++] = ' ';
            pending_space = false;
        }
        out[n++] = (char)c;
    }
    out[n++] = ' ';
    out[n] = '\0';
    return true;
}

/* " term" — the term at the start of some word. */
[[nodiscard]] static bool has_prefix_term(const char *norm, const char *term) {
    char probe[64];
    if (snprintf(probe, sizeof probe, " %s", term) >= (int)sizeof probe) {
        return false;
    }
    return strstr(norm, probe) != nullptr;
}

/* " term " — the term as a whole word. */
[[nodiscard]] static bool has_word(const char *norm, const char *term) {
    char probe[64];
    if (snprintf(probe, sizeof probe, " %s ", term) >= (int)sizeof probe) {
        return false;
    }
    return strstr(norm, probe) != nullptr;
}

[[nodiscard]] static bool any_prefix_term(const char *norm, const char *const *terms) {
    for (const char *const *t = terms; *t != nullptr; t += 1) {
        if (has_prefix_term(norm, *t)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Durations                                                            */
/* ------------------------------------------------------------------ */

struct numeral {
    const char *word;
    int64_t value;
};

/* Only the numbers a person actually writes into a watch rule. A general
 * German number parser ("einhundertzwanzig") is a project of its own and
 * would earn nothing here: anything past this table can be given as
 * digits, which always work. */
static const struct numeral NUMERALS[] = {
    {"one", 1},       {"two", 2},        {"three", 3},    {"four", 4},     {"five", 5},
    {"six", 6},       {"seven", 7},      {"eight", 8},    {"nine", 9},     {"ten", 10},
    {"fifteen", 15},  {"twenty", 20},    {"thirty", 30},  {"sixty", 60},   {"ein", 1},
    {"eine", 1},      {"eins", 1},       {"zwei", 2},     {"drei", 3},     {"vier", 4},
    {"fünf", 5},      {"fuenf", 5},      {"sechs", 6},    {"sieben", 7},   {"acht", 8},
    {"neun", 9},      {"zehn", 10},      {"elf", 11},     {"zwölf", 12},   {"zwoelf", 12},
    {"fünfzehn", 15}, {"fuenfzehn", 15}, {"zwanzig", 20}, {"dreißig", 30}, {"dreissig", 30},
    {"sechzig", 60},
};

struct unit {
    const char *prefix;
    int64_t seconds;
    bool exact; /* matched as a whole word, not as a prefix */
};

static const struct unit UNITS[] = {
    {"sekund", 1, false},   {"second", 1, false},  {"sec", 1, false},
    {"s", 1, true},         {"minut", 60, false},  {"min", 60, false},
    {"stund", 3600, false}, {"hour", 3600, false}, {"h", 3600, true},
};

[[nodiscard]] static int64_t unit_seconds(const char *word, const size_t len) {
    for (size_t i = 0u; i < sizeof UNITS / sizeof UNITS[0]; i += 1) {
        const size_t pl = strlen(UNITS[i].prefix);
        if (UNITS[i].exact ? (len == pl) : (len >= pl)) {
            if (memcmp(word, UNITS[i].prefix, pl) == 0) {
                return UNITS[i].seconds;
            }
        }
    }
    return 0;
}

/* -1 when the word is not a number this table knows. */
[[nodiscard]] static int64_t word_number(const char *word, const size_t len) {
    for (size_t i = 0u; i < sizeof NUMERALS / sizeof NUMERALS[0]; i += 1) {
        if (strlen(NUMERALS[i].word) == len && memcmp(word, NUMERALS[i].word, len) == 0) {
            return NUMERALS[i].value;
        }
    }
    return -1;
}

/* Digits at the start of a word, with the count consumed. -1 for none. */
[[nodiscard]] static int64_t leading_digits(const char *word, const size_t len, size_t *used) {
    size_t i = 0u;
    int64_t v = 0;
    while (i < len && word[i] >= '0' && word[i] <= '9') {
        v = v * 10 + (word[i] - '0');
        if (v > MAX_DURATION_S * 3600LL) {
            return -1;
        }
        i += 1;
    }
    *used = i;
    return i == 0u ? -1 : v;
}

/* Every duration in the sentence. `count` distinguishes "none" from
 * "several", because several is a refusal and none is a default —
 * collapsing them would silently take the first of two contradictory
 * numbers. */
static void scan_durations(const char *norm, int64_t *first_s, uint32_t *count) {
    *first_s = 0;
    *count = 0u;

    const char *p = norm;
    while (*p != '\0') {
        while (*p == ' ') {
            p += 1;
        }
        const char *word = p;
        while (*p != '\0' && *p != ' ') {
            p += 1;
        }
        const size_t len = (size_t)(p - word);
        if (len == 0u) {
            break;
        }

        int64_t n = -1;
        int64_t secs = 0;
        size_t digits = 0u;
        const int64_t d = leading_digits(word, len, &digits);
        if (d >= 0 && digits < len) {
            /* "300s", "5min" — number and unit written as one word. */
            n = d;
            secs = unit_seconds(word + digits, len - digits);
        } else {
            n = (d >= 0) ? d : word_number(word, len);
            if (n >= 0) {
                /* Unit as the next word. */
                const char *q = p;
                while (*q == ' ') {
                    q += 1;
                }
                const char *unit = q;
                while (*q != '\0' && *q != ' ') {
                    q += 1;
                }
                const size_t ulen = (size_t)(q - unit);
                secs = ulen == 0u ? 0 : unit_seconds(unit, ulen);
                if (secs > 0) {
                    p = q;
                }
            }
        }
        if (n > 0 && secs > 0) {
            if (*count == 0u) {
                *first_s = n * secs;
            }
            *count += 1u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Templates                                                            */
/* ------------------------------------------------------------------ */

/* "delivery" is deliberately absent: it names the act as often as the
 * object, and "tell me when the delivery van arrives" would otherwise
 * compile into a rule watching the ground for a parcel. Requiring a
 * concrete noun costs one rewrite and removes a whole class of rule that
 * looks right and watches for something else. */
static const char *const PKG_NOUN_EN[] = {"packag", "parcel", nullptr};
static const char *const PKG_NOUN_DE[] = {"paket",   "päckchen",  "paeckchen",
                                          "sendung", "lieferung", nullptr};
static const char *const PKG_VERB_EN[] = {"left",  "leave", "deliver", "arriv", "drop", "placed",
                                          "place", "put",   "appear",  "shows", nullptr};
static const char *const PKG_VERB_DE[] = {"abgestellt", "abstell",     "abgelegt",  "ableg",
                                          "zugestellt", "zustell",     "geliefert", "liefer",
                                          "ankomm",     "hingestellt", "hinstell",  "auftaucht",
                                          "dasteht",    "erscheint",   nullptr};

/* German compounds are spelled out rather than matched by substring:
 * "Haustür" has to hit, "Nebentürchen" must not arm a front-door rule,
 * and only an explicit list can tell those apart without a lexicon. */
static const char *const DOOR_NOUN_EN[] = {"door", "gate", nullptr};
static const char *const DOOR_NOUN_DE[] = {
    "tür",         "tuer",         "haustür",    "haustuer", "eingangstür", "eingangstuer",
    "wohnungstür", "wohnungstuer", "garagentor", "tor",      nullptr};
static const char *const OPEN_EN[] = {"open", "ajar", nullptr};
static const char *const OPEN_DE[] = {"offen", "geöffnet", "geoeffnet", "aufsteht", nullptr};

struct term_group {
    const char *const *en;
    const char *const *de;
};

struct tmpl {
    const char *name;
    struct term_group groups[2];
    uint32_t group_count;

    enum gw_rule_kind kind;
    enum gw_value target;
    bool takes_duration;

    uint32_t confirm_count;
    int64_t confirm_window_ns;
    int64_t max_gap_ns;
    int64_t sustain_ns;

    const char *question;
    const char *event_type;
    const char *message_en;
    const char *message_de;
    const char *example_en;
    const char *example_de;
};

/* The thresholds below are the plan's EXAMPLE values, not measurements.
 * They are the same numbers the benchmark scenes use, so a rule compiled
 * from a sentence behaves exactly like the scenes the harness scores —
 * and when the Pi measurements in W03 replace them, one table changes. */
static const struct tmpl TEMPLATES[] = {
    {
        .name = "doorstep-package",
        .groups = {{PKG_NOUN_EN, PKG_NOUN_DE}, {PKG_VERB_EN, PKG_VERB_DE}},
        .group_count = 2u,
        .kind = GW_RULE_APPEARED,
        .target = GW_YES,
        .takes_duration = false,
        .confirm_count = 3u,
        .confirm_window_ns = 10 * NS_S,
        .max_gap_ns = 15 * NS_S,
        .sustain_ns = 0,
        .question = "Is a package or parcel resting on the ground in the marked region?",
        .event_type = "package_appeared",
        .message_en = "New package detected at the door.",
        .message_de = "Neues Paket vor der Tür erkannt.",
        .example_en = "Tell me when a package is left at the door.",
        .example_de = "Sag mir, wenn ein Paket vor der Tür abgestellt wird.",
    },
    {
        .name = "door-open",
        .groups = {{DOOR_NOUN_EN, DOOR_NOUN_DE}, {OPEN_EN, OPEN_DE}},
        .group_count = 2u,
        .kind = GW_RULE_SUSTAINED,
        .target = GW_YES,
        .takes_duration = true,
        .confirm_count = 2u,
        .confirm_window_ns = 2 * NS_S,
        .max_gap_ns = 15 * NS_S,
        .sustain_ns = 300 * NS_S,
        .question = "Is the door in the marked region open?",
        .event_type = "door_open_sustained",
        .message_en = "The door has been open for the confirmed duration.",
        .message_de = "Die Tür steht seit der bestätigten Dauer offen.",
        .example_en = "Tell me when the front door has been open for five minutes.",
        .example_de = "Melde dich, wenn die Haustür fünf Minuten offen steht.",
    },
};

enum { TEMPLATE_COUNT = sizeof TEMPLATES / sizeof TEMPLATES[0] };

const char *gw_rule_template_name(const uint32_t index) {
    return index < (uint32_t)TEMPLATE_COUNT ? TEMPLATES[index].name : nullptr;
}

const char *gw_rule_template_example(const uint32_t index, const enum gw_lang lang) {
    if (index >= (uint32_t)TEMPLATE_COUNT) {
        return nullptr;
    }
    return lang == GW_LANG_DE ? TEMPLATES[index].example_de : TEMPLATES[index].example_en;
}

/* Negation is matched as a whole word, never as a prefix: "no" as a
 * prefix would refuse "Notify me when ...", which is how half the
 * English examples start. */
static const char *const NEGATIONS[] = {"no",     "not",    "nothing", "none", "never", "without",
                                        "dont",   "doesnt", "isnt",    "don",  "kein",  "keine",
                                        "keinen", "keins",  "nicht",   "nie",  "ohne",  nullptr};

[[nodiscard]] static bool matches(const struct tmpl *t, const char *norm, const enum gw_lang lang) {
    for (uint32_t g = 0u; g < t->group_count; g += 1) {
        const char *const *terms = lang == GW_LANG_DE ? t->groups[g].de : t->groups[g].en;
        if (!any_prefix_term(norm, terms)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Compile                                                             */
/* ------------------------------------------------------------------ */

[[nodiscard]] static bool id_ok(const char *id) {
    if (id == nullptr || *id == '\0') {
        return false;
    }
    const size_t n = strlen(id);
    if (n >= GW_ID_CAP) {
        return false;
    }
    const char c0 = id[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= '0' && c0 <= '9'))) {
        return false;
    }
    for (size_t i = 1u; i < n; i += 1) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

enum gw_status gw_rule_compile(const struct gw_rule_request *req, const size_t err_cap, char *err,
                               struct gw_rule_spec *out) {
    if (req == nullptr || out == nullptr) {
        return GW_E_INVALID_ARG;
    }
    if (!id_ok(req->id)) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "rule id must start with a letter or digit, continue with letters, digits, "
                    "'-' or '_', and fit in %d bytes",
                    GW_ID_CAP - 1);
    }

    struct gw_region region = {.x_ppm = 0u, .y_ppm = 0u, .w_ppm = GW_PPM, .h_ppm = GW_PPM};
    if (req->region != nullptr && req->region[0] != '\0') {
        const enum gw_status rs = gw_region_parse(req->region, err_cap, err, &region);
        if (rs != GW_OK) {
            return rs;
        }
    }

    /* Zero means "take the template default". A negative value is not a
     * quieter way of saying zero — it is a mistake, and treating it as a
     * default would be the same silent drop this layer refuses
     * everywhere else. */
    if (req->confirm_window_ns < 0 || req->max_gap_ns < 0 || req->sustain_ns < 0) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "durations cannot be negative; leave a field at zero to take the "
                    "template's default");
    }

    if (req->sentence == nullptr || req->sentence[0] == '\0') {
        return fail(err_cap, err, GW_E_INVALID_ARG, "a rule needs a sentence; try \"%s\"",
                    TEMPLATES[0].example_en);
    }
    char norm[SENTENCE_CAP];
    if (!normalise(req->sentence, norm, sizeof norm)) {
        return fail(err_cap, err, GW_E_LIMIT, "sentence is longer than %d bytes",
                    (int)sizeof norm - 2);
    }

    for (const char *const *ng = NEGATIONS; *ng != nullptr; ng += 1) {
        if (has_word(norm, *ng)) {
            return fail(err_cap, err, GW_E_INVALID_ARG,
                        "\"%s\" makes this a negated condition, and no template expresses one; "
                        "describe what should be seen, not what should not",
                        *ng);
        }
    }

    const struct tmpl *hit = nullptr;
    enum gw_lang lang = GW_LANG_EN;
    uint32_t hits = 0u;
    for (uint32_t i = 0u; i < (uint32_t)TEMPLATE_COUNT; i += 1) {
        const bool en = matches(&TEMPLATES[i], norm, GW_LANG_EN);
        const bool de = matches(&TEMPLATES[i], norm, GW_LANG_DE);
        if (en && de) {
            return fail(err_cap, err, GW_E_INVALID_ARG,
                        "sentence reads as both German and English; write it in one language");
        }
        if (en || de) {
            hit = &TEMPLATES[i];
            lang = en ? GW_LANG_EN : GW_LANG_DE;
            hits += 1u;
        }
    }
    if (hits > 1u) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "sentence matches more than one template; ask for one thing per rule");
    }
    if (hits == 0u) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "no template understands this sentence; the ones this build knows are "
                    "\"%s\" and \"%s\"",
                    TEMPLATES[0].example_en, TEMPLATES[1].example_en);
    }

    int64_t said_s = 0;
    uint32_t said_n = 0u;
    scan_durations(norm, &said_s, &said_n);
    if (said_n > 1u) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "sentence names more than one duration; give exactly one");
    }
    if (said_n == 1u && !hit->takes_duration) {
        return fail(err_cap, err, GW_E_INVALID_ARG,
                    "template '%s' reports an appearance and has no duration to apply; "
                    "remove it or describe a state that lasts",
                    hit->name);
    }
    if (said_s > MAX_DURATION_S) {
        return fail(err_cap, err, GW_E_INVALID_ARG, "duration must be at most %d seconds",
                    MAX_DURATION_S);
    }

    int64_t sustain_ns = hit->takes_duration ? hit->sustain_ns : 0;
    if (said_n == 1u) {
        sustain_ns = said_s * NS_S;
    }
    if (req->sustain_ns > 0) {
        if (!hit->takes_duration) {
            return fail(err_cap, err, GW_E_INVALID_ARG, "template '%s' takes no duration",
                        hit->name);
        }
        /* A sentence saying five minutes and a flag saying ten is not a
         * precedence question. One of them is a mistake, and the person
         * who wrote both is the only one who knows which. */
        if (said_n == 1u && req->sustain_ns != said_s * NS_S) {
            return fail(err_cap, err, GW_E_INVALID_ARG,
                        "sentence says %lld s but the duration given is %lld s; they must agree",
                        (long long)said_s, (long long)(req->sustain_ns / NS_S));
        }
        sustain_ns = req->sustain_ns;
    }

    struct gw_rule_config timing = {
        .kind = hit->kind,
        .target = hit->target,
        .confirm_count = req->confirm_count > 0u ? req->confirm_count : hit->confirm_count,
        .confirm_window_ns =
            req->confirm_window_ns > 0 ? req->confirm_window_ns : hit->confirm_window_ns,
        .max_gap_ns = req->max_gap_ns > 0 ? req->max_gap_ns : hit->max_gap_ns,
        .sustain_ns = sustain_ns,
    };
    (void)snprintf(timing.id, sizeof timing.id, "%s", req->id);

    /* Validate by construction rather than by restating the core's rules
     * here. No request this function accepts can reach it today — every
     * way of reaching a bad config is refused above with a better
     * message — so this is a guard against a future template shipping a
     * default the core will not run, caught by the command that writes
     * the rule rather than by the watch that later reports `unknown`
     * forever. */
    struct gw_watch probe;
    if (gw_init(&probe, "probe") != GW_OK) {
        return GW_E_INVALID_ARG;
    }
    uint32_t slot = 0u;
    const enum gw_status as = gw_add_rule(&probe, &timing, &slot);
    if (as != GW_OK) {
        return fail(err_cap, err, as, "the resulting rule is not one the core can run (%s)",
                    gw_status_str(as));
    }

    *out = (struct gw_rule_spec){
        .region = region,
        .timing = timing,
        .lang = lang,
        .template_name = hit->name,
        .question = hit->question,
        .event_type = hit->event_type,
        .message_en = hit->message_en,
        .message_de = hit->message_de,
    };
    (void)snprintf(out->id, sizeof out->id, "%s", req->id);
    return GW_OK;
}

const char *gw_rule_message(const struct gw_rule_spec *spec) {
    if (spec == nullptr) {
        return "";
    }
    return spec->lang == GW_LANG_DE ? spec->message_de : spec->message_en;
}
