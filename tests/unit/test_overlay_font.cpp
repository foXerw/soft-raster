#include "render/overlay_font.h"

#include <cstdint>
#include <cstring>   // strlen / strcmp（不是 memcmp：见下面那条注释）
#include <string>
#include <vector>

#include "harness.h"
#include "render/color.h"
#include "render/render_target.h"

namespace {

int countSet(const sr::Glyph& g) {
    int n = 0;
    for (int row = 0; row < sr::kGlyphHeightPx; ++row)
        for (int col = 0; col < sr::kGlyphWidthPx; ++col)
            if (g.rows[row][col] == '#') ++n;
    return n;
}

// 就地构造，绝不按值返回：rt 里存的是指向自己成员的裸指针，返回一次就悬垂一次
// （这个坑在 Task 0.5 已经踩过一次，症状是测试进程挂住）。
struct Canvas {
    std::vector<uint8_t> colorMem;
    std::vector<float> depthMem;
    sr::RenderTarget rt;

    explicit Canvas(int w, int h)
        : colorMem(static_cast<size_t>(w) * h * 4u, 0), depthMem(static_cast<size_t>(w) * h, 1.0f) {
        rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(colorMem.data()), w, h, w * 4};
        rt.depth = sr::DepthTarget{depthMem.data(), w, h};
    }
};

}  // namespace

SR_TEST(glyph_rows_are_five_characters_wide) {
    for (unsigned char c = ' '; c <= 'Z'; ++c) {
        const sr::Glyph& g = sr::glyphFor(static_cast<char>(c));
        for (int row = 0; row < sr::kGlyphHeightPx; ++row) {
            SR_ASSERT_EQ(std::strlen(g.rows[row]), sr::kGlyphWidthPx);
        }
    }
}

SR_TEST(digits_and_letters_are_not_blank) {
    for (char c = '0'; c <= '9'; ++c) SR_ASSERT(countSet(sr::glyphFor(c)) >= 8);
    for (char c = 'A'; c <= 'Z'; ++c) SR_ASSERT(countSet(sr::glyphFor(c)) >= 8);
}

SR_TEST(letter_shapes_differ_in_shape_not_just_in_ink_amount) {
    // 检查**具体格子的有无**，而不是墨点数量之比。第一版写的是 "W 的墨点 > 2 倍 I"，
    // 那其实是在猜我自己的字形设计：给 I 加衬线（更可读）就会把它打破，
    // 而加衬线并不是 bug。可读性来自笔画的位置，所以断言位置。
    const sr::Glyph& i = sr::glyphFor('I');
    const sr::Glyph& w = sr::glyphFor('W');
    SR_ASSERT(i.rows[3][0] == '.' && i.rows[3][4] == '.');   // I 的中间行两侧空白
    SR_ASSERT(i.rows[0][0] == '#' && i.rows[6][4] == '#');   // 上下衬线
    SR_ASSERT(w.rows[3][0] == '#' && w.rows[3][4] == '#');   // W 的中间行两侧有笔画
    // W 的两条腿收在中缝两侧（而不是像 M 那样靠边），这是它和 M 的区别所在。
    SR_ASSERT(w.rows[6][1] == '#' && w.rows[6][3] == '#');
    SR_ASSERT(w.rows[6][0] == '.' && w.rows[6][4] == '.');
    SR_ASSERT(sr::glyphFor('M').rows[6][0] == '#');
    SR_ASSERT(countSet(w) > countSet(i));                    // 粗细分明仍然成立
    SR_ASSERT(countSet(sr::glyphFor('E')) != countSet(sr::glyphFor('O')));
    // 'O' 是空心圈，'E' 中间那一横是满的。
    SR_ASSERT(sr::glyphFor('O').rows[3][2] == '.');
    SR_ASSERT(sr::glyphFor('E').rows[3][0] == '#');
    SR_ASSERT(sr::glyphFor('E').rows[3][4] == '#');
    // '0' 与 'O' 必须能区分（HUD 里两者都会出现）。
    // 注意：不能 memcmp 那个 rows 数组 —— 它存的是指针，比地址会得到"必然不同"的假通过。
    bool anyRowDiffers = false;
    for (int row = 0; row < sr::kGlyphHeightPx; ++row) {
        if (std::strcmp(sr::glyphFor('0').rows[row], sr::glyphFor('O').rows[row]) != 0) anyRowDiffers = true;
    }
    SR_ASSERT(anyRowDiffers);
}

SR_TEST(lowercase_uses_the_uppercase_glyph) {
    // M0 只做大小写一套，小写直接映射到大写：症状是"文字全是大写"，而不是缺字。
    SR_ASSERT(sr::glyphFor('a').rows[0] == sr::glyphFor('A').rows[0]);
    SR_ASSERT(sr::glyphFor('z').rows[0] == sr::glyphFor('Z').rows[0]);
}

SR_TEST(unknown_printable_falls_back_to_a_box_not_nothing) {
    // 缺字必须**看得见**。静默画成空白的话，HUD 上少了一整段文字你也不会发现。
    const sr::Glyph& g = sr::glyphFor('{');
    SR_ASSERT(countSet(g) >= 8);
    SR_ASSERT(g.rows[0][0] == '#');
    SR_ASSERT(g.rows[6][4] == '#');
}

