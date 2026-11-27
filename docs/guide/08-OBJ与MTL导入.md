# 08 — OBJ 与 MTL 导入

第 07 章结束时 uv 已能变成颜色，但几何还是程序生成的立方体。这一章把**外部文件**接进来：`--obj <path>` 载入任意 OBJ（含 MTL 与 PNG/JPG/TGA 贴图），不管单位是 mm 还是 m，立刻可见。结束时你手里有一个能跑的迷你解析器、一个"两次翻转互相抵消"的判别演示，以及四件各自会咬人的事——`(v,vt,vn)` 三元组去重与缺 vn 的 unroll、负索引按各自表长解析、`map_Kd` 选项 arity、全栈唯一一次 v 翻转，外加多材质分批、相机 fit、UTF-8 路径。本章片段本机实测：MSVC `cl /W4 /utf-8` 与 MSYS2 g++ 16.1.0（`-ffp-contract=off`）零警告、两侧输出逐字相同；生产侧 `soft_raster_tests.exe` 468 用例失败 0（命令见"真 app"节）。

## 分层：谁碰像素

解析在 core、装配在 loader、场景持有在 app——这不是偏好，是三方交集的唯一解（M5 notes §3）。`parseObj`/`parseMtl` 只认文本、零 IO（D2，`src/loader/obj.h:2-4`、`mtl.h:2-3`），畸形输入因此能内联在测试里当夹具；装配"OBJ+MTL+贴图"必须解码像素，而 C2 禁 core 感知第三方解码器（守卫是文本的：`grep -rn "stb_image" src/` 只命中 `image_stb.*`——M5.1 曾在 `mtl.h` 注释里写了字面量 `stb_image`，当场被这条 grep 抓红）；tests 链 core+loader（`CMakeLists.txt:148`）、不链 app 的 Win32 主程序（`:177-178`）。三条画成集合，**loader 是唯一交集**，`loader/model.cpp` 是 IO 的唯一发生处（`model.h:8-9`）。目录 ≠ target：文件住哪个目录不回答它链不链 stb，CMake 源列表才回答。

## 片段 1：迷你 OBJ 解析器

先把夹具写进你自己的私有目录（本章产物在 `build/guide-scratch-08/`，gitignored）。`mini.obj` 刻意让三张表长度不同（v=4、vt=2、vn=1），含负索引、缺 vt（`v//vn`）、缺 vn（裸 `v`）、A-B-A 材质开关：

```obj
v 0 0 0
v 1 0 0
v 0 1 0
v 1 1 0
vt 0 0
vt 1 1
vn 0 0 1
usemtl A
f 1/1/1 2/1/1 3/1/1
f 1/2/1 2/1/1 3/1/1
usemtl B
f -4/-2/-1 -1/-1/-1 -2/-1/-1
f 1//1 2//1 4//1
usemtl A
f 1 2 4
f 1 2 4
```

`bad.obj` 只有一条畸形面：`v 0 0 0` / `v 1 0 0` / `v 0 1 0` / `vt 0 0` / `vt 1 1` 五行声明后跟 `f 1/-3 2 3`。vt 表长 2，`-3` 按它自己的长度解得 -1 ⇒ 越界，必须响亮失败。解析器（单文件，只用标准库）：

