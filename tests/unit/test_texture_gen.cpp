#include <cstddef>
#include <cstdint>

#include "harness.h"
#include "render/texture.h"

namespace {

// 为什么这个文件存在：M4.7 的端到端测试要靠"贴图朝向"来判定 uv 有没有被转置/翻转，
// 而**纯棋盘格在转置下是恒等的**（(cx+cy)&1 对称），在 uv 翻转下也只是"明暗对调"。
// 一张自身带对称性的贴图，会让"uv 轴接反"这类错误永远红不起来 —— 与 4.5 的方形足迹、
// 4.3 的等差值、4.2 的整数乘积同族（账本里那条"夹具在某个对称操作下不变"）。
// 所以 checker 有三个角标记色，第四角留素棋盘 ⇒ 正方形的任何对称都不保持这张图。

const sr::CheckerPalette kPal{};

const uint8_t* pixelAt(const sr::ImageRGBA8& img, int x, int y) {
    // 越界即返回 nullptr：测试自己不许越界索引自己的输出（Ruling 26），
    // 一次越界读会把整个测试进程带走，连同所有还没打印的结果。
    if (img.width <= 0 || img.height <= 0) return nullptr;
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return nullptr;
    return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.width) +
                      static_cast<size_t>(x)) * 4u];
}

bool sameRGBA(const uint8_t* a, const uint8_t* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

int countMatching(const sr::ImageRGBA8& img, const uint8_t want[4]) {
    int n = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            if (sameRGBA(pixelAt(img, x, y), want)) ++n;
        }
    }
    return n;
}

// 把图片按 fn 重排后逐像素比对原图，返回"不一样"的像素数。fn 收 (x,y,w,h) 给出重排后的坐标。
template <class Fn>
int countDifferingUnder(const sr::ImageRGBA8& img, Fn fn) {
    int n = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            int sx = 0, sy = 0;
            fn(x, y, img.width, img.height, sx, sy);
            if (!sameRGBA(pixelAt(img, x, y), pixelAt(img, sx, sy))) ++n;
        }
    }
    return n;
}

void expectPixel(const uint8_t* got, const uint8_t want[4], const char* what) {
    if (!sameRGBA(got, want)) {
        std::printf("[checker] %s 得到 %d,%d,%d 期望 %d,%d,%d\n", what, got ? got[0] : -1,
                    got ? got[1] : -1, got ? got[2] : -1, want[0], want[1], want[2]);
    }
    SR_ASSERT(sameRGBA(got, want));
}

}  // namespace

SR_TEST(checker_image_size_is_cells_times_cell_pixels) {
    const sr::ImageRGBA8 img = sr::makeCheckerImage(4, 8, kPal);
    SR_ASSERT_EQ(img.width, 32);
    SR_ASSERT_EQ(img.height, 32);
    SR_ASSERT_EQ(img.rgba.size(), 32u * 32u * 4u);
    // alpha 必须是 255：这张图是"颜色数据"，半透明会让 mip 平均混进背景的想象。
    const uint8_t* p = pixelAt(img, 17, 5);
    SR_ASSERT(p != nullptr);
    if (p != nullptr) SR_ASSERT_EQ(p[3], 255);
}

SR_TEST(checker_alternates_and_marks_three_corners) {
    const sr::ImageRGBA8 img = sr::makeCheckerImage(4, 8, kPal);
    // 每格的中心像素 = cx*cellPx + cellPx/2
    const auto center = [&img](int cx, int cy) { return pixelAt(img, cx * 8 + 4, cy * 8 + 4); };

    // 三个标记角：uv(0,0) 左上、uv(1,0) 右上、uv(0,1) 左下（图片行主序、top-down，
    // 所以 v 的正方向在图像里是"往下"，与 ColorTarget 的 top-down 约定一致）。
    expectPixel(center(0, 0), kPal.origin, "cell(0,0) 应为 uv 原点标记");
    expectPixel(center(3, 0), kPal.uAxis, "cell(3,0) 应为 u 轴末端标记");
    expectPixel(center(0, 3), kPal.vAxis, "cell(0,3) 应为 v 轴末端标记");
    // 第四角是**素棋盘**：parity 约定是 ((cx+cy)&1)==0 ⇒ dark，(3+3)&1==0。
    // 这一条同时钉住了"标记不会覆盖到对角"与奇偶约定本身。
    expectPixel(center(3, 3), kPal.dark, "cell(3,3) 应为素棋盘 dark");
    // 内部格的奇偶交替（三个都不是标记色 ⇒ 证明标记只碰了三个角）。
    expectPixel(center(1, 0), kPal.light, "cell(1,0) 应为 light");
    expectPixel(center(1, 1), kPal.dark, "cell(1,1) 应为 dark");
    expectPixel(center(2, 3), kPal.light, "cell(2,3) 应为 light");
}

SR_TEST(exactly_three_cells_carry_a_marker) {
    const int cells = 4, cellPx = 8;
    const sr::ImageRGBA8 img = sr::makeCheckerImage(cells, cellPx, kPal);
    const int oneCell = cellPx * cellPx;
    SR_ASSERT_EQ(countMatching(img, kPal.origin), oneCell);
    SR_ASSERT_EQ(countMatching(img, kPal.uAxis), oneCell);
    SR_ASSERT_EQ(countMatching(img, kPal.vAxis), oneCell);
    // 16 格里 3 格是标记 ⇒ 剩 13 格按奇偶分：dark 占 7（含 (3,3)）、light 占 6。
    SR_ASSERT_EQ(countMatching(img, kPal.dark), 7 * oneCell);
    SR_ASSERT_EQ(countMatching(img, kPal.light), 6 * oneCell);
}

