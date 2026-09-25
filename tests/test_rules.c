/*
 * Rule compilation: regions, templates, refusals.
 *
 * The cases that matter most here are the refusals. A matcher that
 * accepts too much does not fail loudly — it produces a perfectly valid
 * rule that watches for the wrong thing, and every confirmation the core
 * then makes is correct for that wrong rule. So each test that expects a
 * refusal is a test that the system says "I did not understand you"
 * instead of quietly deciding what was meant.
 */
#include "gw_rule.h"
#include "test_support.h"

#include <string.h>

static struct gw_rule_spec g_spec;

static enum gw_status compile_sentence(const char *sentence, char *err, size_t err_cap) {
    const struct gw_rule_request req = {
        .id = "doorstep",
        .region = "0.15,0.35,0.70,0.60",
        .sentence = sentence,
    };
    err[0] = '\0';
    return gw_rule_compile(&req, err_cap, err, &g_spec);
}

static void expect_refused(const char *sentence, const char *needle) {
    char err[256];
    if (compile_sentence(sentence, err, sizeof err) == GW_OK) {
        fprintf(stderr, "  expected a refusal for \"%s\"\n", sentence);
        gw_test_failures += 1;
        return;
    }
    if (strstr(err, needle) == nullptr) {
        fprintf(stderr, "  refusal was '%s', expected it to mention '%s'\n", err, needle);
        gw_test_failures += 1;
    }
}

/* --- regions -------------------------------------------------------- */

static void test_region_is_parsed_without_a_locale(void) {
    char err[128] = "";
    struct gw_region r;
    CHECK_OK(gw_region_parse("0.15,0.35,0.70,0.60", sizeof err, err, &r));
    CHECK_EQ_INT(r.x_ppm, 150000);
    CHECK_EQ_INT(r.y_ppm, 350000);
    CHECK_EQ_INT(r.w_ppm, 700000);
    CHECK_EQ_INT(r.h_ppm, 600000);

    /* Spaces around the separators, and the whole frame. */
    CHECK_OK(gw_region_parse(" 0 , 0 , 1 , 1 ", sizeof err, err, &r));
    CHECK_EQ_INT(r.w_ppm, GW_PPM);
}

static void test_region_refuses_what_it_cannot_mean(void) {
    char err[128] = "";
    struct gw_region r;
    struct {
        const char *text;
        const char *needle;
    } cases[] = {
        {"0,0,1", "four"},
        {"0,0,1,1,1", "trailing"},
        {"0,0,0,1", "greater"},
        {"0.5,0,0.6,1", "leaves the frame"},
        {"0,0.5,1,0.6", "leaves the frame"},
        {"1.5,0,0.1,0.1", "between 0 and 1"},
        {"0.1234567,0,0.1,0.1", "between 0 and 1"}, /* a seventh digit, not rounded */
        {"0,5;0,1;0,1;0,1", "between 0 and 1"},     /* decimal comma is not accepted */
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1) {
        err[0] = '\0';
        if (gw_region_parse(cases[i].text, sizeof err, err, &r) == GW_OK) {
            fprintf(stderr, "  expected '%s' to be refused\n", cases[i].text);
            gw_test_failures += 1;
        } else if (strstr(err, cases[i].needle) == nullptr) {
            fprintf(stderr, "  '%s' refused with '%s', expected '%s'\n", cases[i].text, err,
                    cases[i].needle);
            gw_test_failures += 1;
        }
    }
}

static void test_region_to_pixels_is_exact(void) {
    char err[128] = "";
    struct gw_region r;
    struct gw_pixel_rect px;
    CHECK_OK(gw_region_parse("0.25,0.5,0.5,0.25", sizeof err, err, &r));
    CHECK_OK(gw_region_pixels(&r, 640u, 480u, &px));
    CHECK_EQ_INT(px.x, 160);
    CHECK_EQ_INT(px.y, 240);
    CHECK_EQ_INT(px.w, 320);
    CHECK_EQ_INT(px.h, 120);
}

