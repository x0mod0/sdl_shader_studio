#include "ssstudio/keys.h"
#include "test.h"

using namespace ssstudio;

TEST(keys_are_stable_for_the_same_id) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    const auto a = assign_keys({{"sprite_vert", std::nullopt}}, KeyStrategy::Hash, false, d, &pins);
    const auto b = assign_keys({{"sprite_vert", std::nullopt}}, KeyStrategy::Hash, false, d, &pins);
    CHECK_EQ(a[0], b[0]);
    CHECK(!has_errors(d));
}

TEST(keys_do_not_move_when_a_shader_is_added) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    const auto before = assign_keys({{"a", std::nullopt}, {"b", std::nullopt}}, KeyStrategy::Hash,
                                    false, d, &pins);
    const auto after = assign_keys({{"a", std::nullopt}, {"new", std::nullopt}, {"b", std::nullopt}},
                                   KeyStrategy::Hash, false, d, &pins);
    CHECK_EQ(before[0], after[0]);
    CHECK_EQ(before[1], after[2]);
}

TEST(pinned_keys_always_win) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    const auto keys = assign_keys({{"a", 7u}, {"b", std::nullopt}}, KeyStrategy::Enum, false, d,
                                  &pins);
    CHECK_EQ(keys[0], 7u);
    CHECK(keys[1] != 7u);
}

TEST(enum_strategy_is_sequential_and_skips_pins) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    const auto keys =
        assign_keys({{"a", std::nullopt}, {"b", 0u}, {"c", std::nullopt}}, KeyStrategy::Enum,
                    false, d, &pins);
    CHECK_EQ(keys[1], 0u);
    CHECK(keys[0] != keys[2]);
    CHECK(keys[0] != 0u);
    CHECK(keys[2] != 0u);
}

TEST(duplicate_pins_are_reported) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    assign_keys({{"a", 5u}, {"b", 5u}}, KeyStrategy::Explicit, false, d, &pins);
    CHECK(has_errors(d));
}

TEST(explicit_strategy_reports_new_pins) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    assign_keys({{"a", std::nullopt}, {"b", 3u}}, KeyStrategy::Explicit, false, d, &pins);
    CHECK_EQ(pins.size(), std::size_t{1});
    CHECK(pins.count("a") == 1);
    CHECK(pins.count("b") == 0);
}

TEST(key16_layout_narrows_keys) {
    Diagnostics d;
    std::map<std::string, std::uint32_t> pins;
    const auto keys = assign_keys({{"some_long_shader_id", std::nullopt}}, KeyStrategy::Hash, true,
                                  d, &pins);
    CHECK(keys[0] <= 0xFFFFu);
}

TEST(enum_names_are_valid_identifiers) {
    CHECK_STREQ(enum_name("SHADER", "sprite_frag"), "SHADER_SPRITE_FRAG");
    CHECK_STREQ(enum_name("SHADER", "post/blur-h"), "SHADER_POST_BLUR_H");
    CHECK_STREQ(enum_name("", "2d_lit"), "_2D_LIT");
}
