#include <cmath>

#include "ssstudio/color.h"
#include "test.h"

using namespace ssstudio;

namespace {

/// Within one step of the 0..255 grid a hex colour is quantised to. Half a step
/// would sit exactly on the boundary for a value like 0.5, which rounds to 128
/// and comes back as 128/255 rather than as itself.
bool near(float a, float b) { return std::fabs(a - b) <= 1.0f / 255.0f; }

}  // namespace

TEST(a_colour_is_written_as_hex) {
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK_STREQ(color_to_hex(red, 3), "#FF0000");
    CHECK_STREQ(color_to_hex(red, 4), "#FF0000FF");

    // 0.75 * 255 is 191.25, which rounds to 191 rather than to a round number.
    const float half[4] = {0.5f, 0.25f, 0.75f, 0.5f};
    CHECK_STREQ(color_to_hex(half, 4), "#8040BF80");
}

TEST(a_colour_out_of_range_is_written_as_the_nearest_one) {
    // A picker cannot produce these, but a shader value bound to one can.
    const float wild[4] = {-2.0f, 4.0f, 0.0f, 1.0f};
    CHECK_STREQ(color_to_hex(wild, 3), "#00FF00");
}

TEST(hex_is_read_back_as_the_colour_it_names) {
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    CHECK(color_from_hex("#FF8000", rgba, 3));
    CHECK(near(rgba[0], 1.0f));
    CHECK(near(rgba[1], 0.5f));
    CHECK(near(rgba[2], 0.0f));

    // Round trips, which is the property the two halves of the widget rely on.
    const float source[4] = {0.13f, 0.42f, 0.87f, 0.61f};
    float back[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(color_from_hex(color_to_hex(source, 4), back, 4));
    for (int i = 0; i < 4; ++i) CHECK(near(back[i], source[i]));
}

TEST(the_hash_and_the_case_and_the_spaces_are_all_optional) {
    float a[3] = {0, 0, 0};
    float b[3] = {0, 0, 0};
    float c[3] = {0, 0, 0};
    CHECK(color_from_hex("#ff8000", a, 3));
    CHECK(color_from_hex("FF8000", b, 3));
    CHECK(color_from_hex("  #Ff8000  ", c, 3));
    for (int i = 0; i < 3; ++i) {
        CHECK(near(a[i], b[i]));
        CHECK(near(a[i], c[i]));
    }
}

TEST(the_short_form_doubles_each_digit) {
    float rgba[3] = {0, 0, 0};
    CHECK(color_from_hex("#abc", rgba, 3));
    float expanded[3] = {0, 0, 0};
    CHECK(color_from_hex("#aabbcc", expanded, 3));
    for (int i = 0; i < 3; ++i) CHECK(near(rgba[i], expanded[i]));
}

TEST(six_digits_leave_an_existing_alpha_alone) {
    // "this colour" and "this colour, this transparent" are different requests.
    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.25f};
    CHECK(color_from_hex("#FFFFFF", rgba, 4));
    CHECK(near(rgba[3], 0.25f));

    CHECK(color_from_hex("#FFFFFF80", rgba, 4));
    CHECK(near(rgba[3], 0.5f));
}

TEST(a_string_that_is_not_a_colour_changes_nothing) {
    float rgba[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float before[4] = {0.1f, 0.2f, 0.3f, 0.4f};

    // Half typed, wrong length, a digit that is not one, and nothing at all.
    CHECK(!color_from_hex("#ff", rgba, 4));
    CHECK(!color_from_hex("#ff000", rgba, 4));
    CHECK(!color_from_hex("#gg0000", rgba, 4));
    CHECK(!color_from_hex("", rgba, 4));
    CHECK(!color_from_hex("#", rgba, 4));
    CHECK(!color_from_hex("#FF0000FFF", rgba, 4));

    for (int i = 0; i < 4; ++i) CHECK(near(rgba[i], before[i]));
}

TEST(eight_digits_into_a_three_component_colour_drop_the_alpha) {
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    CHECK(color_from_hex("#FF000080", rgb, 3));
    CHECK(near(rgb[0], 1.0f));
    CHECK(near(rgb[1], 0.0f));
}

// --- telling a colour from a direction by its name -----------------------

TEST(a_value_named_like_a_colour_is_recognised) {
    CHECK(names_a_color("color"));
    CHECK(names_a_color("colour"));
    CHECK(names_a_color("tint"));
    CHECK(names_a_color("albedo"));
    // However it is spelled and wherever the word sits in the name.
    CHECK(names_a_color("base_color"));
    CHECK(names_a_color("tintColour"));
    CHECK(names_a_color("emissive_rgb"));
    CHECK(names_a_color("BACKGROUND"));
    CHECK(names_a_color("specular_strength"));
}

TEST(a_value_that_only_happens_to_have_three_components_is_not) {
    // The cases the old count-based rule got wrong: all of these are three or
    // four floats, and none of them is a colour.
    CHECK(!names_a_color("normal"));
    CHECK(!names_a_color("position"));
    CHECK(!names_a_color("uv"));
    CHECK(!names_a_color("scale"));
    CHECK(!names_a_color("direction"));
    CHECK(!names_a_color("size"));
    CHECK(!names_a_color(""));
}