/* A region smaller than a pixel still has to produce a croppable
 * rectangle: an empty one would hand the model no image and yield an
 * `unknown` nobody could explain. */
static void test_tiny_region_still_has_a_pixel(void) {
    char err[128] = "";
    struct gw_region r;
    struct gw_pixel_rect px;
    CHECK_OK(gw_region_parse("0.999,0.999,0.001,0.001", sizeof err, err, &r));
    CHECK_OK(gw_region_pixels(&r, 64u, 64u, &px));
    CHECK(px.w >= 1u && px.h >= 1u);
    CHECK(px.x + px.w <= 64u);
    CHECK(px.y + px.h <= 64u);
    /* The null check the struct form makes possible: with a
     * uint32_t[static 4] parameter this call would not compile, and the
     * check inside would be a contradiction GCC diagnoses. */
    CHECK(gw_region_pixels(&r, 64u, 64u, nullptr) == GW_E_INVALID_ARG);
}

/* --- templates ------------------------------------------------------ */

static void test_the_plans_own_english_sentences(void) {
    char err[256] = "";
    CHECK_OK(compile_sentence("Tell me when a package is left at the door.", err, sizeof err));
    CHECK(strcmp(g_spec.template_name, "doorstep-package") == 0);
    CHECK_EQ_INT(g_spec.lang, GW_LANG_EN);
    CHECK_EQ_INT(g_spec.timing.kind, GW_RULE_APPEARED);
    CHECK_EQ_INT(g_spec.timing.target, GW_YES);
    CHECK_EQ_INT(g_spec.timing.confirm_count, 3);
    CHECK_EQ_INT(g_spec.timing.confirm_window_ns, 10 * NS_S);
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 0);
    CHECK(strcmp(g_spec.event_type, "package_appeared") == 0);
    CHECK(strcmp(gw_rule_message(&g_spec), "New package detected at the door.") == 0);

    CHECK_OK(compile_sentence("Tell me when the front door has been open for five minutes.", err,
                              sizeof err));
    CHECK(strcmp(g_spec.template_name, "door-open") == 0);
    CHECK_EQ_INT(g_spec.timing.kind, GW_RULE_SUSTAINED);
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 300 * NS_S);
}

static void test_the_plans_own_german_sentences(void) {
    char err[256] = "";
    CHECK_OK(
        compile_sentence("Sag mir, wenn ein Paket vor der Tür abgestellt wird.", err, sizeof err));
    CHECK(strcmp(g_spec.template_name, "doorstep-package") == 0);
    CHECK_EQ_INT(g_spec.lang, GW_LANG_DE);
    CHECK(strcmp(gw_rule_message(&g_spec), "Neues Paket vor der Tür erkannt.") == 0);

    CHECK_OK(compile_sentence("Melde dich, wenn die Haustür fünf Minuten offen steht.", err,
                              sizeof err));
    CHECK(strcmp(g_spec.template_name, "door-open") == 0);
    CHECK_EQ_INT(g_spec.lang, GW_LANG_DE);
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 300 * NS_S);
}

/* The compiled defaults are the ones the benchmark scenes use, so a rule
 * written as a sentence behaves exactly like the scenes the harness
 * scores. If either side is retuned, this test says so. */
static void test_defaults_match_the_benchmark_scenes(void) {
    char err[256] = "";
    CHECK_OK(compile_sentence("Tell me when a package is left at the door.", err, sizeof err));
    CHECK_EQ_INT(g_spec.timing.confirm_count, 3);
    CHECK_EQ_INT(g_spec.timing.confirm_window_ns, 10000 * NS_MS);
    CHECK_EQ_INT(g_spec.timing.max_gap_ns, 15000 * NS_MS);

    CHECK_OK(compile_sentence("Tell me when the door is open for 300 seconds.", err, sizeof err));
    CHECK_EQ_INT(g_spec.timing.confirm_count, 2);
    CHECK_EQ_INT(g_spec.timing.confirm_window_ns, 2000 * NS_MS);
    CHECK_EQ_INT(g_spec.timing.max_gap_ns, 15000 * NS_MS);
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 300000 * NS_MS);
}