```cpp
// guide08 片段 1：迷你 OBJ 解析器（只用标准库）。用法: mini_obj mini.obj bad.obj
// 演示：(v,vt,vn) 三元组去重、缺 vn 整面 unroll、负索引按各自表长解析、相邻同材质合并成组、
// 畸形输入全有或全无 + 行号。生产版 src/loader/obj.cpp 多了完整的 D3/D6 纪律与统计口径。
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
struct V3 { float x, y, z; };
template <class T> static bool num(const std::string& t, T& v) {
    auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    if (r.ec != std::errc{} || r.ptr != t.data() + t.size()) return false;
    if constexpr (std::is_floating_point_v<T>) return std::isfinite(v);  // 拒 inf/nan
    else return true;
}
struct Tri { int64_t v = -1, vt = -1, vn = -1; };  // 0-based；-1 = 缺项
struct Group { std::string mat; size_t off, cnt; };
struct Model {
    std::vector<V3> pos; std::vector<uint32_t> idx; std::vector<Group> grp;
    std::vector<std::pair<Tri, uint32_t>> table;  // 去重表（插入序，只为打印稳定）
    int corners = 0, tris = 0, neg = 0, unrolled = 0, noNrm = 0;
};
// 生产版是三个 int64 的哈希键 + unordered_map（obj.cpp:32-48）；迷你版用字符串键的 map。
static std::string key(const Tri& t) {
    return std::to_string(t.v) + "/" + std::to_string(t.vt) + "/" + std::to_string(t.vn);
}
// 负索引按**各自**表长解析（v 用 P、vt 用 T、vn 用 N）；先比较再相加，防 INT64_MIN 溢出 UB。
static bool resolve(const std::string& t, int64_t n, int ln, const char* what,
                    int64_t& i, int& neg, std::string& e) {
    int64_t raw = 0;
    const std::string tag = "第 " + std::to_string(ln) + " 行: " + what + " 索引 '" + t + "' ";
    if (!num(t, raw) || raw == 0) { e = tag + "不是非零整数"; return false; }
    if (raw < 0) { ++neg; i = raw < -n ? -1 : n + raw; } else i = raw - 1;
    if (i < 0 || i >= n) { e = tag + "越界（当前只有 " + std::to_string(n) + " 个 " + what + "）"; return false; }
    return true;
}
static std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> r; size_t a = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == c) { r.push_back(s.substr(a, i - a)); a = i + 1; }
    return r;
}
static bool parse(const std::string& text, Model& out, std::string& err) {
    out = Model{};
    std::vector<V3> P, N; std::vector<std::pair<float, float>> T;
    std::map<std::string, uint32_t> dedup;
    std::string mat = "(default)", line;
    std::istringstream in(text);
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        auto h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        std::istringstream ls(line); std::vector<std::string> tok;
        for (std::string w; ls >> w;) tok.push_back(w);
        if (tok.empty()) continue;
        auto fail = [&](std::string m) { out = Model{}; err = m; return false; };
        const std::string& d = tok[0];
        if (d == "v" || d == "vn") {
            if (tok.size() < 4) return fail("第 " + std::to_string(ln) + " 行: 需要 3 个数字");
            float a[3];
            for (int k = 0; k < 3; ++k)
                if (!num(tok[k + 1], a[k])) return fail("第 " + std::to_string(ln) + " 行: 坏数字 '" + tok[k + 1] + "'");
            V3 v{a[0], a[1], a[2]};
            (d == "v" ? P : N).push_back(v);  // 生产版对 vn 归一化（obj.cpp:160）
        } else if (d == "vt") {
            float a[2] = {0, 0};
            for (int k = 0; k < 2 && k + 1 < (int)tok.size(); ++k)
                if (!num(tok[k + 1], a[k])) return fail("第 " + std::to_string(ln) + " 行: 坏数字");
            T.push_back({a[0], a[1]});
        } else if (d == "usemtl") { mat = tok.size() > 1 ? tok[1] : "(default)";
        } else if (d == "f") {
            size_t n = tok.size() - 1;
            if (n < 3) return fail("第 " + std::to_string(ln) + " 行: f 少于 3 个角");
            std::vector<Tri> c(n);
            for (size_t k = 0; k < n; ++k) {
                const auto p = split(tok[k + 1], '/');  // v / v/vt / v/vt/vn / v//vn
                if (p.size() > 3 || p[0].empty()) return fail("第 " + std::to_string(ln) + " 行: 坏角 '" + tok[k + 1] + "'");
                if (!resolve(p[0], (int64_t)P.size(), ln, "v", c[k].v, out.neg, err)) return fail(err);
                if (p.size() > 1 && !p[1].empty() && !resolve(p[1], (int64_t)T.size(), ln, "vt", c[k].vt, out.neg, err)) return fail(err);
                if (p.size() > 2 && !p[2].empty() && !resolve(p[2], (int64_t)N.size(), ln, "vn", c[k].vn, out.neg, err)) return fail(err);
            }
            out.corners += (int)n;
            bool any = false, all = true;
            for (const Tri& t : c) { if (t.vn < 0) any = true; else all = false; }
            std::vector<uint32_t> vi(n);
            if (any) {  // 任一角缺 vn ⇒ 整面 unroll 成独占顶点，不进去重表
                ++out.unrolled; if (all) ++out.noNrm;
                // 生产版在这里给缺 vn 的角补 normalize(cross(b-a, c-a)) 面法线（obj.cpp:287-297）：
                // 共享顶点无法携带两个面法线，这是 unroll 的几何理由。迷你版只数顶点。
                for (size_t k = 0; k < n; ++k) {
                    vi[k] = (uint32_t)out.pos.size();
                    out.pos.push_back(P[c[k].v]);
                }
            } else {
                for (size_t k = 0; k < n; ++k) {
                    const std::string kk = key(c[k]);
                    auto it = dedup.find(kk);
                    if (it != dedup.end()) { vi[k] = it->second; continue; }
                    vi[k] = (uint32_t)out.pos.size();
                    dedup.emplace(kk, vi[k]);
                    out.table.push_back({c[k], vi[k]});
                    out.pos.push_back(P[c[k].v]);
                }
            }
            if (out.grp.empty() || out.grp.back().mat != mat) out.grp.push_back({mat, out.idx.size(), 0});
            for (size_t k = 1; k + 1 < n; ++k) {  // 扇形分解：n 角 ⇒ n-2 三角形
                out.idx.insert(out.idx.end(), {vi[0], vi[k], vi[k + 1]});
                out.grp.back().cnt += 3; ++out.tris;
            }
        }
    }
    return true;
}
static std::string slurp(const char* p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream s; s << f.rdbuf(); return s.str();
}
int main(int argc, char** argv) {
    if (argc < 3) { std::printf("用法: mini_obj <mini.obj> <bad.obj>\n"); return 2; }
    Model m; std::string err;
    if (!parse(slurp(argv[1]), m, err)) { std::printf("[mini] 意外失败: %s\n", err.c_str()); return 1; }
    std::printf("[mini] faceCorners=%d triangles=%d negativeIndices=%d\n", m.corners, m.tris, m.neg);
    std::printf("[mini] 去重表 (v,vt,vn -> 顶点下标, -1=缺项, 0-based):\n");
    for (const auto& e : m.table)
        std::printf("  (%lld,%lld,%lld) -> %u\n", (long long)e.first.v, (long long)e.first.vt, (long long)e.first.vn, e.second);
    std::printf("[mini] unroll: facesUnrolled=%d facesWithoutNormals=%d ⇒ 独占顶点 %zu 个\n",
                m.unrolled, m.noNrm, m.pos.size() - m.table.size());
    std::printf("[mini] uniqueVertices=%zu (去重 %zu + unroll %zu)\n", m.pos.size(), m.table.size(), m.pos.size() - m.table.size());
    std::printf("[mini] groups=%zu:", m.grp.size());
    for (const Group& g : m.grp) std::printf(" %s[%zu,%zu)", g.mat.c_str(), g.off, g.off + g.cnt);
    std::printf("\n");
    Model bad;
    const bool ok = parse(slurp(argv[2]), bad, err);
    std::printf("[bad] ok=%s err=%s\n", ok ? "true" : "false", err.c_str());
    std::printf("[bad] out 已清空: pos=%zu idx=%zu\n", bad.pos.size(), bad.idx.size());
    return (!ok && bad.pos.empty() && !err.empty()) ? 0 : 1;
}
```

