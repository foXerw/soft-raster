#include "render/viewport.h"

#include <cmath>
#include <cstring>

#include "harness.h"
#include "math/vec.h"
#include "render/vertex.h"

namespace {

sr::ClipVertex clipAt(const sr::Vec4& clip, const sr::Varyings& v) {
    sr::ClipVertex cv;
    cv.clip = clip;
    const float invW = 1.0f / clip.w;
    cv.vOverW.color = v.color * invW;
    cv.vOverW.worldNormal = v.worldNormal * invW;
    cv.vOverW.uv = v.uv * invW;
    cv.vOverW.worldPos = v.worldPos * invW;
    return cv;
}

}  // namespace

SR_TEST(viewport_maps_ndc_corners_to_pixel_corners_top_down) {
    const sr::Viewport vp{0, 0, 640, 360};
    // NDC (-1,-1) 是左下 ⇒ top-down 的第 height 行（C8）
    const sr::ScreenVertex bl = vp.project(clipAt(sr::Vec4{-1, -1, -1, 1}, {}));
    SR_ASSERT_NEAR(bl.pos.x, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(bl.pos.y, 360.0f, 1e-5f);
    SR_ASSERT_NEAR(bl.depth, 0.0f, 1e-6f);

    const sr::ScreenVertex tr = vp.project(clipAt(sr::Vec4{1, 1, 1, 1}, {}));
    SR_ASSERT_NEAR(tr.pos.x, 640.0f, 1e-5f);
    SR_ASSERT_NEAR(tr.pos.y, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(tr.depth, 1.0f, 1e-6f);

    const sr::ScreenVertex c = vp.project(clipAt(sr::Vec4{0, 0, 0, 1}, {}));
    SR_ASSERT_NEAR(c.pos.x, 320.0f, 1e-5f);
    SR_ASSERT_NEAR(c.pos.y, 180.0f, 1e-5f);
    SR_ASSERT_NEAR(c.depth, 0.5f, 1e-6f);

    // NDC 上边缘（y=+1）在 top-down 里是第 0 行：这条是 y 翻转的守门人
    const sr::ScreenVertex top = vp.project(clipAt(sr::Vec4{0, 1, -1, 1}, {}));
    SR_ASSERT_NEAR(top.pos.y, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(top.depth, 0.0f, 1e-6f);
}

SR_TEST(viewport_respects_its_offset) {
    const sr::Viewport vp{10, 20, 100, 50};
    const sr::ScreenVertex c = vp.project(clipAt(sr::Vec4{0, 0, 0, 1}, {}));
    SR_ASSERT_NEAR(c.pos.x, 60.0f, 1e-5f);
    SR_ASSERT_NEAR(c.pos.y, 45.0f, 1e-5f);
}

SR_TEST(viewport_divides_by_w_before_mapping) {
    const sr::Viewport vp{0, 0, 100, 100};
    const sr::ScreenVertex a = vp.project(clipAt(sr::Vec4{2, 2, 0, 2}, {}));
    const sr::ScreenVertex b = vp.project(clipAt(sr::Vec4{1, 1, 0, 1}, {}));
    SR_ASSERT_NEAR(a.pos.x, b.pos.x, 1e-5f);
    SR_ASSERT_NEAR(a.pos.y, b.pos.y, 1e-5f);
    SR_ASSERT_NEAR(a.w, 2.0f, 1e-6f);
    SR_ASSERT_NEAR(a.invW, 0.5f, 1e-6f);
}

SR_TEST(viewport_restores_varyings_from_their_divided_form) {
    const sr::Viewport vp{0, 0, 100, 100};
    sr::Varyings v;
    v.color = sr::Vec4{0.25f, 0.5f, 0.75f, 1.0f};
    v.uv = sr::Vec2{0.1f, 0.9f};
    const sr::ScreenVertex s = vp.project(clipAt(sr::Vec4{0, 0, 0, 4}, v));
    SR_ASSERT_NEAR(s.v.color.x, 0.25f, 1e-6f);
    SR_ASSERT_NEAR(s.v.color.z, 0.75f, 1e-6f);
    SR_ASSERT_NEAR(s.v.uv.x, 0.1f, 1e-6f);
    SR_ASSERT_NEAR(s.v.uv.y, 0.9f, 1e-6f);
}

SR_TEST(varyings_are_laid_out_as_twelve_floats) {
    // PlaneEq 按 float 分量建数组（M3），所以 Varyings 必须全 float 无填充。
    SR_ASSERT_EQ(sizeof(sr::Varyings), 12u * sizeof(float));
    sr::Varyings v;
    v.color = sr::Vec4{1, 2, 3, 4};
    v.worldNormal = sr::Vec3{5, 6, 7};
    v.uv = sr::Vec2{8, 9};
    v.worldPos = sr::Vec3{10, 11, 12};
    const float* f = sr::varyingFloats(v);
    SR_ASSERT_NEAR(f[0], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(f[3], 4.0f, 1e-6f);
    SR_ASSERT_NEAR(f[4], 5.0f, 1e-6f);
    SR_ASSERT_NEAR(f[7], 8.0f, 1e-6f);
    SR_ASSERT_NEAR(f[9], 10.0f, 1e-6f);
    SR_ASSERT_NEAR(f[11], 12.0f, 1e-6f);
    // 字段名用于报错信息
    SR_ASSERT(std::strcmp(sr::varyingFieldName(0), "color.x") == 0);
    SR_ASSERT(std::strcmp(sr::varyingFieldName(8), "uv.y") == 0);
}