/* Case and umlaut spelling should not decide whether a rule exists. */
static void test_case_and_ascii_umlauts(void) {
    char err[256] = "";
    CHECK_OK(compile_sentence("MELDE DICH, WENN DIE HAUSTÜR 5 MIN OFFEN STEHT", err, sizeof err));
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 300 * NS_S);
    CHECK_OK(compile_sentence("melde dich wenn die haustuer 5min offen steht", err, sizeof err));
    CHECK_EQ_INT(g_spec.timing.sustain_ns, 300 * NS_S);
}

static void test_durations_in_several_spellings(void) {
    char err[256] = "";
    const struct {
        const char *sentence;
        int64_t seconds;
    } cases[] = {
        {"Tell me when the door is open for 90 seconds.", 90},
        {"Tell me when the door is open for 90s.", 90},
        {"Tell me when the door is open for two minutes.", 120},
        {"Tell me when the door is open for 1 hour.", 3600},
        {"Melde dich, wenn die Tür zwei Stunden offen steht.", 7200},
        {"Melde dich, wenn die Tür 30 Sekunden offen steht.", 30},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1) {
        if (compile_sentence(cases[i].sentence, err, sizeof err) != GW_OK) {
            fprintf(stderr, "  \"%s\" was refused: %s\n", cases[i].sentence, err);
            gw_test_failures += 1;
            continue;
        }
        CHECK_EQ_INT(g_spec.timing.sustain_ns, cases[i].seconds * NS_S);
    }
}

/* --- refusals ------------------------------------------------------- */

static void test_unknown_sentence_lists_what_is_known(void) {
    expect_refused("Tell me when the washing machine is finished.", "no template");
    expect_refused("Sag mir, wenn der Topf überkocht.", "no template");
}

/* Watching for the opposite of what was asked is the worst outcome
 * available, so a negation is refused rather than interpreted. */
static void test_negation_is_refused(void) {
    expect_refused("Tell me when no package has been delivered.", "negated");
    expect_refused("Melde dich, wenn kein Paket abgestellt wurde.", "negated");
}

/* "Notify" starts with "no". Negation therefore matches whole words
 * only, and this is the test that keeps it that way. */
static void test_notify_is_not_a_negation(void) {
    char err[256] = "";
    CHECK_OK(compile_sentence("Notify me when a parcel is dropped at the door.", err, sizeof err));
    CHECK(strcmp(g_spec.template_name, "doorstep-package") == 0);
}

/* "Delivery" names the act as often as the object. Requiring a concrete
 * noun costs one rewrite and stops "the delivery van arrives" becoming a
 * rule that watches the ground for a parcel. */
static void test_a_delivery_is_not_a_package(void) {
    expect_refused("Tell me when the delivery van arrives.", "no template");
    char err[256] = "";
    CHECK_OK(compile_sentence("Tell me when a package is delivered.", err, sizeof err));
}

static void test_two_templates_in_one_sentence_is_refused(void) {
    expect_refused("Tell me when a package is left at the door and the door is open.",
                   "more than one template");
}

/* A duration the template cannot honour is dropped by no one: the
 * person is told, because a rule that silently ignores "ten minutes" is
 * a rule that fires nine minutes early forever. */
static void test_duration_on_an_appearance_is_refused(void) {
    expect_refused("Tell me when a package has been left at the door for ten minutes.",
                   "no duration to apply");
}

static void test_two_durations_are_refused(void) {
    expect_refused("Tell me when the door is open for 5 minutes and 30 seconds.",
                   "more than one duration");
}

/* A sentence and a flag that disagree is a mistake, not a precedence
 * question, and only the author knows which of the two was meant. */