```bash
g++ -std=c++20 -Wall -Wextra -ffp-contract=off mini_obj.cpp -o mini_obj && ./mini_obj mini.obj bad.obj
cl /nologo /EHsc /std:c++20 /W4 /utf-8 mini_obj.cpp   # 中文串要 /utf-8；产物 mini_obj.exe mini.obj bad.obj
```

你应该看到（去重表 9 条，这里摘 3 条代表，其余同形）：

```text
[mini] faceCorners=18 triangles=6 negativeIndices=9
[mini] 去重表 (v,vt,vn -> 顶点下标, -1=缺项, 0-based):
  (0,0,0) -> 0
  (0,1,0) -> 3
  (0,-1,0) -> 6
[mini] unroll: facesUnrolled=2 facesWithoutNormals=2 ⇒ 独占顶点 6 个
[mini] uniqueVertices=15 (去重 9 + unroll 6)
[mini] groups=3: A[0,6) B[6,12) A[12,18)
[bad] ok=false err=第 6 行: vt 索引 '-3' 越界（当前只有 2 个 vt）
[bad] out 已清空: pos=0 idx=0
```

三个读数各自钉住一件事。**去重与 unroll 方向相反**（`obj.h:12-14`）：面 1/2 共享 `(1,0,0)`/`(2,0,0)` 两个角，6 个角去重成 4 个顶点；面 2 的 `(0,1,0)` 与面 1 的 `(0,0,0)` 位置相同但 vt 不同 ⇒ 必须是两个顶点——键是三元组不是 vIdx（`obj_dedups_vertices_by_the_v_vt_vn_triple`，`test_obj.cpp:69-96`，变异"键退化成 vIdx"会把 4 并成 3 而红）。最后两个裸 `f 1 2 4` 完全相同却各拿 3 个独占顶点（共 6，不是 3）：共享顶点无法携带两个面法线，这是几何事实（`obj_unrolls_faces_that_have_no_normals`，`test_obj.cpp:289-331`）。生产版给缺 vn 的角补真面法线而不是靠后端兜底——`render_backend.cpp:134` 对缺失 normal 兜底 `(0,1,0)` 是安全网不是功能，靠它的话 M6 一上光照整个模型朝 +Y 打光（`obj.cpp:274-276`）。**负索引各用各的表长**：`resolve` 的第二个参数对 v/vt/vn 分别传 `P/T/N.size()`（生产 `obj.cpp:246-252`）；经典错法是三个负索引都拿 v 的表长解，判别夹具让两表长度不同且错解仍落在表内 ⇒ 断言级红而不是越界 abort（`obj_negative_vt_vn_indices_resolve_against_their_own_table_lengths`，`test_obj.cpp:671-744`，双向 + 正控制；"一次 abort 的变异运行不算证据"，Ruling 57，notes §2）。`bad.obj` 的 `-3` 若错用 v 长 4 会解成 1 而被接受——这条夹具就是抓那个错的。**分组只合并相邻 run**：A-B-A ⇒ 3 组不重排（`obj.cpp:320-331`，`test_obj.cpp:387-424`）；`MeshGroup` 的单位是索引数组（`obj.h:32-34`），`drawRange` 仍防御非 3 倍数丢尾 + 计数（Ruling 45，`test_draw_range.cpp:145-163`）。