SR_TEST(nul_and_space_draw_nothing) {
    SR_ASSERT_EQ(countSet(sr::glyphFor('\0')), 0);
    SR_ASSERT_EQ(countSet(sr::glyphFor(' ')), 0);
}

SR_TEST(drawText_paints_foreground_where_the_glyph_has_ink) {
    Canvas c{64, 16};
    c.rt.color.clear(0xFF000000u);
    const uint32_t fg = sr::encodeSRGB({1.0f, 1.0f, 1.0f});
    sr::drawText(c.rt.color, 0, 0, "I", fg);   // 'I' 的顶行是满的 5 格

    const sr::Glyph& i = sr::glyphFor('I');
    for (int col = 0; col < sr::kGlyphWidthPx; ++col) {
        const bool ink = i.rows[0][col] == '#';
        SR_ASSERT_EQ(c.rt.color.readPacked(col, 0) == fg, ink);
    }
    // 字形右侧那一列是字符间距，必须是背景。
    SR_ASSERT_EQ(c.rt.color.readPacked(sr::kGlyphWidthPx, 0), 0xFF000000u);
}

SR_TEST(drawText_with_transparent_background_leaves_the_rest_untouched) {
    Canvas c{64, 16};
    c.rt.color.clear(0xFF112233u);
    sr::drawText(c.rt.color, 4, 2, "8", sr::encodeSRGB({1, 1, 1}));
    // 画布其它地方一个字节都没动
    int changed = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 64; ++x)
            if (c.rt.color.readPacked(x, y) != 0xFF112233u) ++changed;
    SR_ASSERT(changed > 0);
    SR_ASSERT(changed <= sr::kGlyphWidthPx * sr::kGlyphHeightPx);   // 不超过一个字形的外框
    SR_ASSERT_EQ(c.rt.color.readPacked(63, 15), 0xFF112233u);
}

SR_TEST(drawText_with_opaque_background_fills_the_cell) {
    Canvas c{64, 16};
    c.rt.color.clear(0xFF000000u);
    const uint32_t fg = sr::encodeSRGB({1, 1, 1});
    const uint32_t bg = sr::encodeSRGB({0, 0, 1});
    sr::drawText(c.rt.color, 0, 0, ".", fg, bg);
    // '.' 只有右下角一个点，其余 4 格宽 7 行都必须是 bg。
    SR_ASSERT_EQ(c.rt.color.readPacked(0, 0), bg);
    SR_ASSERT_EQ(c.rt.color.readPacked(4, 6), bg);
    SR_ASSERT_EQ(c.rt.color.readPacked(2, 7), bg);   // 第 8 行是行距，也要填
}

SR_TEST(drawText_scale_multiplies_every_pixel_block) {
    Canvas c{64, 32};
    c.rt.color.clear(0xFF000000u);
    const uint32_t fg = sr::encodeSRGB({1, 1, 1});
    sr::drawText(c.rt.color, 0, 0, "I", fg, 0u, 2);
    const sr::Glyph& i = sr::glyphFor('I');
    for (int row = 0; row < sr::kGlyphHeightPx; ++row) {
        for (int col = 0; col < sr::kGlyphWidthPx; ++col) {
            const bool ink = i.rows[row][col] == '#';
            SR_ASSERT_EQ(c.rt.color.readPacked(col * 2, row * 2) == fg, ink);
            SR_ASSERT_EQ(c.rt.color.readPacked(col * 2 + 1, row * 2 + 1) == fg, ink);
        }
    }
}

SR_TEST(text_width_is_advance_times_length) {
    SR_ASSERT_EQ(sr::textWidthPx("", 1), 0);
    SR_ASSERT_EQ(sr::textWidthPx("ABC", 1), 3 * sr::kGlyphAdvancePx);
    SR_ASSERT_EQ(sr::textWidthPx("ABC", 2), 6 * sr::kGlyphAdvancePx);
}

SR_TEST(drawText_clips_to_the_target_without_crashing) {
    Canvas c{8, 8};
    c.rt.color.clear(0xFF000000u);
    sr::drawText(c.rt.color, -3, -3, "HELLO WORLD", sr::encodeSRGB({1, 1, 1}));
    sr::drawText(c.rt.color, 7, 7, "XYZ", sr::encodeSRGB({1, 1, 1}));
    // 越界部分被丢弃，界内仍然可读；这里只要求"没有写到界外"（由 ColorTarget 保证）。
    SR_ASSERT(c.rt.color.readPacked(0, 0) != 0xFF000000u || c.rt.color.readPacked(7, 7) != 0xFF000000u);
}

SR_TEST(drawText_never_touches_depth) {
    Canvas c{40, 12};
    c.rt.depth.clear(0.5f);
    sr::drawText(c.rt.color, 0, 0, "FPS 59.8", sr::encodeSRGB({1, 1, 1}));
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 40; ++x) SR_ASSERT_NEAR(c.rt.depth.read(x, y), 0.5f, 1e-6f);
}
