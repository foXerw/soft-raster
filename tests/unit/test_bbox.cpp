#include "math/bbox.h"

#include <cmath>

#include "harness.h"
#include "math/rect.h"
#include "math/vec.h"
#include "test_util.h"

SR_TEST(bbox_grow_accumulates_extremes) {
    sr::BBox b;
    SR_ASSERT(b.empty());
    b.grow(sr::Vec3{1, -2, 3});
    b.grow(sr::Vec3{-4, 5, -6});
    expectVec3Near(b.min, -4, -2, -6, 1e-6f);
    expectVec3Near(b.max, 1, 5, 3, 1e-6f);
    SR_ASSERT_FALSE(b.empty());
}

SR_TEST(bbox_center_and_radius) {
    sr::BBox b;
    b.grow(sr::Vec3{-1, -1, -1});
    b.grow(sr::Vec3{1, 1, 1});
    expectVec3Near(b.center(), 0, 0, 0, 1e-6f);
    SR_ASSERT_NEAR(b.radius(), std::sqrt(3.0f), 1e-5f);
}

SR_TEST(bbox_from_points_matches_manual_grow) {
    const std::vector<sr::Vec3> pts{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {0, 0, 4}};
    const sr::BBox b = sr::BBox::fromPoints(pts);
    expectVec3Near(b.min, 0, 0, 0, 1e-6f);
    expectVec3Near(b.max, 2, 3, 4, 1e-6f);
    SR_ASSERT_NEAR(b.radius(), 0.5f * std::sqrt(4 + 9 + 16), 1e-5f);
    // 空输入：保持 empty，不要返回一个"看起来正常"的盒子
    SR_ASSERT(sr::BBox::fromPoints({}).empty());
}

SR_TEST(bbox_grow_merges_another_box) {
    sr::BBox a;
    a.grow(sr::Vec3{0, 0, 0});
    a.grow(sr::Vec3{1, 1, 1});
    sr::BBox b;
    b.grow(sr::Vec3{-2, 0, 0});
    b.grow(sr::Vec3{0, 0, 5});
    a.grow(b);
    expectVec3Near(a.min, -2, 0, 0, 1e-6f);
    expectVec3Near(a.max, 1, 1, 5, 1e-6f);
}

SR_TEST(rect_clamp_never_returns_an_inverted_range) {
    const sr::Rect whole = sr::Rect{-5, -5, 100, 100}.clampedTo(8, 8);
    SR_ASSERT_EQ(whole.x0, 0);
    SR_ASSERT_EQ(whole.y0, 0);
    SR_ASSERT_EQ(whole.x1, 7);
    SR_ASSERT_EQ(whole.y1, 7);

    const sr::Rect outside = sr::Rect{50, 50, 60, 60}.clampedTo(8, 8);
    SR_ASSERT(outside.empty());
    SR_ASSERT_EQ(outside.width(), 0);

    const sr::Rect inside = sr::Rect{2, 3, 4, 5}.clampedTo(8, 8);
    SR_ASSERT_EQ(inside.x0, 2);
    SR_ASSERT_EQ(inside.y1, 5);
    SR_ASSERT_EQ(inside.width(), 3);
    SR_ASSERT_EQ(inside.height(), 3);
}