## 片段 2：两次翻转互相抵消

OBJ 的 vt 说 v=0 是图片**下**边缘，TGA 默认 bottom-up（文件第 0 行 = 图像底行），而管线约定内存第 0 行 = 顶部、采样 v=0 = 第 0 行（`mesh.cpp:51-54`）。于是有两处独立翻译：解析器出口 `v←1-v`（`obj.cpp:59` 的 `uvOut`，D9）与加载器行翻转（`image_tga.cpp:121-125`）；PNG/JPG 总是 top-down，stb 侧显式不翻（`image_stb.cpp:82`）。把场景缩到一张 4×8 程序纹理，四种配置各渲一帧与"两处都对"逐像素比对：

```cpp
// guide08 片段 2：两次翻转互相抵消（只用标准库）。编译后直接运行，无参数。
// 一张 4×8 程序纹理，文件按 TGA 默认 bottom-up 存（文件第 0 行 = 图像底行）；OBJ 的 vt 说
// v=0 = 图像下边缘；管线约定内存第 0 行 = 顶部、采样 v=0 = 第 0 行。两处翻译各自独立：
// 解析器出口 v←1-v（obj.cpp:59 的 uvOut）与加载器行翻转（image_tga.cpp:121-125）。
// 四种配置各渲一帧与"两处都对"的参考逐像素比对；再换竖直对称纹理重跑一遍。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
constexpr int W = 4, H = 8;
static unsigned char texValue(int imageRow, int col, bool sym) {
    const int r = sym ? (imageRow < H - 1 - imageRow ? imageRow : H - 1 - imageRow) : imageRow;
    return static_cast<unsigned char>(r * 32 + col * 8);  // 非对称：行单调、列也变
}
static std::vector<unsigned char> render(bool sym, bool parserFlip, bool loaderFlip) {
    unsigned char file[W * H], mem[W * H], out[W * H];
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            file[r * W + c] = texValue(H - 1 - r, c, sym);  // bottom-up 的文件
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            mem[r * W + c] = file[(loaderFlip ? H - 1 - r : r) * W + c];
    for (int y = 0; y < H; ++y) {
        const double vFile = 1.0 - (y + 0.5) / H;      // OBJ 作者按惯例写的 vt
        const double vMem = parserFlip ? 1.0 - vFile : vFile;
        int row = static_cast<int>(std::floor(vMem * H));
        row = row < 0 ? 0 : (row > H - 1 ? H - 1 : row);
        for (int c = 0; c < W; ++c) out[y * W + c] = mem[row * W + c];  // 整数 nearest
    }
    return std::vector<unsigned char>(out, out + W * H);
}
static void report(const char* name, bool sym) {
    const auto ref = render(sym, true, true);
    const auto a = render(sym, false, true), b = render(sym, true, false),
               c = render(sym, false, false);
    auto diff = [&](const std::vector<unsigned char>& x) {
        int n = 0, mx = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const int d = std::abs(int(x[i]) - int(ref[i]));
            if (d > 0) ++n;
            if (d > mx) mx = d;
        }
        std::printf("diff=%d/%d maxDelta=%d", n, W * H, mx);
    };
    std::printf("[%s] 只解析器忘翻: ", name); diff(a);
    std::printf("  | 只加载器忘翻: "); diff(b);
    std::printf("  | 两处都忘翻: "); diff(c);
    std::printf("%s\n", (!sym && c == ref) ? "  ← 抵消：与参考逐字节相同" : "");
    if (!sym) {
        std::printf("[%s] row0 参考/单错/都错:", name);
        for (const auto* img : {&ref, &a, &c}) {
            std::printf("  [");
            for (int k = 0; k < W; ++k) std::printf("%s%d", k ? " " : "", (*img)[k]);
            std::printf("]");
        }
        std::printf("\n");
    }
}
int main() {
    report("asym", false);
    report("sym ", true);
    std::printf("[结论] 单错一处整幅颠倒；两处都错反而全同 ⇒ 画面正常不构成证据。\n");
    std::printf("[结论] 对称纹理下四种配置全同 ⇒ 夹具必须非对称，判别靠各自独立的锚。\n");
    return 0;
}
```