SR_TEST(no_symmetry_of_the_square_preserves_the_image) {
    const int cellPx = 8;
    const sr::ImageRGBA8 img = sr::makeCheckerImage(4, cellPx, kPal);
    const int oneCell = cellPx * cellPx;
    // 正方形的对称群有 8 个元素，恒等那个不必查，所以**这里必须正好 7 条**：
    // 名字承诺"没有任何对称保持这张图"，只查 4 条就是名字比断言大
    // （终审点出的正是这条：90°/270° 与反对角镜像当时没查，只是被 180° 间接盖住）。
    // 每一条都要求"至少一整格被搬走"：>0 只能说明不是恒等，>= oneCell 才说明
    // 是某个**角标记**换到了别处（那正是 uv 接反时会发生的事）。
    struct Sym { const char* name; int kind; };
    const Sym syms[7] = {
        {"转置（主对角镜像）⇔ u/v 对调", 0},
        {"u 镜像 ⇔ u → 1-u", 1},
        {"v 镜像 ⇔ v → 1-v", 2},
        {"180° 旋转", 3},
        {"90° 旋转", 4},
        {"270° 旋转", 5},
        {"反对角镜像", 6},
    };
    for (const Sym& s : syms) {
        int differing = 0;
        switch (s.kind) {
            case 0: differing = countDifferingUnder(img, [](int x, int y, int, int, int& sx, int& sy) { sx = y; sy = x; }); break;
            case 1: differing = countDifferingUnder(img, [](int x, int y, int w, int, int& sx, int& sy) { sx = w - 1 - x; sy = y; }); break;
            case 2: differing = countDifferingUnder(img, [](int x, int y, int, int h, int& sx, int& sy) { sx = x; sy = h - 1 - y; }); break;
            case 3: differing = countDifferingUnder(img, [](int x, int y, int w, int h, int& sx, int& sy) { sx = w - 1 - x; sy = h - 1 - y; }); break;
            case 4: differing = countDifferingUnder(img, [](int x, int y, int w, int, int& sx, int& sy) { sx = y; sy = w - 1 - x; }); break;
            case 5: differing = countDifferingUnder(img, [](int x, int y, int, int h, int& sx, int& sy) { sx = h - 1 - y; sy = x; }); break;
            default: differing = countDifferingUnder(img, [](int x, int y, int w, int h, int& sx, int& sy) { sx = w - 1 - y; sy = h - 1 - x; }); break;
        }
        if (differing < oneCell) std::printf("[checker] %s 只搬动了 %d 个像素（< 一整格 %d）\n",
                                             s.name, differing, oneCell);
        SR_ASSERT(differing >= oneCell);
    }
}

SR_TEST(single_cell_checker_is_entirely_the_origin_marker) {
    // cells==1 时三个标记都指向同一格：优先级必须是**原点**（它是最有用的那个 ——
    // 分不清"哪个角是 uv 原点"是贴图接错的第一症状，而 u/v 末端标记在这一格上没有意义）。
    const sr::ImageRGBA8 img = sr::makeCheckerImage(1, 4, kPal);
    SR_ASSERT_EQ(img.width, 4);
    SR_ASSERT_EQ(img.height, 4);
    SR_ASSERT_EQ(countMatching(img, kPal.origin), 16);
    SR_ASSERT_EQ(countMatching(img, kPal.uAxis), 0);
    SR_ASSERT_EQ(countMatching(img, kPal.vAxis), 0);
}

SR_TEST(two_cell_checker_has_no_plain_cell) {
    // cells==2：四个角恰好被三个标记 + 一个素格占满，parity 仍要按 (cx+cy)&1==0 ⇒ dark。
    const sr::ImageRGBA8 img = sr::makeCheckerImage(2, 4, kPal);
    SR_ASSERT_EQ(countMatching(img, kPal.origin), 16);
    SR_ASSERT_EQ(countMatching(img, kPal.uAxis), 16);
    SR_ASSERT_EQ(countMatching(img, kPal.vAxis), 16);
    SR_ASSERT_EQ(countMatching(img, kPal.dark), 16);   // cell(1,1)
    SR_ASSERT_EQ(countMatching(img, kPal.light), 0);
}

SR_TEST(non_positive_arguments_give_an_empty_image) {
    // 与 downsampleBox2 的兜底一致：非法尺寸 ⇒ 空图（width 0），由调用方判断，
    // 而不是分配一个负数大小的 vector 或者崩掉。
    const sr::ImageRGBA8 a = sr::makeCheckerImage(0, 8, kPal);
    SR_ASSERT_EQ(a.width, 0);
    SR_ASSERT_EQ(a.height, 0);
    SR_ASSERT_EQ(a.rgba.size(), 0u);
    const sr::ImageRGBA8 b = sr::makeCheckerImage(4, 0, kPal);
    SR_ASSERT_EQ(b.width, 0);
    SR_ASSERT_EQ(b.height, 0);
    SR_ASSERT_EQ(b.rgba.size(), 0u);
    const sr::ImageRGBA8 c = sr::makeCheckerImage(-3, 8, kPal);
    SR_ASSERT_EQ(c.width, 0);
    SR_ASSERT_EQ(c.rgba.size(), 0u);
}