static void test_sentence_and_flag_must_agree(void) {
    char err[256] = "";
    const struct gw_rule_request req = {
        .id = "front-door",
        .sentence = "Tell me when the door is open for five minutes.",
        .sustain_ns = 600 * NS_S,
    };
    struct gw_rule_spec spec;
    CHECK(gw_rule_compile(&req, sizeof err, err, &spec) != GW_OK);
    CHECK(strstr(err, "must agree") != nullptr);

    /* Agreeing is fine, and so is a flag with no duration in the text. */
    const struct gw_rule_request agree = {
        .id = "front-door",
        .sentence = "Tell me when the door is open for five minutes.",
        .sustain_ns = 300 * NS_S,
    };
    CHECK_OK(gw_rule_compile(&agree, sizeof err, err, &spec));
    const struct gw_rule_request flag_only = {
        .id = "front-door",
        .sentence = "Tell me when the door is open.",
        .sustain_ns = 42 * NS_S,
    };
    CHECK_OK(gw_rule_compile(&flag_only, sizeof err, err, &spec));
    CHECK_EQ_INT(spec.timing.sustain_ns, 42 * NS_S);
}

static void test_ids_are_constrained(void) {
    char err[256] = "";
    struct gw_rule_spec spec;
    const char *bad[] = {"", "-leading", "has space", "has/slash",
                         "this-id-is-far-too-long-for-the-cap"};
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i += 1) {
        const struct gw_rule_request req = {
            .id = bad[i],
            .sentence = "Tell me when a package is left at the door.",
        };
        if (gw_rule_compile(&req, sizeof err, err, &spec) == GW_OK) {
            fprintf(stderr, "  expected id '%s' to be refused\n", bad[i]);
            gw_test_failures += 1;
        }
    }
}

/* Zero means "use the template default"; a negative value is a mistake,
 * and quietly reading it as zero would be the same silent drop this
 * layer refuses for durations in the sentence. */
static void test_negative_parameters_are_refused(void) {
    char err[256] = "";
    struct gw_rule_spec spec;
    const int64_t fields[] = {-1, 0, 0};
    for (size_t i = 0u; i < 3u; i += 1) {
        struct gw_rule_request req = {
            .id = "doorstep",
            .sentence = "Tell me when a package is left at the door.",
        };
        req.confirm_window_ns = i == 0u ? fields[0] : 0;
        req.max_gap_ns = i == 1u ? -1 : 0;
        req.sustain_ns = i == 2u ? -1 : 0;
        err[0] = '\0';
        CHECK(gw_rule_compile(&req, sizeof err, err, &spec) != GW_OK);
        CHECK(strstr(err, "cannot be negative") != nullptr);
    }
}

/* Every template's own defaults have to be a configuration the core will
 * accept. The compiler proves that by handing each one to gw_add_rule,
 * which is also why no template can ship a default that only fails once
 * a watch is running. */
static void test_every_template_default_is_runnable(void) {
    char err[256] = "";
    for (uint32_t i = 0u; gw_rule_template_name(i) != nullptr; i += 1) {
        const struct gw_rule_request req = {.id = "r",
                                            .sentence = gw_rule_template_example(i, GW_LANG_EN)};
        struct gw_rule_spec spec;
        CHECK_OK(gw_rule_compile(&req, sizeof err, err, &spec));

        struct gw_watch w;
        CHECK_OK(gw_init(&w, "cam"));
        uint32_t slot = 0u;
        CHECK_OK(gw_add_rule(&w, &spec.timing, &slot));
    }
}

/* The region is optional, and its absence means the whole frame rather
 * than an error — but it is reported, not hidden. */
static void test_missing_region_is_the_whole_frame(void) {
    char err[256] = "";
    struct gw_rule_spec spec;
    const struct gw_rule_request req = {
        .id = "doorstep",
        .sentence = "Tell me when a package is left at the door.",
    };
    CHECK_OK(gw_rule_compile(&req, sizeof err, err, &spec));
    CHECK_EQ_INT(spec.region.x_ppm, 0);
    CHECK_EQ_INT(spec.region.w_ppm, GW_PPM);
    CHECK_EQ_INT(spec.region.h_ppm, GW_PPM);
}

/* A compiled rule has to run in the core it was validated against, and
 * produce the event its template promised. */