```text
[asym] 只解析器忘翻: diff=32/32 maxDelta=224  | 只加载器忘翻: diff=32/32 maxDelta=224  | 两处都忘翻: diff=0/32 maxDelta=0  ← 抵消：与参考逐字节相同
[asym] row0 参考/单错/都错:  [0 8 16 24]  [224 232 240 248]  [0 8 16 24]
[sym ] 只解析器忘翻: diff=0/32 maxDelta=0  | 只加载器忘翻: diff=0/32 maxDelta=0  | 两处都忘翻: diff=0/32 maxDelta=0
[结论] 单错一处整幅颠倒；两处都错反而全同 ⇒ 画面正常不构成证据。
[结论] 对称纹理下四种配置全同 ⇒ 夹具必须非对称，判别靠各自独立的锚。
```

这就是本项目最危险的一类假通过：**单独任一处错 ⇒ 整幅上下颠倒；两处都错 ⇒ 抵消，画面看起来完全正确**。本演示用整数 nearest，抵消逐字节精确；真实管线里 level 0 与 mip 链生成也精确（`downsampleBox2` 用 int 累加，加法可交换 ⇒ 与竖直翻转逐字节可交换），残差只来自三线性浮点链。M5.7 变异③（同时删两处翻转）实测把判别分成三档（Ruling 77，notes §1）：±1 容差下抵消**完全不可见**；零容差锚 3 会红，但只红 **3 个像素各 ±1/通道**——抓它的是舍入噪声不是语义差异，换编译器/换 SIMD 后可能消失，**不可依赖**；真正可靠的判别只能是锚 1/2：③ 下红在 **12/12 个三角形**（`cube.obj` vs `makeCube`，`test_obj_golden.cpp:94`）与 **49152 字节**（`checker.tga` vs `makeCheckerImage`，`test_tga_writer.cpp:152`），语义级、与舍入无关。给你的判别纪律：凡"翻两次"的路径，问一句"把两次翻转对调，我的夹具会给出不同的数吗？"答"不会"就还没资格写断言；每条翻转各养一条**非对称**基准的独立锚（`obj_flips_vt_because_obj_v_zero_is_the_image_bottom` 刻意用 vt 0.25 而不是 0.5——0.5 在翻转下不变，是假测试，`test_obj.cpp:558-586`）。

## 指回本仓库：生产版本多了什么

