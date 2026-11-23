#include "loader/mtl.h"

#include <string>
#include <string_view>
#include <vector>

#include "harness.h"

namespace {

// 内联字符串夹具，无 IO（D2）：每次调用都给一套全新的 out/warnings/stats/err，
// 这样"out 清空""err 非空"这类断言不会被上一个用例的残留污染。
struct ParseResult {
    std::vector<sr::Material> out;
    std::vector<std::string> warnings;
    sr::MtlStats stats;
    std::string err;
    bool ok = false;
};

ParseResult parse(std::string_view text) {
    ParseResult r;
    r.ok = sr::parseMtl(text, r.out, r.warnings, r.stats, r.err);
    return r;
}

}  // namespace

SR_TEST(mtl_parses_newmtl_blocks_in_file_order) {
    const ParseResult r = parse(
        "newmtl first\n"
        "Kd 1 0 0\n"
        "newmtl second\n"
        "Kd 0 1 0\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 2);
    SR_ASSERT(r.out[0].name == "first");
    SR_ASSERT(r.out[1].name == "second");
    // 顺序就是文件顺序：parseObj 用 usemtl 的名字去查，乱序会让材质张冠李戴。
    SR_ASSERT_NEAR(r.out[0].diffuse.r, 1.0f, 1e-5f);
    SR_ASSERT_NEAR(r.out[0].diffuse.g, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(r.out[1].diffuse.g, 1.0f, 1e-5f);
}

SR_TEST(mtl_parses_kd_as_srgb_encoded_display_value) {
    // Kd 是 display-referred 的浮点，必须走浮点 sRGB 解码（D5）。0.5 → ≈0.21404，
    // 不是 0.5（当成线性）也不是 0.25（平方近似）。变异①（直接 return v）会让这条红。
    const ParseResult r = parse(
        "newmtl m\n"
        "Kd 0.5 0.5 0.5\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    SR_ASSERT_NEAR(r.out[0].diffuse.r, 0.21404f, 1e-4f);
    SR_ASSERT_NEAR(r.out[0].diffuse.g, 0.21404f, 1e-4f);
    SR_ASSERT_NEAR(r.out[0].diffuse.b, 0.21404f, 1e-4f);
}

SR_TEST(mtl_parses_ka_ks_ns_d_and_tr) {
    // Ka/Ks 与 Kd 用同一个浮点解码（三者都是 display-referred）；Ns 原样存；
    // d 是 opacity、Tr 是 transparency，方向相反（Ruling 52）：d 0.3 == Tr 0.7。
    // 范围（终审 M9）：越界的 d/Tr 在解析时夹到 [0,1] —— d 2.0 ⇒ 1.0、d -0.5 ⇒ 0.0、
    // Tr -0.5 ⇒ 1-(-0.5)=1.5 ⇒ 1.0。方向与范围各钉各的：删 clamp 会让越界三条红，
    // 而 Ruling 52 的方向断言（d 0.3 == Tr 0.7）不受影响。M6 消费时须重审语义（mtl.h 注释）。
    const ParseResult r = parse(
        "newmtl m\n"
        "Ka 0.1 0.2 0.3\n"
        "Ks 0.4 0.5 0.6\n"
        "Ns 250\n"
        "d 0.3\n"
        "newmtl t\n"
        "Tr 0.7\n"
        "newmtl hi\nd 2.0\n"
        "newmtl lo\nd -0.5\n"
        "newmtl trneg\nTr -0.5\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 5);
    // 参照值用生产解码函数算：float 解码本身已由 test_color 独立钉过 IEC 中点，
    // 这里要钉的是"Ka/Ks 确实路由进了解码"，而不是把原始 0.1 直接存进去。
    SR_ASSERT_NEAR(r.out[0].ambient.r, sr::srgbToLinear(0.1f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].ambient.g, sr::srgbToLinear(0.2f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].ambient.b, sr::srgbToLinear(0.3f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].specular.r, sr::srgbToLinear(0.4f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].specular.g, sr::srgbToLinear(0.5f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].specular.b, sr::srgbToLinear(0.6f), 1e-5f);
    SR_ASSERT_NEAR(r.out[0].shininess, 250.0f, 1e-5f);  // Ns 原样，0–1000 指数
    SR_ASSERT_NEAR(r.out[0].opacity, 0.3f, 1e-5f);      // d X ⇒ opacity = X
    SR_ASSERT_NEAR(r.out[1].opacity, 0.3f, 1e-5f);      // Tr X ⇒ opacity = 1 - X
    // 范围夹钳（M9）：越界值不许原样存（d 2.0 存成 2.0 会让 M6 的 blend 拿到非法 alpha）。
    // SR_ASSERT 失败不中止：out 被清空时继续索引就是越界（trap (b)），先守住尺寸再取。
    if (r.out.size() == 5) {
        SR_ASSERT_NEAR(r.out[2].opacity, 1.0f, 1e-6f);      // d 2.0 ⇒ clamp 1
        SR_ASSERT_NEAR(r.out[3].opacity, 0.0f, 1e-6f);      // d -0.5 ⇒ clamp 0
        SR_ASSERT_NEAR(r.out[4].opacity, 1.0f, 1e-6f);      // Tr -0.5 ⇒ 1.5 ⇒ clamp 1
    }
}

SR_TEST(default_material_is_white_not_gray) {
    // baseColor 是乘上去的 tint（golden_scene.cpp:110-112）：0.8 灰会让每个无 MTL 的
    // 模型整体暗一档，症状读起来像"光照错了"。所以默认必须纯白 1 1 1。
    const sr::Material& d = sr::defaultMaterial();
    SR_ASSERT_NEAR(d.diffuse.r, 1.0f, 0.0f);
    SR_ASSERT_NEAR(d.diffuse.g, 1.0f, 0.0f);
    SR_ASSERT_NEAR(d.diffuse.b, 1.0f, 0.0f);
    SR_ASSERT_NEAR(d.ambient.r, 0.0f, 0.0f);
    SR_ASSERT_NEAR(d.specular.r, 0.0f, 0.0f);
    SR_ASSERT_NEAR(d.opacity, 1.0f, 0.0f);
    SR_ASSERT_NEAR(d.shininess, 0.0f, 0.0f);
    SR_ASSERT(d.diffuseMap.empty());
    // 单例：返回的引用进程生命周期内有效，两次取地址必须相同（M5.2 会持有它）。
    SR_ASSERT(&sr::defaultMaterial() == &d);
}

SR_TEST(mtl_unknown_attribute_is_warning_and_count) {
    // 未知指令走 D3：忽略 + 计数 + 警告，不是错误（真实文件天天带 refl/map_Ks 等）。
    const ParseResult r = parse(
        "newmtl m\n"
        "refl -type sphere chrome.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    SR_ASSERT_EQ(r.stats.unrecognized, 1);
    SR_ASSERT(!r.warnings.empty());
}

SR_TEST(mtl_map_kd_skips_blender_style_options) {
    // D17：-s 带 3 个参数，连同参数一起丢，文件名是 brick.png。
    // 变异②（把 -s 后的 3 个数字当文件名起点）会让这条红。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -s 1 1 1 brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    SR_ASSERT(r.out[0].diffuseMap == "brick.png");
    SR_ASSERT_EQ(r.stats.optionTokensSkipped, 4);  // -s 1 1 1 共 4 个 token
}

SR_TEST(mtl_map_kd_skips_unknown_option_tokens) {
    // 未知 -xxx 不在 arity 表里：丢它自己 + 紧随其后的数字 token，并计一条警告。
    // arity 表只决定"少吞还是多吞"，不是白名单 —— 未知选项不该让整行报错。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -weird 2 brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT(r.out[0].diffuseMap == "brick.png");
    SR_ASSERT(!r.warnings.empty());
    SR_ASSERT_EQ(r.stats.optionTokensSkipped, 2);  // -weird + 数字 2
}

SR_TEST(mtl_map_kd_keeps_spaces_in_filename) {
    // 第一个不以 - 开头的 token 起，剩余用单空格重拼 = 文件名（保住带空格的贴图名）。
    // 变异③（取最后一个 token）会得到 "brick.png"，让这条红。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -s 2 2 2 my brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT(r.out[0].diffuseMap == "my brick.png");
}

SR_TEST(mtl_map_kd_mm_takes_two_args) {
    // -mm 是 2 个参数（multiplier add），不是 3。原表写成 3 会把 brick.png 当第三个参数
    // 吃掉 ⇒ 剩空串 ⇒ 误判"缺文件名" ⇒ 整个文件加载失败（D17 修订，G-M5.1 要防的那类事）。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -mm 0 1 brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    // 失败时 parseMtl 会把 out 清空，先判空再取 [0]，免得 RED 状态下越界读成 UB。
    if (!r.out.empty()) SR_ASSERT(r.out[0].diffuseMap == "brick.png");
}

SR_TEST(mtl_map_kd_unknown_option_swallows_whole_number_run) {
    // Finding 1：未知选项后的**整串**数字都要吞掉，不是一个。-weird 1 2 3 file.png ⇒ file.png，
    // 而不是静默剩下 "2 3 file.png"。兜底必须比被兜底的 arity 表更稳。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -weird 1 2 3 file.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    if (!r.out.empty()) SR_ASSERT(r.out[0].diffuseMap == "file.png");
    SR_ASSERT(!r.warnings.empty());
}

SR_TEST(mtl_map_kd_imfchan_non_numeric_arg_needs_table) {
    // -imfchan 的参数是通道字母（l），**不是数字**：未知分支的数字串规则接不住它，
    // 必须靠表（arity 1）吞掉，否则文件名会变成 "l brick.png"。这就是表不能删干净的理由。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -imfchan l brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    if (!r.out.empty()) SR_ASSERT(r.out[0].diffuseMap == "brick.png");
}

SR_TEST(mtl_map_kd_clamp_on_non_numeric_arg) {
    // -clamp on：参数 on 非数字，同样必须靠表（arity 1）吞掉，剩下 brick.png。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -clamp on brick.png\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    if (!r.out.empty()) SR_ASSERT(r.out[0].diffuseMap == "brick.png");
}

SR_TEST(mtl_map_kd_digit_leading_filename_is_known_limitation) {
    // 已知限制（D17 修订，**有意为之，不是 bug**）：以数字开头的文件名会被未知选项的数字串
    // 规则误吞。-weird 4k.png 里 4k.png 首字符是 '4' ⇒ 被吞 ⇒ 缺文件名 ⇒ 报错。真实导出器不产生
    // 这种名字；要支持它得让 parseMtl 问文件系统（违反 D2"解析不做 IO"），不值。这条钉住当前
    // 行为：将来谁改坏了这里会红，而不是安静地变成"文件名莫名其妙对不上"。
    const ParseResult r = parse(
        "newmtl m\n"
        "map_Kd -weird 4k.png\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.empty());
    SR_ASSERT(r.err.find("文件名") != std::string::npos);
}

SR_TEST(mtl_map_kd_without_filename_is_error) {
    // 光杆 map_Kd：D6 坏参数 ⇒ false + out 清空 + err 带行号。
    const ParseResult bare = parse(
        "newmtl m\n"
        "map_Kd\n");
    SR_ASSERT_FALSE(bare.ok);
    SR_ASSERT(bare.out.empty());
    SR_ASSERT(!bare.err.empty());
    SR_ASSERT(bare.err.find("第 2 行") != std::string::npos);  // 行号（P4：裸 "2" 出现在哪都算过）

    // 只有选项没有文件名：真实导出器的常见形状，但缺了文件名仍是错误（Review Focus 2）。
    const ParseResult optsOnly = parse(
        "newmtl m\n"
        "map_Kd -s 1 1 1\n");
    SR_ASSERT_FALSE(optsOnly.ok);
    SR_ASSERT(optsOnly.out.empty());
    SR_ASSERT(optsOnly.err.find("文件名") != std::string::npos);
}

SR_TEST(mtl_empty_text_yields_no_materials_not_an_error) {
    // 空文件合法：让 obj 侧兜 defaultMaterial()，而不是把"没有材质"当成加载失败。
    const ParseResult empty = parse("");
    SR_ASSERT(empty.ok);
    SR_ASSERT(empty.out.empty());
    SR_ASSERT(empty.err.empty());

    const ParseResult comments = parse("# just a comment\n\n   \n");
    SR_ASSERT(comments.ok);
    SR_ASSERT(comments.out.empty());
}

// ---- 控制器明文绑定的行为（不在 10 条命名用例里，但属于必须验证的要求）----

SR_TEST(mtl_tolerates_crlf_and_bare_cr) {
    // 行切分必须吃 CRLF 与裸 CR：Windows 导出器写 CRLF，老 Mac 写裸 CR。
    const ParseResult crlf = parse("newmtl m\r\nKd 0.5 0.5 0.5\r\n");
    SR_ASSERT(crlf.ok);
    SR_ASSERT_EQ(crlf.out.size(), 1);
    SR_ASSERT_NEAR(crlf.out[0].diffuse.r, 0.21404f, 1e-4f);

    const ParseResult cr = parse("newmtl m\rKd 1 1 1\r");
    SR_ASSERT(cr.ok);
    SR_ASSERT_EQ(cr.out.size(), 1);
    SR_ASSERT(cr.out[0].name == "m");
}

SR_TEST(mtl_bad_number_is_error_with_line_number) {
    // 已知指令的坏参数是 D6 错误（不是警告）：false + out 清空 + err 带行号与原文片段。
    const ParseResult r = parse(
        "newmtl m\n"
        "Kd 0.5 abc 0.5\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.empty());
    SR_ASSERT(!r.err.empty());
    SR_ASSERT(r.err.find("第 2 行") != std::string::npos);  // P4：钉整段行号标签，不是裸数字
    SR_ASSERT(r.err.find("abc") != std::string::npos);  // 肇事 token
}

SR_TEST(mtl_strips_utf8_bom) {
    // UTF-8 BOM 不该被当成未知指令：剥掉后第一行仍是 newmtl。
    const ParseResult r = parse("\xEF\xBB\xBFnewmtl m\nKd 1 1 1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    SR_ASSERT(r.out[0].name == "m");
    SR_ASSERT_EQ(r.stats.unrecognized, 0);
}

// ---- 终审 P4：四条 D6 守卫此前无测试（删掉任何一条 387 用例仍全绿）----
// 变异纪律（Ruling 57 / trap (b)）：直接删守卫会让错解落在表外（mats[-1] / tokens 越界），
// MSVC Debug 当场 abort，拿不到断言级红 —— 那不是证据。所以变异用"守卫做出另一种合法决策"
// （D6 降级成 D3：警告 + 继续），错解落在表内，给出断言级红。每条注明。

SR_TEST(mtl_known_attribute_before_newmtl_is_error_with_line_number) {
    // D6 守卫：已知属性没有 newmtl 在前是结构性错误（没守卫就是 mats[-1] UB）。
    // 夹具 Kd 在第 1 行：err 必须点名第 1 行与 newmtl。
    // 变异：把该分支降级成 D3（警告 + continue）⇒ ok 变 true ⇒ 本条红（断言级）。
    const ParseResult r = parse("Kd 1 0 0\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.empty());
    SR_ASSERT(r.err.find("第 1 行") != std::string::npos);
    SR_ASSERT(r.err.find("newmtl") != std::string::npos);
}

SR_TEST(mtl_unknown_directive_before_newmtl_is_warning_not_error) {
    // P8 路由：**未知**指令在 newmtl 之前走 D3（忽略 + 计数 + 警告），不是 D6 ——
    // 它污染不了一个尚不存在的材质。真实文件头部可能带散落属性，报错会白白毙掉它。
    // 与上一条是一对方向相反的断言：已知→D6、未知→D3，路由分界两边各钉一条。
    // 变异：把守卫扩成对所有指令生效（去掉 && known）⇒ 本条 ok 变 false ⇒ 红。
    const ParseResult r = parse("Ni 1.5\nnewmtl m\nKd 1 1 1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.size(), 1);
    SR_ASSERT_EQ(r.stats.unrecognized, 1);
    SR_ASSERT_EQ(r.warnings.size(), 1);
    if (!r.warnings.empty())
        SR_ASSERT(r.warnings[0].find("第 1 行") != std::string::npos);
}

SR_TEST(mtl_missing_numeric_argument_is_error_with_line_number) {
    // D6 守卫：已知指令缺数字参数 ⇒ false + out 清空 + err 点名行号。
    // Kd 要 3 个（只给 2）、d 要 1 个（一个不给），两种形状各一次。
    // 变异：把 arity 失败降级成 D3（警告 + continue）⇒ ok 变 true ⇒ 本条红。
    // （直接删 arity 检查会越界读 tokens，Debug abort，不是断言级证据。）
    const ParseResult kd = parse("newmtl m\nKd 0.5 0.5\n");
    SR_ASSERT_FALSE(kd.ok);
    SR_ASSERT(kd.out.empty());
    SR_ASSERT(kd.err.find("第 2 行") != std::string::npos);
    const ParseResult d = parse("newmtl m\nd\n");
    SR_ASSERT_FALSE(d.ok);
    SR_ASSERT(d.out.empty());
    SR_ASSERT(d.err.find("第 2 行") != std::string::npos);
}

SR_TEST(mtl_trailing_garbage_in_number_is_error_with_line_number) {
    // lex::parseFloat 必须整 token 消费："0.5abc" —— from_chars 能解出 0.5，但尾随 abc
    // 必须让整 token 失败（r.ptr != last），不是静默取 0.5。夹具只有这道守卫能拒它
    // （arity 够、其余 token 合法）：删 ptr 检查 ⇒ ok 变 true ⇒ 本条红（断言级）。
    const ParseResult r = parse("newmtl m\nKd 0.5abc 0 0\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.empty());
    SR_ASSERT(r.err.find("第 2 行") != std::string::npos);
    SR_ASSERT(r.err.find("0.5abc") != std::string::npos);  // 肇事 token 原样进 err
}

SR_TEST(mtl_non_finite_number_is_error_with_line_number) {
    // lex::parseFloat 拒非有限值：from_chars 接受 inf/nan，isfinite 必须挡住 ——
    // NaN/inf 进了 opacity/坐标是延迟故障（Ruling 52）。用 d/Ns（原样存、无下游计算），
    // 夹具只触发 isfinite 守卫：删它 ⇒ ok 变 true ⇒ 本条红（断言级）。
    const ParseResult inf = parse("newmtl m\nd inf\n");
    SR_ASSERT_FALSE(inf.ok);
    SR_ASSERT(inf.out.empty());
    SR_ASSERT(inf.err.find("第 2 行") != std::string::npos);
    const ParseResult nan = parse("newmtl m\nNs nan\n");
    SR_ASSERT_FALSE(nan.ok);
    SR_ASSERT(nan.out.empty());
    SR_ASSERT(nan.err.find("第 2 行") != std::string::npos);
}
