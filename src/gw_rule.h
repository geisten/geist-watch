/*
 * gw_rule.h — the rule a person writes, compiled into the timing
 * machine the core runs.
 *
 * Deliberately NOT part of libgeist_watch.a. The core knows about
 * counts, windows and durations and nothing else; sentences, regions and
 * the fixed German and English wordings are the product's surface, and
 * keeping them out of the library is what lets the library's promise
 * ("this header is the whole of it") stay true.
 *
 * What this layer will and will not claim:
 *
 *   It matches a fixed, small set of templates. It is not a parser of
 *   arbitrary wishes, and the plan does not ask it to be one — v0.1
 *   supports tested DE/EN sentence templates plus explicit parameters,
 *   and anything else is refused with a reason a person can act on. A
 *   refusal is the honest outcome; guessing at an unrecognised sentence
 *   would put a wrong rule behind a confident-sounding confirmation.
 *
 *   It refuses rather than silently drops. A sentence that says "five
 *   minutes" to a template that reports an appearance is rejected, not
 *   quietly stripped of its duration. The same goes for a sentence
 *   duration that contradicts an explicit parameter, and for negation,
 *   which no v0.1 template can express.
 *
 *   Every configuration it produces is one the core accepts. The
 *   compiler validates by handing the result to gw_add_rule rather than
 *   by re-stating the core's rules here, so the two cannot drift.
 */
#ifndef GW_RULE_H
#define GW_RULE_H

#include "geist_watch.h"

/* ------------------------------------------------------------------ */
/* Regions                                                              */
/* ------------------------------------------------------------------ */

/* A normalised rectangle in parts per million of the frame.
 *
 * Integers rather than doubles, because a region is converted to pixels
 * on every single frame and this way the conversion is exact integer
 * arithmetic with no rounding drift between two calls that were given
 * the same rectangle. It also makes the bounds check (x + w <= 1) a
 * comparison that means what it says, instead of one that a hair of
 * floating-point error can flip. */
enum { GW_PPM = 1000000u };

struct gw_region {
    uint32_t x_ppm, y_ppm, w_ppm, h_ppm;
};

/* Parse "x,y,w,h", each a decimal in [0,1] with at most six fractional
 * digits. Locale-free by construction: no strtod, so a machine set to a
 * decimal comma cannot reinterpret "0.15" as 15.
 *
 * Refuses a zero-width or zero-height rectangle, and one that leaves the
 * frame. A seventh fractional digit is refused rather than rounded — a
 * region the caller did not get is worth saying out loud. */
[[nodiscard]] enum gw_status gw_region_parse(const char *text, size_t err_cap, char *err,
                                             struct gw_region *out);

struct gw_pixel_rect {
    uint32_t x, y, w, h;
};

/* Convert to a pixel rectangle in a frame of `width` x `height`. Clamped
 * to the frame and guaranteed at least one pixel in each dimension, so a
 * tiny region on a small frame yields something a cropper can use rather
 * than an empty rectangle.
 *
 * A struct rather than a `uint32_t[static 4]`: the array form tells the
 * compiler the argument is non-null, which makes the null check below it
 * a diagnosable contradiction (GCC's -Wnonnull-compare) — and the check
 * is worth more than the array-size hint, since an uninitialised pointer
 * is the likelier mistake. Named fields also beat out[2] at the call
 * site. */
[[nodiscard]] enum gw_status gw_region_pixels(const struct gw_region *r, uint32_t width,
                                              uint32_t height, struct gw_pixel_rect *out);

/* ------------------------------------------------------------------ */
/* Rules                                                                */
/* ------------------------------------------------------------------ */

enum gw_lang {
    GW_LANG_EN = 0,
    GW_LANG_DE,
};

/* What the caller asks for. Zero in any numeric field means "take the
 * template's default"; the defaults are the plan's example values and
 * are not measurements, which is why they live next to the templates
 * rather than being hard-coded at the call site. */
struct gw_rule_request {
    const char *id;       /* required: [A-Za-z0-9], then [A-Za-z0-9_-] */
    const char *region;   /* "x,y,w,h"; null or empty means the whole frame */
    const char *sentence; /* required: a DE or EN template sentence */

    uint32_t confirm_count;
    int64_t confirm_window_ns;
    int64_t max_gap_ns;
    int64_t sustain_ns; /* sustained templates only */
};

/* A compiled rule.
 *
 * The four string fields point into a static table with the lifetime of
 * the program. They are borrowed, never freed, and never written to —
 * which is also why the wordings cannot be edited at runtime: the plan
 * asks for fixed German and English messages, not model-authored text.
 *
 * `question` is the internal observation prompt and is always English.
 * The plan's reasoning is that German model quality is an untested
 * assumption, so the question the model sees stays in the language the
 * model cards document, while what the person reads is a fixed
 * translation chosen here. */
struct gw_rule_spec {
    char id[GW_ID_CAP];
    struct gw_region region;
    struct gw_rule_config timing;
    enum gw_lang lang; /* the language the sentence was written in */

    const char *template_name;
    const char *question;   /* English, internal, for the model */
    const char *event_type; /* appears in the event log */
    const char *message_en;
    const char *message_de;
};

/* Compile a request. On failure writes a one-line reason into `err` —
 * addressed to the person who wrote the sentence, so it names what was
 * not understood rather than an error code. */
[[nodiscard]] enum gw_status gw_rule_compile(const struct gw_rule_request *req, size_t err_cap,
                                             char *err, struct gw_rule_spec *out);

/* The fixed message for this rule, in the language its sentence was
 * written in. Never null. */
const char *gw_rule_message(const struct gw_rule_spec *spec);

/* The templates this build understands, for `rule templates` and for a
 * refusal that can list the alternatives instead of just saying no.
 * `index` counts from zero; returns null past the end. */
const char *gw_rule_template_name(uint32_t index);
const char *gw_rule_template_example(uint32_t index, enum gw_lang lang);

#endif /* GW_RULE_H */