- **共享词法**（`lex.h`）：BOM/CRLF/裸 CR/tab 容忍（`:55-78`），`parseFloat` 整 token 必须是有限数字、拒尾随垃圾与 inf/nan（`:32-39`）；OBJ 索引另要求非零整数（`obj.cpp:19-26`，"1.0" 不是合法索引）。两份解析器对畸形输入的容忍由"同一份代码"保证同构，不是两份恰好一样的副本。
- **D6 全有或全无**：任何结构性错误 ⇒ false、out 清空、err 带行号与肇事 token（`obj.h:10-11`），没有"跳过这一面继续"。但**退化面**（≥3 角、位置引用重复）是丢弃 + 警告不是错误（`obj.cpp:256-269`）；越界索引是 D6 错误不是 clamp——`render_backend.cpp:133` 的 `positions[idx]` 没有边界保护，放进去就是渲染期越界读（`obj.cpp:221-223`）。
- **统计口径**（`obj.h:47-61`）：`faceCorners` 计文件输入（含被丢的退化面）；`uvOutOfRange` 按 vt 声明行计、越界值照存不 clamp（`obj.cpp:139-145`，wrap 本就为越界 uv 设计）；`negativeIndices` 按分量计；未知指令 D3 忽略 + 计数 + 警告（`obj.cpp:343-346`），`o`/`g`/`mtllib` 是已知指令、名字不切分几何（`obj.cpp:338-341`）。
- **材质兜底**（Ruling 46，`obj.h:75-82`）：入参空 ⇒ 恰好一个 `defaultMaterial()`；`usemtl` 未知名 ⇒ 警告 + 末尾追加**一次**并复用。默认纯白 1 1 1 不是 0.8 灰——baseColor 是乘性 tint，0.8 会让没带 MTL 的模型整体暗一档，症状像"光照错了"（`mtl.h:22-24`）。
- **MTL 的 sRGB 不对称**（D5）：Kd/Ka/Ks 在解析时解码成线性（`mtl.cpp:117-121`），贴图像素**不在加载时解码**——`makeTexture(img, /*colorDataIsSrgb=*/true)` 只设标志 + 建 mip 链，解码在采样时逐纹素（`model.cpp:197-198`）。设计文档曾把这句写反（说解码在 makeTexture 里），按那句规划会得到双重解码（notes §4(d)）——文档陈述是待验证断言。
- **`map_Kd` 选项 arity**（D17，`mtl.cpp:32-39`）：`-s/-o/-t` 吞 3、`-bf/-mm` 吞 2、`-bm/-texres/-imfchan/-clamp/-cl` 吞 1，其余交未知路径。本仓真实错过：原表凭印象写 `-mm=3`、`-cc`（根本不是 MTL 选项）、`-imf`（真名 `-imfchan`）。错的方向不对称——写**少**了只是少吞（未知路径接得住），写**多**了是多吞：`map_Kd -mm 0 1 brick.png` 把文件名当第三个参数吃掉 ⇒ 空串 ⇒ 判"缺文件名" ⇒ `parseMtl` 整体失败 ⇒ `loadModel` 报错、**整个模型载入失败**（`model.cpp:129-134`；`mtl_map_kd_mm_takes_two_args`，`test_mtl.cpp:158-168`）。表不能删干净：`-imfchan l`/`-clamp on` 的参数不是数字，未知分支的"吞紧随整串数字"规则接不住（`mtl.cpp:152-161`，`test_mtl.cpp:182-201`）；以数字开头的文件名会被误吞是写明不追的已知限制（`test_mtl.cpp:203-214`）。
- **装配与路径解析**（`model.cpp:101-207`）：mtllib 名相对 **OBJ 目录**、map_Kd 相对 **MTL 目录**（`model.h:69-71`）；去重键 `weakly_canonical` ⇒ `checker.tga` 与 `./checker.tga` 共享一条 mip 链（`model.cpp:176-184`，`test_model.cpp:228`）；`.tga` 大小写不敏感分派到我们自己的解析器、其余走第三方（Ruling 63，`model.cpp:186-190`）——锚 2 守的正是这条路径。警告/错误分界（`model.h:79-86`）：缺整个 MTL 文件 ⇒ 警告（作者没打算给材质）；缺 map_Kd 文件、MTL 存在但解析坏 ⇒ 错误（症状都是"看起来正常但纯色"，最难发现，必须响亮失败）。警告打印每文件最多前 8 条 + 汇总（Ruling 75，`model.cpp:77-87`），总数不缩水：MTL 的未知属性计数在 `parseObj` **之前**并入 `stats.unrecognized`（终审 I1，`model.cpp:135-144`，`test_model.cpp:685`）。
- **逐组提交**（`model.cpp:241-270`）：每个 `MeshGroup` 一次 `drawRange`，`fu.baseColor` = 该组线性 Kd、texture 按槽位取地址，借用只活在那一次调用内。`textures` 载入后不得再增长（裸指针会悬垂），由 `texturesFrozenAt` 的 O(1) 入口检查守（Ruling 66，`model.h:29-39`）。
- **fit 相机**（`fit.h:57-68`）：`distance = r/sin(min(fovX,fovY)/2)`、`near = r/1000`、`far = r*10`（near/far/fov 出自 `docs/superpowers/specs/2026-11-21-soft-raster-design.md:236-238`，distance 出自 `docs/superpowers/specs/2026-11-22-soft-raster-m5-obj-design.md:396-399`）。用 **sin 不是 tan**：界是球，半径 r 的球在距离 d 处轮廓半角 `asin(r/d)`，tan 对应平面张角、用在球上差一个 `1/cos` 会裁掉盒子一圈；取 min(fovX,fovY) 因为竖屏时水平 fov 更紧（`fit.h:29-34`）。退化输入 ⇒ 不动作（Ruling 49，`fit.h:46-56`）。**已知限制**：`sin(θ/2)<0.1` 即 `θ<2·asin(0.1)≈11.478°` 时 distance 超过 far，远角被裁；今天 fov 恒 60° 不可达，不加 clamp（`fit.h:64-67`，Ruling 71）。fit 与投影必须取同一个 `kDefaultFovYRadians`（P24，`fit.h:10-18`），两边漂移的症状是"某些模型装不下"。公式逐字段断言 + 8 角 NDC 三尺度包含（D16，`test_fit.cpp:89-113` 给 1.6 立方体 `distance 2.77128 / near 0.001386 / far 13.85641`，`:135` 三尺度）；refit 还把滚轮夹钳改成随尺度（Ruling 73，`scene.cpp:95-102`），否则 mm 级模型第一下滚轮就被旧常数 snap。**golden 豁免 fit**（D15）：锚 3 用 M4 固定姿态而非 fit 相机（`obj_scene.cpp:16-23`），否则"与 M4 逐字节相同"不可能成立（notes §5(a)）。
- **UTF-8 路径是两处不同的转换**：argv/拖放走 `nativePath`（`MultiByteToWideChar(CP_UTF8)`，`path.cpp:15-23`）；**文件内容里的**名字（mtllib、map_Kd）走 `pathFromUtf8`（char8_t 源构造 path，`model.cpp:54-68`，Ruling 65）——`path(std::string)` 在 MSVC 按本地 ANSI 码页解释，中文贴图名被误解码成"打不开文件"；`u8path` C++20 已废弃、两侧告警。消息里的路径一律 `displayPath`（u8string，无损），**任何 `.string()` 都算缺陷**（Ruling 62，`lex.h:80-89`、`model.h:11-14`）：MSVC 英文 locale 下它遇非 ASCII 路径抛 system_error，未捕获 = CRT abort，表现为 ctest 无声挂死。中文目录 + 空格贴图名、中文贴图名各有测试（`test_model.cpp:465`、`:532`）。
- **路径遍历可接受**（整枝终审 deferred 分诊①，`.superpowers/sdd/2026-11-23-soft-raster-m5-obj/progress.md:784-786`）：`map_Kd ../../x` 或绝对路径**不要求收紧**——本地桌面查看器以用户自己的权限运行，用户本来就能读那些文件，而绝对 map_Kd 在真实 3ds Max 导出里很常见。这不是编号 Ruling，是终审记录的一条裁定。