static void test_a_compiled_rule_runs_in_the_core(void) {
    char err[256] = "";
    CHECK_OK(compile_sentence("Tell me when a package is left at the door.", err, sizeof err));

    struct gw_watch w;
    CHECK_OK(gw_init(&w, "cam-front"));
    uint32_t rule = 0u;
    CHECK_OK(gw_add_rule(&w, &g_spec.timing, &rule));

    uint64_t frame = 0u;
    for (int64_t t = 0; t <= 10 * NS_S; t += 5 * NS_S) {
        CHECK_OK(feed(&w, rule, GW_NO, t, &frame));
        CHECK_EQ_INT(tick_count(&w, t), 0);
    }
    uint32_t fired = 0u;
    for (int64_t t = 15 * NS_S; t <= 25 * NS_S; t += 5 * NS_S) {
        CHECK_OK(feed(&w, rule, GW_YES, t, &frame));
        fired += tick_count(&w, t);
    }
    CHECK_EQ_INT(fired, 1);
}

static void test_templates_are_listable(void) {
    CHECK(gw_rule_template_name(0) != nullptr);
    CHECK(gw_rule_template_name(1) != nullptr);
    CHECK(gw_rule_template_name(2) == nullptr);
    CHECK(gw_rule_template_example(0, GW_LANG_DE) != nullptr);
    CHECK(gw_rule_template_example(0, GW_LANG_EN) != nullptr);
    CHECK(gw_rule_template_example(2, GW_LANG_EN) == nullptr);
}

/* Every listed example must compile, in both languages. A template whose
 * own example is refused is a template nobody can use. */
static void test_every_example_compiles(void) {
    char err[256] = "";
    for (uint32_t i = 0u; gw_rule_template_name(i) != nullptr; i += 1) {
        const enum gw_lang langs[] = {GW_LANG_EN, GW_LANG_DE};
        for (size_t l = 0u; l < 2u; l += 1) {
            const char *example = gw_rule_template_example(i, langs[l]);
            const struct gw_rule_request req = {.id = "r", .sentence = example};
            struct gw_rule_spec spec;
            err[0] = '\0';
            if (gw_rule_compile(&req, sizeof err, err, &spec) != GW_OK) {
                fprintf(stderr, "  example \"%s\" was refused: %s\n", example, err);
                gw_test_failures += 1;
                continue;
            }
            if (strcmp(spec.template_name, gw_rule_template_name(i)) != 0) {
                fprintf(stderr, "  example \"%s\" compiled to template '%s', expected '%s'\n",
                        example, spec.template_name, gw_rule_template_name(i));
                gw_test_failures += 1;
            }
            CHECK_EQ_INT(spec.lang, langs[l]);
        }
    }
}

int main(void) {
    RUN(test_region_is_parsed_without_a_locale);
    RUN(test_region_refuses_what_it_cannot_mean);
    RUN(test_region_to_pixels_is_exact);
    RUN(test_tiny_region_still_has_a_pixel);
    RUN(test_the_plans_own_english_sentences);
    RUN(test_the_plans_own_german_sentences);
    RUN(test_defaults_match_the_benchmark_scenes);
    RUN(test_case_and_ascii_umlauts);
    RUN(test_durations_in_several_spellings);
    RUN(test_unknown_sentence_lists_what_is_known);
    RUN(test_negation_is_refused);
    RUN(test_notify_is_not_a_negation);
    RUN(test_a_delivery_is_not_a_package);
    RUN(test_two_templates_in_one_sentence_is_refused);
    RUN(test_duration_on_an_appearance_is_refused);
    RUN(test_two_durations_are_refused);
    RUN(test_sentence_and_flag_must_agree);
    RUN(test_ids_are_constrained);
    RUN(test_negative_parameters_are_refused);
    RUN(test_every_template_default_is_runnable);
    RUN(test_missing_region_is_the_whole_frame);
    RUN(test_a_compiled_rule_runs_in_the_core);
    RUN(test_templates_are_listable);
    RUN(test_every_example_compiles);
    return TEST_MAIN;
}
