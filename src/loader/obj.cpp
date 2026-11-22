#include "loader/obj.h"

#include <charconv>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "math/vec.h"

namespace sr {
namespace {

// 与 mtl.cpp 的 parseFloat 同一规则：整个 token 必须是一个有限数字才算成功。
// from_chars 不跳前导空白、不吃尾随垃圾（"0.5abc" 挡掉）；inf/nan 也拒绝。
bool parseFloat(std::string_view tok, float& v) {
    if (tok.empty()) return false;
    const char* first = tok.data();
    const char* last = first + tok.size();
    const auto r = std::from_chars(first, last, v, std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    return std::isfinite(v);
}

// OBJ 的索引必须是**非零整数**（"1.0" 这种浮点写法不是合法索引）。
bool parseInt(std::string_view tok, int64_t& v) {
    if (tok.empty()) return false;
    const char* first = tok.data();
    const char* last = first + tok.size();
    const auto r = std::from_chars(first, last, v);
    return r.ec == std::errc{} && r.ptr == last;
}

// 按空格/tab 切 token（与 mtl.cpp 同一写法）。
std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> t;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        t.push_back(line.substr(start, i - start));
    }
    return t;
}

// 行切分容忍 CRLF 与裸 CR（与 mtl.cpp 同一写法）：\r\n 算一个行尾，单独的 \r 或 \n 也算。
// Windows 导出器写 CRLF，老 Mac 写裸 CR。
std::string_view nextLine(std::string_view text, size_t& cursor) {
    size_t end = cursor;
    while (end < text.size() && text[end] != '\n' && text[end] != '\r') ++end;
    const std::string_view line = text.substr(cursor, end - cursor);
    if (end < text.size()) {
        if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
            end += 2;
        else
            end += 1;
    }
    cursor = end;
    return line;
}

// UTF-8 BOM 剥掉，否则它会被当成第一行的未知指令（Blender/某些导出器会写 BOM）。
size_t skipBom(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        return 3;
    return 0;
}

// D7 去重键：解析后的 (v, vt, vn) 三元组，缺项用 -1 哨兵。三个 int64 打包成一个键，
// unordered_map 只在**加载期**用（C5 的热路径不碰它）。
constexpr int64_t kMissing = -1;

struct TripleKey {
    int64_t v, vt, vn;
    bool operator==(const TripleKey& o) const { return v == o.v && vt == o.vt && vn == o.vn; }
};

struct TripleHash {
    size_t operator()(const TripleKey& k) const {
        size_t h = 0;
        const auto mix = [&h](int64_t x) {
            h ^= std::hash<int64_t>{}(x) + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        mix(k.v);
        mix(k.vt);
        mix(k.vn);
        return h;
    }
};

struct Corner {
    int64_t v = kMissing, vt = kMissing, vn = kMissing;
};

// D9：全栈唯一的一次 v 翻转，发生在解析器的**出口**（顶点属性装配的最后一站）。
// 契约见 mesh.cpp:49-52："图像第 0 行 = 顶部、采样器 v=0 = 第 0 行、mesh 的 v=0 =
// 这一面的顶边"。OBJ 的 vt 原点在图片左下 ⇒ uvOut = {u, 1 - v}。
// 格式的差异在格式的门口处理掉，不留进管线 —— 不放 sampler（文件格式知识漏进采样器），
// 不放 mesh gen（会破坏 M4 golden 钉死的约定）。缺 vt 的角不走这里（(0,0) 不翻转，D7）。
Vec2 uvOut(const Vec2& vt) { return Vec2{vt.x, 1.0f - vt.y}; }

}  // namespace

bool parseObj(std::string_view text, std::span<const Material> materials, Model& out,
              std::vector<std::string>& warnings, ObjStats& stats, std::string& err) {
    out = Model{};
    err.clear();

    // D6 全有或全无：几何先建在本地 Model 里，成功才 move 进 out；任何一步失败 out 保持空。
    Model m;
    // Ruling 46 / 收紧 3：入参是**只读** span，兜底追加发生在 out.materials 里。
    // (a) 入参空 ⇒ 恰好一个 defaultMaterial()，fallbackIdx 直接是 0：此时未知 usemtl
    //     也只警告并复用它，**不再追加**（"恰好一个"优先于规则 (b)）。
    // (b) 入参非空 ⇒ 原样拷贝；未知 usemtl 在末尾追加**一次** default 并复用那个追加项。
    int fallbackIdx = -1;
    if (materials.empty()) {
        m.materials.push_back(defaultMaterial());
        fallbackIdx = 0;
    } else {
        m.materials.assign(materials.begin(), materials.end());
    }
    int currentMat = 0;   // 第一条 usemtl 之前的面用 materials[0]

    // OBJ 的三张声明表（1-based 索引的底座）。vt 存**原始** (u,v)，翻转在顶点出口做（D9）。
    std::vector<Vec3> posTable;
    std::vector<Vec2> vtTable;
    std::vector<Vec3> vnTable;
    std::unordered_map<TripleKey, uint32_t, TripleHash> dedup;

    const auto fail = [&](std::string msg) {
        out = Model{};
        err = std::move(msg);
        return false;
    };
    const auto lineTag = [](int n) { return "第 " + std::to_string(n) + " 行: "; };

    int lineNo = 0;
    size_t cursor = skipBom(text);
    while (cursor < text.size()) {
        const std::string_view line = nextLine(text, cursor);
        ++lineNo;

        // '#' 起是注释；空行/纯注释跳过。
        const size_t hash = line.find('#');
        const std::vector<std::string_view> tokens =
            tokenize(hash != std::string_view::npos ? line.substr(0, hash) : line);
        if (tokens.empty()) continue;
        const std::string_view dir = tokens[0];

        if (dir == "v") {
            if (tokens.size() < 4)
                return fail(lineTag(lineNo) + "v 需要 3 个数字参数");
            float xyz[3];
            for (int k = 0; k < 3; ++k) {
                if (!parseFloat(tokens[k + 1], xyz[k]))
                    return fail(lineTag(lineNo) + "v 的数字参数 '" + std::string(tokens[k + 1]) +
                                "' 解析失败");
            }
            // 第 4 个分量 w 若有则必须是合法数字，但**忽略其值**（不支持有理顶点，YAGNI）。
            if (tokens.size() >= 5) {
                float w = 0.0f;
                if (!parseFloat(tokens[4], w))
                    return fail(lineTag(lineNo) + "v 的 w 分量 '" + std::string(tokens[4]) +
                                "' 解析失败");
            }
            posTable.push_back(Vec3{xyz[0], xyz[1], xyz[2]});
            continue;
        }

        if (dir == "vt") {
            if (tokens.size() < 2)
                return fail(lineTag(lineNo) + "vt 需要至少 1 个数字参数 (u)");
            float uvw[3] = {0.0f, 0.0f, 0.0f};   // v 缺省 0（OBJ 规范）；w 忽略
            const int have = tokens.size() >= 4 ? 3 : static_cast<int>(tokens.size()) - 1;
            for (int k = 0; k < have; ++k) {
                if (!parseFloat(tokens[k + 1], uvw[k]))
                    return fail(lineTag(lineNo) + "vt 的数字参数 '" + std::string(tokens[k + 1]) +
                                "' 解析失败");
            }
            // 越界值**照存**（出口翻转之后），只计数不 clamp：clamp 会把"文件写错了"
            // 藏起来，而 wrap 模式本来就是设计给越界 uv 用的（Review Focus 4）。
            // 按声明行计：u 或 v 越出 [0,1] 记 1（两个都越界也只记 1）；未被引用也计 ——
            // "文件说了奇怪的话"这件事发生在声明处。边界值 0/1 算界内。
            if (uvw[0] < 0.0f || uvw[0] > 1.0f || uvw[1] < 0.0f || uvw[1] > 1.0f)
                ++stats.uvOutOfRange;
            vtTable.push_back(Vec2{uvw[0], uvw[1]});
            continue;
        }

        if (dir == "vn") {
            if (tokens.size() < 4)
                return fail(lineTag(lineNo) + "vn 需要 3 个数字参数");
            float xyz[3];
            for (int k = 0; k < 3; ++k) {
                if (!parseFloat(tokens[k + 1], xyz[k]))
                    return fail(lineTag(lineNo) + "vn 的数字参数 '" + std::string(tokens[k + 1]) +
                                "' 解析失败");
            }
            // 导出器常写非单位 vn：归一化只改长度不改方向（D10 禁的是翻转，不是缩放）。
            // 零向量按 vec.h 的约定归一化仍是零，不会产出 NaN。
            vnTable.push_back(normalize(Vec3{xyz[0], xyz[1], xyz[2]}));
            continue;
        }

        if (dir == "usemtl") {
            // 材质名可含空格：指令之后的 token 用单空格重拼（与 newmtl 同一形状）。
            std::string name;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (!name.empty()) name += ' ';
                name.append(tokens[i].data(), tokens[i].size());
            }
            int found = -1;
            for (size_t i = 0; i < m.materials.size(); ++i) {
                if (m.materials[i].name == name) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            if (found >= 0) {
                currentMat = found;
                continue;
            }
            // 未知名 ⇒ **警告不是 D6 错误**（收紧 3：缺材质只损失 tint，不损失几何）。
            warnings.push_back(lineTag(lineNo) + "usemtl 指向未知材质 '" + name +
                               "'，使用默认材质");
            if (fallbackIdx < 0) {
                m.materials.push_back(defaultMaterial());
                fallbackIdx = static_cast<int>(m.materials.size()) - 1;
            }
            currentMat = fallbackIdx;   // 多个未知名复用同一个追加项
            continue;
        }

        if (dir == "f") {
            const size_t n = tokens.size() - 1;
            // **少于 3 个角是 D6 结构性错误**（畸形文件）；而 ≥3 个角但位置引用重复是
            // **退化面**（导出器常写的冗余），丢弃 + 警告即可 —— 两者不可统一处理。
            if (n < 3)
                return fail(lineTag(lineNo) + "f 需要至少 3 个顶点（只有 " + std::to_string(n) +
                            " 个）");

            std::vector<Corner> corners(n);
            for (size_t k = 0; k < n; ++k) {
                const std::string_view tok = tokens[k + 1];
                // 角的四种写法：v、v/vt、v/vt/vn、v//vn。空段 = 缺项（哨兵）。
                const size_t s1 = tok.find('/');
                std::string_view fv = tok, fvt, fvn;
                if (s1 != std::string_view::npos) {
                    const size_t s2 = tok.find('/', s1 + 1);
                    if (s2 != std::string_view::npos &&
                        tok.find('/', s2 + 1) != std::string_view::npos)
                        return fail(lineTag(lineNo) + "f 的角 '" + std::string(tok) +
                                    "' 有多于两个 '/'");
                    fv = tok.substr(0, s1);
                    fvt = s2 == std::string_view::npos ? tok.substr(s1 + 1)
                                                       : tok.substr(s1 + 1, s2 - s1 - 1);
                    fvn = s2 == std::string_view::npos ? std::string_view{} : tok.substr(s2 + 1);
                }
                if (fv.empty())
                    return fail(lineTag(lineNo) + "f 的角 '" + std::string(tok) + "' 缺 v 索引");

                // 索引解析 + 越界检查。**越界是 D6 错误不是 clamp**：
                // render_backend.cpp:115 的 positions[idx] 没有边界保护，放一个越界索引
                // 进去就是渲染期的越界读（内存安全问题，不是格式洁癖）。
                const auto resolve = [&](std::string_view itok, int64_t count, const char* what,
                                         int64_t& idx) -> bool {
                    int64_t raw = 0;
                    if (!parseInt(itok, raw) || raw == 0)
                        return fail(lineTag(lineNo) + "f 的 " + what + " 索引 '" +
                                    std::string(itok) + "' 不是非零整数");
                    if (raw < 0) {
                        ++stats.negativeIndices;
                        // raw 可以是 INT64_MIN：count + raw 会符号溢出（UB）。先比较再相加，
                        // 越过开头的统一落到 -1，由下面的范围检查报错（解析器对不可信输入
                        // 的健壮性包括"不触发 UB"，不只是"不越界读"）。
                        idx = raw < -count ? -1 : count + raw;   // -1 ⇒ 最后一个
                    } else {
                        idx = raw - 1;       // OBJ 是 1-based
                    }
                    if (idx < 0 || idx >= count)
                        return fail(lineTag(lineNo) + "f 的 " + what + " 索引 '" +
                                    std::string(itok) + "' 越界（当前只有 " +
                                    std::to_string(count) + " 个 " + what + "）");
                    return true;
                };
                Corner& c = corners[k];
                if (!resolve(fv, static_cast<int64_t>(posTable.size()), "v", c.v)) return false;
                if (!fvt.empty() &&
                    !resolve(fvt, static_cast<int64_t>(vtTable.size()), "vt", c.vt))
                    return false;
                if (!fvn.empty() &&
                    !resolve(fvn, static_cast<int64_t>(vnTable.size()), "vn", c.vn))
                    return false;
            }
            stats.faceCorners += static_cast<int>(n);

            // 退化面：≥3 个角但**位置引用重复**（f 1 1 1 / 两点重合）⇒ 丢弃 + 计数（警告）。
            // 判定在 unroll 之前：被丢的面不产生顶点，也不计 facesWithoutNormals/facesUnrolled。
            // faceCorners 已经计了它 —— 那个计数度量的是文件输入，不是产出。
            bool degenerate = false;
            for (size_t a = 0; a < n && !degenerate; ++a)
                for (size_t b = a + 1; b < n; ++b)
                    if (corners[a].v == corners[b].v) {
                        degenerate = true;
                        break;
                    }
            if (degenerate) {
                warnings.push_back(lineTag(lineNo) + "f 的位置引用重复，丢弃退化面");
                continue;
            }

            // D7 的两条路，对顶点数的作用方向**相反**：
            // - vn 齐全 ⇒ 按 (v,vt,vn) 三元组去重（共享角合并，顶点变少）；
            // - 任一角缺 vn ⇒ **整面 unroll** 成独占顶点（顶点变多）。共享顶点无法携带
            //   两个面法线，这是几何事实不是实现选择。有 vn 的角保留声明值，缺的补面法线。
            //   为什么必须补**真**法线：render_backend.cpp:116 对缺失 normal 兜底 (0,1,0)，
            //   但那是安全网不是功能 —— 靠它的话 M6 一上光照，整个模型会朝 +Y 打光。
            bool anyMissingVn = false;
            bool allMissingVn = true;
            for (const Corner& c : corners) {
                if (c.vn < 0) anyMissingVn = true;
                else allMissingVn = false;
            }
            std::vector<uint32_t> vi(n);
            if (anyMissingVn) {
                ++stats.facesUnrolled;
                if (allMissingVn) ++stats.facesWithoutNormals;
                // 面法线 = normalize(cross(b-a, c-a))，右手方向跟着绕序走（D10 不翻转）。
                // 取前三个角；共线的相异位置叉积为零 ⇒ normalize 按 vec.h 约定返回零（无 NaN）。
                const Vec3 faceNormal =
                    normalize(cross(posTable[corners[1].v] - posTable[corners[0].v],
                                    posTable[corners[2].v] - posTable[corners[0].v]));
                for (size_t k = 0; k < n; ++k) {
                    const Corner& c = corners[k];
                    vi[k] = static_cast<uint32_t>(m.mesh.positions.size());
                    // unroll 的顶点**不进**去重 map：它们是独占的。
                    m.mesh.positions.push_back(posTable[c.v]);
                    m.mesh.normals.push_back(c.vn >= 0 ? vnTable[c.vn] : faceNormal);
                    // D9 翻转在出口；缺 vt ⇒ (0,0) 且**不翻转**（那是"没有 uv"，不是坐标）。
                    m.mesh.uvs.push_back(c.vt >= 0 ? uvOut(vtTable[c.vt]) : Vec2{0.0f, 0.0f});
                    // colors 不 push：OBJ 几何无顶点色，render_backend.cpp:118 兜底白色。
                }
            } else {
                for (size_t k = 0; k < n; ++k) {
                    const Corner& c = corners[k];
                    const TripleKey key{c.v, c.vt, c.vn};
                    const auto it = dedup.find(key);
                    if (it != dedup.end()) {
                        vi[k] = it->second;
                    } else {
                        vi[k] = static_cast<uint32_t>(m.mesh.positions.size());
                        dedup.emplace(key, vi[k]);
                        m.mesh.positions.push_back(posTable[c.v]);
                        m.mesh.normals.push_back(vnTable[c.vn]);
                        m.mesh.uvs.push_back(c.vt >= 0 ? uvOut(vtTable[c.vt])
                                                       : Vec2{0.0f, 0.0f});
                    }
                }
            }

            // 分组：只合并**相邻**的同材质 run，不重排（spec §7.5 "最小的 batching"）。
            // 单位是索引数组（与 drawRange 的 firstIndex/indexCount 相同）。被丢的退化面
            // 不产出索引，因此不会打断 run 的相邻性。
            if (m.groups.empty() || m.groups.back().material != currentMat)
                m.groups.push_back(
                    MeshGroup{static_cast<uint32_t>(m.mesh.indices.size()), 0, currentMat});
            // D8 扇形分解：f a b c d [e...] ⇒ (a,b,c) (a,c,d) [ (a,d,e) ...]，
            // 从第一个角出发，不改角序（D10）。n 角 ⇒ n-2 个三角形。
            for (size_t k = 1; k + 1 < n; ++k)
                m.mesh.indices.insert(m.mesh.indices.end(), {vi[0], vi[k], vi[k + 1]});
            const int tris = static_cast<int>(n) - 2;
            m.groups.back().indexCount += static_cast<uint32_t>(3 * tris);
            stats.triangles += tris;
            if (n == 4) stats.trianglesFromQuads += 2;
            else if (n >= 5) stats.trianglesFromNGons += tris;
            continue;
        }

        // o/g：已知指令，名字**不切分几何**（分组只按材质，spec §7.5）⇒ 接受但忽略。
        // mtllib：已知，但 parseObj 不做 IO（D2），名字由 scanObjMtllib 收集 ⇒ 接受但忽略。
        // 三者都不计 unrecognized（它们不是"未知指令"）。
        if (dir == "o" || dir == "g" || dir == "mtllib") continue;

        // 未知指令（s/vp/bevel/cset/...）：D3 忽略 + 计数 + 警告，不是错误。
        // A4：不许静默忽略 —— s off 被忽略后模型看起来是"平的"，用户会去怀疑法线插值。
        ++stats.unrecognized;
        warnings.push_back(lineTag(lineNo) + "未知指令 '" + std::string(dir) + "'");
    }

    stats.uniqueVertices = static_cast<int>(m.mesh.positions.size());
    m.mesh.recomputeBounds();
    out = std::move(m);
    return true;
}

bool scanObjMtllib(std::string_view text, std::vector<std::string>& out) {
    out.clear();
    size_t cursor = skipBom(text);
    while (cursor < text.size()) {
        const std::string_view line = nextLine(text, cursor);
        const size_t hash = line.find('#');
        const std::vector<std::string_view> tokens =
            tokenize(hash != std::string_view::npos ? line.substr(0, hash) : line);
        if (tokens.empty() || tokens[0] != "mtllib") continue;
        // 一行 `mtllib a.mtl b.mtl` 的每个 token 是一个文件名（OBJ 规范允许一行多个）。
        // 不解析几何：坏面/坏数字与 scan 无关，loadModel 先决定读哪些 .mtl 再 parseObj。
        for (size_t i = 1; i < tokens.size(); ++i)
            out.emplace_back(tokens[i].data(), tokens[i].size());
    }
    return true;   // 只扫名字，没有失败模式
}

}  // namespace sr