## 坑（一手来自 notes）

- **抵消**（§1）与**夹具对称性**（§2）：见片段 2。对称夹具让"实现正确、测试永远不可能红"——M4 那张表在 M5 续了第 6 行"双翻转"。
- **假注释比无注释危险**（§6，Ruling 61/68）：把"锚 3 在③下也红"叙述成稳定能力，最坏读法诱导人删掉两条语义锚（P33 当场修）。
- **选项吞文件名**（§4(b)）：arity 表写多了 ⇒ 整个模型载入失败，见上。`cmdline` 也有一族：`nextString` 第一版忘了 `++index`，`--obj foo.obj` 静默把 obj 设成 `"--obj"`——不报错的那种更危险（`cmdline.cpp:41-43`）。
- **绕向不动**（D10，§4(c)）：OBJ 从正面看 CCW 与我们 NDC 约定一致，解析器不改角序、不翻法线；top-down 屏幕判据是 **area2<0 为正面**（`pipeline.h:107-111`，别读成"视觉 CW"）。导出器写成 CW 的症状是"模型内表面朝外"，`W` 键看背面是诊断工具，不做静默自动修复。
- **警告洪水**（§6，Ruling 75）：真实模型 female02 吐 44 条警告（MTL 12 + OBJ 32），不加上限看起来像卡死；上限只压噪声不压信息，HUD 读合计。

