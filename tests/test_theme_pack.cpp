#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "ssstudio/theme_pack.h"
#include "test.h"

using namespace ssstudio;

namespace {

/// A pack with only what [pack] requires, so each test adds exactly the section
/// it is about and nothing else can explain the result.
std::string pack_text(const std::string& body, const std::string& inherit = "dark") {
    return "[pack]\nformat = 1\nname = \"Test\"\ninherit = \"" + inherit + "\"\n\n" + body;
}

ResolvedTheme resolve(const std::string& body, Diagnostics& diags,
                      const std::string& id = "test") {
    ThemePack pack;
    CHECK(parse_theme_pack_text(pack_text(body), id, pack, diags));
    ResolvedTheme theme;
    resolve_theme(pack, ThemeResolveContext{}, theme, diags);
    return theme;
}

std::string hex(Rgba color) { return color_rgba_to_hex(color); }

bool has_errors_in(const Diagnostics& diags) { return has_errors(diags); }

}  // namespace

// ---------------------------------------------------------------------------
// The value grammar
// ---------------------------------------------------------------------------

TEST(a_theme_value_reads_hex_references_and_transforms) {
    const std::map<std::string, Rgba> roles = {{"accent", 0x4080c0FFu}};
    const std::map<std::string, Rgba> palette = {{"ink", 0xffffffFFu}};
    Rgba out = 0;
    std::string error;

    CHECK(eval_theme_color("#112233", roles, palette, out, error));
    CHECK_EQ(out, 0x112233FFu);

    // Three digits expand, and a bare name looks in roles before the palette.
    CHECK(eval_theme_color("#abc", roles, palette, out, error));
    CHECK_EQ(out, 0xaabbccFFu);
    CHECK(eval_theme_color("$accent", roles, palette, out, error));
    CHECK_EQ(out, 0x4080c0FFu);
    CHECK(eval_theme_color("$palette.ink", roles, palette, out, error));
    CHECK_EQ(out, 0xffffffFFu);

    // alpha replaces the alpha and leaves the colour alone.
    CHECK(eval_theme_color("alpha($accent, 50%)", roles, palette, out, error));
    CHECK_EQ(out & 0xFFFFFF00u, 0x4080c000u);
    CHECK(std::abs(static_cast<int>(out & 0xFFu) - 128) <= 1);

    // Nesting, to the depth a real pack uses.
    CHECK(eval_theme_color("mix(alpha($accent, 50%), lighten($palette.ink, 5%), 30%)", roles,
                           palette, out, error));

    CHECK(!eval_theme_color("$nothing", roles, palette, out, error));
    CHECK(!error.empty());
    CHECK(!eval_theme_color("rotate($accent, 20%)", roles, palette, out, error));
    CHECK(!eval_theme_color("lighten($accent)", roles, palette, out, error));
}

TEST(lightening_is_perceptual_and_total) {
    // The property the format promises: a fixed step is the same visible step
    // whatever the hue, and no argument can produce something unpaintable.
    const Rgba blue = 0x2244aaFFu;
    const Rgba yellow = 0xccbb44FFu;
    const float before_blue = color_contrast(color_lighten(blue, 0.1f), blue);
    const float before_yellow = color_contrast(color_lighten(yellow, 0.1f), yellow);
    CHECK(std::fabs(before_blue - before_yellow) < 0.35f);

    // Saturation survives: an sRGB scale would wash the blue out towards grey.
    const Rgba lighter = color_lighten(blue, 0.2f);
    const int r = static_cast<int>((lighter >> 24) & 0xFF);
    const int b = static_cast<int>((lighter >> 8) & 0xFF);
    CHECK(b - r > 60);

    // Out of range in both directions, and alpha untouched.
    CHECK_EQ(color_lighten(0xffffffFFu, 4.0f), 0xffffffFFu);
    CHECK_EQ(color_darken(0x000000FFu, 4.0f), 0x000000FFu);
    CHECK_EQ(color_alpha(0x112233FFu, 0.0f) & 0xFFu, 0x00u);
}

TEST(contrast_is_measured_on_what_reaches_the_eye) {
    // White on black is the extreme WCAG ratio.
    CHECK(std::fabs(color_contrast(0xffffffFFu, 0x000000FFu) - 21.0f) < 0.05f);
    CHECK(std::fabs(color_contrast(0x000000FFu, 0x000000FFu) - 1.0f) < 0.01f);

    // A fully transparent foreground is the background, so the ratio is 1 -
    // not the ratio the unblended colour would have had.
    CHECK(std::fabs(color_contrast(color_alpha(0xffffffFFu, 0.0f), 0x000000FFu) - 1.0f) < 0.01f);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(a_pack_is_refused_for_the_reasons_that_make_it_unreadable) {
    ThemePack pack;
    Diagnostics diags;

    // No format, an unknown format, and unparseable TOML are all refusals
    // rather than best-effort reads.
    CHECK(!parse_theme_pack_text("[pack]\nname = \"x\"\n", "x", pack, diags));
    CHECK(!parse_theme_pack_text("[pack]\nformat = 2\n", "x", pack, diags));
    CHECK(!parse_theme_pack_text("[pack\nformat = 1\n", "x", pack, diags));
    CHECK(!parse_theme_pack_text("name = \"x\"\n", "x", pack, diags));
    CHECK(has_errors_in(diags));

    // A built-in id is refused: the settings file records an id, and two themes
    // answering to "dark" would make that record ambiguous.
    diags.clear();
    CHECK(!parse_theme_pack_text(pack_text(""), "dark", pack, diags));
    CHECK(has_errors_in(diags));

    // A font path that leaves the pack would work on one machine only.
    diags.clear();
    CHECK(!parse_theme_pack_text(pack_text("[font]\nui = \"../../../etc/font.ttf\"\n"), "x", pack,
                                 diags));
    diags.clear();
    CHECK(!parse_theme_pack_text(pack_text("[font]\nui = \"/Users/someone/font.ttf\"\n"), "x",
                                 pack, diags));
}

TEST(unknown_keys_warn_but_the_pack_still_loads) {
    ThemePack pack;
    Diagnostics diags;
    CHECK(parse_theme_pack_text(
        pack_text("[roles]\nnot_a_role = \"#ffffff\"\naccent = \"#123456\"\n"
                  "[colors]\nNotAColor = \"#ffffff\"\nButton = \"#654321\"\n"
                  "[style]\nnot_a_metric = 4.0\nframe_rounding = 3.0\n"),
        "x", pack, diags));
    CHECK(!has_errors_in(diags));
    CHECK_EQ(diags.size(), std::size_t{3});

    // The good keys survived; the unknown ones did not become values.
    CHECK_EQ(pack.roles.count("accent"), std::size_t{1});
    CHECK_EQ(pack.roles.count("not_a_role"), std::size_t{0});
    CHECK_EQ(pack.ui.count("Button"), std::size_t{1});
    CHECK_EQ(pack.style.scalar.count("frame_rounding"), std::size_t{1});
    CHECK_EQ(pack.style.scalar.count("not_a_metric"), std::size_t{0});
}

TEST(a_retired_imgui_colour_name_still_means_its_colour) {
    ThemePack pack;
    Diagnostics diags;
    CHECK(parse_theme_pack_text(pack_text("[colors]\nTabActive = \"#010203\"\n"), "x", pack,
                                diags));
    CHECK_EQ(pack.ui.count("TabSelected"), std::size_t{1});
    CHECK_EQ(pack.ui.count("TabActive"), std::size_t{0});
    CHECK(!has_errors_in(diags));
}

TEST(a_metric_outside_its_range_is_clamped_and_said_so) {
    ThemePack pack;
    Diagnostics diags;
    CHECK(parse_theme_pack_text(pack_text("[style]\nwindow_rounding = 400.0\n"), "x", pack,
                                diags));
    CHECK_EQ(pack.style.scalar["window_rounding"], 16.0f);
    CHECK_EQ(diags.size(), std::size_t{1});
    CHECK(diags[0].severity == Severity::Warning);
}

TEST(a_pack_id_comes_from_the_filename) {
    CHECK_STREQ(theme_pack_id("themes/midnight.s3theme"), "midnight");
    CHECK_STREQ(theme_pack_id("themes/Midnight.S3Theme"), "midnight");
    CHECK_STREQ(theme_pack_id("themes/mid-night_2.s3theme"), "mid-night_2");
    // Not a pack, or not a usable id.
    CHECK_STREQ(theme_pack_id("themes/midnight.toml"), "");
    CHECK_STREQ(theme_pack_id("themes/.s3theme"), "");
    CHECK_STREQ(theme_pack_id("themes/-leading.s3theme"), "");
    CHECK_STREQ(theme_pack_id("themes/has space.s3theme"), "");
}

// ---------------------------------------------------------------------------
// The cascade
// ---------------------------------------------------------------------------

TEST(twenty_roles_fill_sixty_three_widget_colours) {
    Diagnostics diags;
    const ResolvedTheme theme = resolve(
        "[palette]\nvoid = \"#0b0c0f\"\nmist = \"#c7ccd6\"\nice = \"#7aa2f7\"\n"
        "[roles]\n\"surface.base\" = \"$palette.void\"\n"
        "\"ink.primary\" = \"$palette.mist\"\naccent = \"$palette.ice\"\n",
        diags);
    CHECK(!has_errors_in(diags));

    // Every name in the table resolved, not only the ones the pack named.
    CHECK_EQ(theme.ui.size(), theme_ui_color_names().size());
    CHECK_STREQ(hex(theme.ui_color("WindowBg")), "#0B0C0F");
    CHECK_STREQ(hex(theme.ui_color("Text")), "#C7CCD6");
    CHECK_STREQ(hex(theme.ui_color("Button")), "#7AA2F7");

    // Derived states are visibly different from what they derive from, which is
    // the property that makes a hover state a hover state.
    CHECK(theme.ui_color("ButtonHovered") != theme.ui_color("Button"));
    CHECK(theme.ui_color("ButtonActive") != theme.ui_color("Button"));
    CHECK(color_contrast(theme.ui_color("Text"), theme.ui_color("WindowBg")) > 7.0f);
}

TEST(an_explicit_colour_beats_its_derivation) {
    Diagnostics diags;
    const ResolvedTheme theme = resolve(
        "[roles]\naccent = \"#7aa2f7\"\n[colors]\nButton = \"#ff0000\"\n", diags);
    CHECK_STREQ(hex(theme.ui_color("Button")), "#FF0000");
    // and only that one: its neighbours still track the role.
    CHECK_STREQ(hex(theme.ui_color("Header")), "#7AA2F7");
}

TEST(a_role_may_reference_a_role_written_below_it) {
    // Resolution retries what referred forwards, so the order keys land in the
    // file (alphabetical, in a TOML table) cannot decide whether a pack works.
    Diagnostics diags;
    const ResolvedTheme theme =
        resolve("[roles]\n\"surface.raised\" = \"lighten($surface.sunken, 20%)\"\n"
                "\"surface.sunken\" = \"#101010\"\n",
                diags);
    CHECK(!has_errors_in(diags));
    CHECK(theme.role("surface.raised") != theme.role("surface.sunken"));
}

TEST(a_role_may_tweak_the_value_it_inherits) {
    // The idiom for "the parent's accent, a little lighter". It reads as a
    // self-reference, and it resolves against the inherited value rather than
    // being called a cycle.
    Diagnostics diags;
    ThemeResolveContext context;
    ThemePack parent;
    CHECK(parse_theme_pack_text(pack_text("[roles]\naccent = \"#404040\"\n"), "parent", parent,
                                diags));
    context.available["parent"] = parent;

    ThemePack child;
    CHECK(parse_theme_pack_text(pack_text("[roles]\naccent = \"lighten($accent, 20%)\"\n",
                                          "parent"),
                                "child", child, diags));
    ResolvedTheme theme;
    CHECK(resolve_theme(child, context, theme, diags));
    CHECK(!has_errors_in(diags));
    CHECK(color_contrast(theme.role("accent"), 0x404040FFu) > 1.4f);
}

TEST(a_role_cycle_falls_back_to_the_base_theme) {
    Diagnostics diags;
    ThemePack pack;
    CHECK(parse_theme_pack_text(pack_text("[roles]\naccent = \"$accent.hover\"\n"
                                          "\"accent.hover\" = \"$accent\"\n"),
                                "x", pack, diags));
    ResolvedTheme theme;
    CHECK(!resolve_theme(pack, ThemeResolveContext{}, theme, diags));
    CHECK(has_errors_in(diags));
    // Refused, but usable: a cosmetic mistake cannot stop the app.
    CHECK(theme.builtin);
    CHECK_STREQ(theme.id, "dark");
    CHECK(theme.role("accent") != 0u);
}

TEST(inherit_walks_packs_and_a_child_overrides_its_parent) {
    Diagnostics diags;
    ThemeResolveContext context;

    ThemePack parent;
    CHECK(parse_theme_pack_text(
        pack_text("[roles]\naccent = \"#111111\"\n\"ink.primary\" = \"#eeeeee\"\n"), "parent",
        parent, diags));
    context.available["parent"] = parent;

    ThemePack child;
    CHECK(parse_theme_pack_text(pack_text("[roles]\naccent = \"#222222\"\n", "parent"), "child",
                                child, diags));

    ResolvedTheme theme;
    CHECK(resolve_theme(child, context, theme, diags));
    CHECK(!has_errors_in(diags));
    CHECK_STREQ(hex(theme.role("accent")), "#222222");     // the child's
    CHECK_STREQ(hex(theme.role("ink.primary")), "#EEEEEE"); // inherited

    // And the child's role change re-derives the parent's widget colours rather
    // than leaving them where the parent computed them.
    CHECK_STREQ(hex(theme.ui_color("Button")), "#222222");
}

TEST(an_inherit_cycle_is_refused) {
    Diagnostics diags;
    ThemeResolveContext context;
    ThemePack a, b;
    CHECK(parse_theme_pack_text(pack_text("", "b"), "a", a, diags));
    CHECK(parse_theme_pack_text(pack_text("", "a"), "b", b, diags));
    context.available["a"] = a;
    context.available["b"] = b;

    ResolvedTheme theme;
    CHECK(!resolve_theme(a, context, theme, diags));
    CHECK(has_errors_in(diags));
    CHECK(theme.builtin);
}

TEST(inheriting_a_pack_that_is_not_installed_warns_and_uses_dark) {
    Diagnostics diags;
    ThemePack pack;
    CHECK(parse_theme_pack_text(pack_text("", "gone"), "x", pack, diags));
    ResolvedTheme theme;
    CHECK(resolve_theme(pack, ThemeResolveContext{}, theme, diags));
    CHECK(!has_errors_in(diags));
    CHECK_EQ(diags.size(), std::size_t{1});
    CHECK(diags[0].severity == Severity::Warning);
    // Dark's roles, but still this pack's identity.
    CHECK_STREQ(theme.id, "x");
    CHECK_EQ(theme.role("surface.base"), builtin_theme_roles("dark").at("surface.base"));
}

TEST(inverted_ink_picks_the_side_that_can_be_read) {
    Diagnostics diags;
    // A pale accent needs the dark surface on it...
    const ResolvedTheme pale =
        resolve("[roles]\n\"surface.base\" = \"#101010\"\n\"ink.primary\" = \"#f0f0f0\"\n"
                "accent = \"#ffe680\"\n",
                diags);
    CHECK(color_contrast(pale.role("ink.inverted"), pale.role("accent")) > 4.5f);
    CHECK_STREQ(hex(pale.role("ink.inverted")), "#101010");

    // ...and a deep one needs the light ink.
    const ResolvedTheme deep =
        resolve("[roles]\n\"surface.base\" = \"#101010\"\n\"ink.primary\" = \"#f0f0f0\"\n"
                "accent = \"#1a2a6c\"\n",
                diags);
    CHECK_STREQ(hex(deep.role("ink.inverted")), "#F0F0F0");

    // Stated explicitly, the pack wins over the choice.
    const ResolvedTheme named =
        resolve("[roles]\naccent = \"#ffe680\"\n\"ink.inverted\" = \"#ff00ff\"\n", diags);
    CHECK_STREQ(hex(named.role("ink.inverted")), "#FF00FF");
}

TEST(appearance_is_inferred_from_the_surface_when_absent) {
    Diagnostics diags;
    CHECK_STREQ(resolve("[roles]\n\"surface.base\" = \"#fafafa\"\n", diags).appearance, "light");
    CHECK_STREQ(resolve("[roles]\n\"surface.base\" = \"#0a0a0a\"\n", diags).appearance, "dark");
}

TEST(a_partial_syntax_block_and_a_missing_one_agree_about_the_rest) {
    Diagnostics diags;
    const ResolvedTheme none = resolve("", diags);
    const ResolvedTheme some = resolve("[syntax]\nkeyword = \"#ff00ff\"\n", diags);

    CHECK_STREQ(hex(some.syntax[static_cast<std::size_t>(TokenKind::Keyword)]), "#FF00FF");
    for (std::size_t i = 0; i < kTokenKindCount; ++i) {
        if (i == static_cast<std::size_t>(TokenKind::Keyword)) continue;
        CHECK_EQ(some.syntax[i], none.syntax[i]);
    }

    // Alpha is ignored here: the editor draws text, and a half-transparent
    // keyword reads as a rendering bug rather than as a style.
    const ResolvedTheme faded = resolve("[syntax]\nkeyword = \"alpha(#ff00ff, 30%)\"\n", diags);
    CHECK_EQ(faded.syntax[static_cast<std::size_t>(TokenKind::Keyword)] & 0xFFu, 0xFFu);
}

TEST(the_graph_keeps_the_colours_it_already_had) {
    Diagnostics diags;
    const ResolvedTheme theme = resolve("", diags);
    // The literals graph_panel.cpp drew before packs existed.
    CHECK_STREQ(hex(theme.graph_color("pin.float")), "#A0C8FF");
    CHECK_STREQ(hex(theme.graph_color("node.math")), "#3A4A60");
    CHECK_STREQ(hex(theme.graph_color("node.output")), "#6E4A38");

    // A pack names them under [graph.pins] and [graph.nodes].
    const ResolvedTheme painted = resolve(
        "[graph.pins]\nfloat = \"#ff0000\"\n[graph.nodes]\nmath = \"#00ff00\"\n", diags);
    CHECK_STREQ(hex(painted.graph_color("pin.float")), "#FF0000");
    CHECK_STREQ(hex(painted.graph_color("node.math")), "#00FF00");
    // Unnamed ones keep their default rather than going black.
    CHECK_STREQ(hex(painted.graph_color("pin.bool")), "#C8A0F0");
}

TEST(a_builtin_theme_resolves_its_own_colours_without_a_pack) {
    const ResolvedTheme dark = builtin_resolved_theme("dark");
    CHECK(dark.builtin);
    // Widget colours stay ImGui's, so the built-in look cannot drift...
    CHECK(dark.ui.empty());
    // ...but the app's own colours are filled either way, which is what gives
    // the panels one code path.
    CHECK(!dark.diagnostics.empty());
    CHECK(!dark.graph.empty());
    CHECK(!dark.preview.empty());
    CHECK(dark.syntax[0] != 0u);

    const ResolvedTheme light = builtin_resolved_theme("light");
    CHECK_STREQ(light.appearance, "light");
    // An unknown id opens as dark rather than as nothing.
    CHECK_STREQ(builtin_resolved_theme("nonsense").id, "dark");
}

TEST(a_bad_value_costs_its_own_key_and_nothing_else) {
    Diagnostics diags;
    const ResolvedTheme theme = resolve(
        "[roles]\naccent = \"#7aa2f7\"\n"
        "[colors]\nButton = \"$nothing\"\nHeader = \"#010203\"\n",
        diags);
    CHECK(!has_errors_in(diags));
    CHECK_STREQ(hex(theme.ui_color("Header")), "#010203");
    // Button fell back to its derivation rather than to black or to nothing.
    CHECK_STREQ(hex(theme.ui_color("Button")), "#7AA2F7");
}

TEST(the_palette_may_only_be_hex) {
    Diagnostics diags;
    ThemePack pack;
    CHECK(parse_theme_pack_text(pack_text("[palette]\nice = \"lighten(#7aa2f7, 5%)\"\n"), "x",
                                pack, diags));
    ResolvedTheme theme;
    CHECK(resolve_theme(pack, ThemeResolveContext{}, theme, diags));
    // A warning, not an error: the pack loads without that palette entry.
    CHECK(!has_errors_in(diags));
    CHECK_EQ(diags.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Lint
// ---------------------------------------------------------------------------

TEST(lint_finds_text_nobody_can_read) {
    Diagnostics diags;
    const ResolvedTheme bad = resolve(
        "[roles]\n\"surface.base\" = \"#303030\"\n\"ink.primary\" = \"#3a3a3a\"\n", diags);
    const Diagnostics findings = lint_theme(bad);
    CHECK(!findings.empty());
    CHECK(!has_errors(findings));  // advice about a theme, never a refusal

    bool mentions_ink = false;
    for (const auto& f : findings) {
        if (f.message.find("ink.primary on surface.base") != std::string::npos) mentions_ink = true;
    }
    CHECK(mentions_ink);

    // A theme built from a sane palette has nothing to say about its ink.
    const ResolvedTheme good = resolve(
        "[roles]\n\"surface.base\" = \"#0b0c0f\"\n\"ink.primary\" = \"#e8ecf2\"\n"
        "accent = \"#7aa2f7\"\n",
        diags);
    for (const auto& f : lint_theme(good)) {
        CHECK(f.message.find("ink.primary on surface.base") == std::string::npos);
    }
}

TEST(lint_passes_the_themes_this_build_ships) {
    // The calibration test. Thresholds that fail the app's own themes are
    // miscalibrated rather than insightful, so every built-in has to come back
    // clean - and a theme has to be worse than the ones shipped before it is
    // worth telling anyone about.
    for (const char* id : {"dark", "light", "classic"}) {
        const Diagnostics findings = lint_theme(builtin_resolved_theme(id));
        for (const auto& f : findings) {
            // Spelled out so a failure names the theme and the finding.
            ::test::fail(__FILE__, __LINE__, std::string(id) + ": " + f.message);
        }
    }
}

TEST(lint_holds_comments_to_a_lower_bar_than_code) {
    // Comments are dimmed on purpose in every palette here, so the body-text
    // threshold would flag all three of them. The kinds that carry meaning are
    // still held to it.
    Diagnostics diags;
    const ResolvedTheme theme = resolve(
        "[roles]\n\"surface.base\" = \"#101014\"\n\"surface.sunken\" = \"#101014\"\n"
        "[syntax]\ncomment = \"#5a6070\"\nkeyword = \"#5a6070\"\n",
        diags);
    const Diagnostics findings = lint_theme(theme);

    bool flagged_keyword = false;
    bool flagged_comment = false;
    for (const auto& f : findings) {
        if (f.message.find("syntax.keyword") != std::string::npos) flagged_keyword = true;
        if (f.message.find("syntax.comment") != std::string::npos) flagged_comment = true;
    }
    CHECK(flagged_keyword);
    CHECK(!flagged_comment);
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------

TEST(a_watch_answers_once_per_edit) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ssstudio_theme_watch_test.s3theme";
    {
        std::ofstream out(path);
        out << "[pack]\nformat = 1\nname = \"W\"\n";
    }

    ThemeWatch watch;
    watch.reset(path);
    CHECK(watch.watching);
    // Nothing has happened yet, and asking twice does not invent a change.
    CHECK(!watch.changed());
    CHECK(!watch.changed());

    // A save. The answer is yes exactly once: a second yes would mean the app
    // re-read and re-applied the same file on every focus for ever after.
    const auto later = std::filesystem::last_write_time(path) + std::chrono::seconds(2);
    std::filesystem::last_write_time(path, later);
    CHECK(watch.changed());
    CHECK(!watch.changed());

    // The rule that matters for a text editor saving in two steps: the attempt
    // is recorded whether or not the caller could read the file, so a truncated
    // save is not retried until the completed one arrives with a newer time.
    const auto later_still = later + std::chrono::seconds(2);
    std::filesystem::last_write_time(path, later_still);
    CHECK(watch.changed());   // the caller may well fail to parse this one
    CHECK(!watch.changed());  // and is not told again until the next save
    std::filesystem::last_write_time(path, later_still + std::chrono::seconds(2));
    CHECK(watch.changed());

    // A file that is gone is not a change - it is a file being written or moved.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    CHECK(!watch.changed());

    // A built-in theme has no file, and that stops the watch rather than
    // leaving it pointed at the last pack.
    watch.reset({});
    CHECK(!watch.watching);
    CHECK(!watch.changed());
}

TEST(the_same_root_named_twice_is_read_once) {
    // A macOS bundle reports its Resources directory as where the executable
    // lives, so two of the three search roots resolve to one directory. Reading
    // it twice would parse every pack twice and double any warning it carries.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ssstudio_theme_roots_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    {
        std::ofstream out(root / "one.s3theme");
        out << "[pack]\nformat = 1\nname = \"One\"\n";
    }

    // Spelled differently on purpose: the same place, by two names.
    const auto found = theme_pack_paths({root, root / "." , root});
    CHECK_EQ(found.size(), std::size_t{1});

    std::filesystem::remove_all(root, ec);
}