## 真 app：载入外部模型

```bash
./build/Debug/soft-raster.exe --obj assets/obj/twomats.obj            # 窗口；拖放 .obj / R 重载 / F 重 fit / W 看背面
./build/Debug/soft-raster.exe --obj assets/obj/twomats.obj --offscreen --frames 1 --out build/guide-scratch-08/twomats.bmp
./build/Debug/soft-raster.exe --obj assets/obj/cube.obj --golden-write-obj build/guide-scratch-08/m5-obj.bmp
./build/Debug/soft_raster_tests.exe --only anchor                     # 锚 1/2/3 + override 敏感性，4 用例失败 0
```

offscreen 那条打印 `[obj] 已载入 assets/obj/twomats.obj: 24 三角形 / 3 组 / 1 贴图`（`scene.cpp:245-247`）——12 个四边形扇形分解成 24 三角形；plain/texA/texB 三组（texA 的冗余 `usemtl` 开关被相邻合并）；texA/texB 共用 `../tga/checker.tga` 去重成 1 张贴图。`--golden-write-obj` 写出 320×180，与 `assets/golden/m4_textured_cube.bmp` 只差 **63 字节且全在 HUD 条**（x<110、y<12，实测逐字节核对）；缺 `--obj` 时它响亮报错而不是画默认场景（`cmdline.cpp:303-307`）。载入失败保留旧模型/兜底立方体、零面 OBJ 在 app 层判失败（loader 问"解析对了吗"，app 问"有没有东西可看"，`scene.cpp:56-73`）。

## 你应该看到 X；如果你看到 Y，说明 Z 错了

你应该看到：片段 1 `uniqueVertices=15`（去重 9 + unroll 6）、`groups=3: A[0,6) B[6,12) A[12,18)`、`negativeIndices=9`，`bad.obj` 报"第 6 行 … 越界（当前只有 2 个 vt）"且 out 清空；片段 2 非对称纹理单错 `diff=32/32 maxDelta=224`、两处都错 `diff=0/32`，对称纹理四配置全 0。仓库侧 `--only anchor` 4 用例失败 0、全套 468 失败 0、twomats 载入 24/3/1。如果你看到：

- **去重后顶点数没变少 / 同位置不同 uv 被并成一个**：键退化成 vIdx 了，回片段 1 的三元组（`test_obj.cpp:69`）。
- **缺 vn 的面共享了顶点 / 光照后整个模型朝 +Y**：unroll 分支没走，或靠了后端 `(0,1,0)` 兜底而没补真面法线。
- **负索引模型缺角或贴图错乱**：三个分量都拿 v 的表长解了；用两表长度不同的夹具双向钉（`test_obj.cpp:671`）。
- **带贴图的模型整体载入失败、err 说"缺文件名"**：`map_Kd` arity 表写多了，把文件名当参数吃了（`mtl.cpp:26-31`）。
- **贴图上下颠倒但"看起来还行"**：先查是不是两处翻转都错了（抵消）；画面正常不构成证据，回片段 2 与锚 1/2。
- **中文目录能载入、中文贴图名却找不到**：文件内容里的名字走了 `path(std::string)` 而不是 `pathFromUtf8`（Ruling 65）。
- **某些模型装不下 / 第一下滚轮就飞走**：fit 与投影的 fov 各写一份漂了（P24），或夹钳没随尺度（Ruling 73）。
- **golden 红**：先看差异像素数与坐标；个位数 ±1 多半是舍入漂移，成千上万是真回归。**不许未经产品方肉眼验收就重生成**（第 06 章）。

下一章（09）：光照——Ka/Ks/Ns 在 M5 存而不用，M6 第一次消费它们；Gouraud 与逐像素的可见差别、线性空间与 sRGB。