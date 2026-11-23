#include "loader/mtl.h"

#include <utility>

#include "loader/lex.h"

namespace sr {
namespace {

// 词法层与 obj.cpp 共用一份（loader/lex.h，提炼理由见该文件头部引的 test_util.h 教训）。
using lex::parseFloat;
using lex::tokenize;

bool startsWithDigit(std::string_view tok) {
    return !tok.empty() && tok[0] >= '0' && tok[0] <= '9';
}

// opacity 范围守卫（终审 M9）：文件里的 d/Tr 可以越界（d 2.0 ⇒ 2.0、Tr -0.5 ⇒ 1.5）。
// parseFloat 已拒非有限值，这里只夹到 [0,1]。M5 存而不用（无 alpha 通路，deferred #4）；
// **M6 消费 opacity 时必须重新审视 d/Tr 的语义**——方向由 Ruling 52 的测试钉死、范围由
// 这里钉死，两条都不是"M6 可以照单全收"的许可（与 D5 的 Kd 解码同一条"换 shader 时重审"纪律）。
float clampOpacity(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// D17 的 arity 表（M5.1 评审修订）：**只决定"少吞还是多吞"，不是白名单**。未知选项返回 -1，
// 走"丢自己 + 吞紧随的整串数字 token + 警告"那一支，而不是把整行报错。
// 只保留高置信度条目：原表凭印象写的 -mm=3 / -cc / -imf 都与真实 MTL 规范不符。错条目的代价
// 不是少吞而是**多吞** —— -mm 0 1 brick.png 会把 brick.png 当第三个参数吃掉 ⇒ 剩空串 ⇒
// 误判"缺文件名" ⇒ 整个文件加载失败（正是 G-M5.1 要防的那类事）。
// **表不能删干净**：-imfchan(r/g/b/a/l/z) 与 -clamp(on/off) 的参数**不是数字**，未知分支的
// 数字串规则接不住它们，所以这两条必须靠表。其余 -cc -fov -imf -pr -sa -tile_res -blend
// -boost -bobst -bump 一律删掉，交给未知路径（修订后的兜底能正确吞掉它们的数字参数）。
int optionArity(std::string_view opt) {
    if (opt == "-s" || opt == "-o" || opt == "-t") return 3;
    if (opt == "-bf" || opt == "-mm") return 2;
    if (opt == "-bm" || opt == "-texres" || opt == "-imfchan" || opt == "-clamp" ||
        opt == "-cl")
        return 1;
    return -1;
}

std::string joinFrom(const std::vector<std::string_view>& tokens, size_t start) {
    std::string s;
    for (size_t i = start; i < tokens.size(); ++i) {
        if (!s.empty()) s += ' ';
        s.append(tokens[i].data(), tokens[i].size());
    }
    return s;
}

}  // namespace

const Material& defaultMaterial() {
    // 函数内 static：引用进程生命周期内有效，M5.2 的 parseObj 会反复取它作兜底。
    static const Material kDefault{};
    return kDefault;
}

bool parseMtl(std::string_view text, std::vector<Material>& out,
              std::vector<std::string>& warnings, MtlStats& stats, std::string& err) {
    out.clear();
    err.clear();

    // 材质先建在本地 vector 里，成功才 move 进 out —— D6 全有或全无：任何一步失败都
    // 让 out 保持空，半解析的材质列表比没有更糟。cur 用**下标**而不是指针：emplace_back
    // 会让 vector 重新分配，指针会失效。
    std::vector<Material> mats;
    int cur = -1;

    auto fail = [&](std::string msg) {
        out.clear();
        err = std::move(msg);
        return false;
    };
    auto lineTag = [](int n) { return "第 " + std::to_string(n) + " 行: "; };

    // UTF-8 BOM 剥掉 + 行切分容忍 CRLF/裸 CR：与 OBJ 侧共用同一份词法（lex.h），
    // 两个解析器对畸形输入的容忍由"同一份代码"保证同构。
    size_t pos = lex::skipBom(text);
    int lineNo = 0;
    while (pos < text.size()) {
        std::string_view line = lex::nextLine(text, pos);
        ++lineNo;

        // '#' 起是注释；空行/纯注释跳过。
        const size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        const std::vector<std::string_view> tokens = tokenize(line);
        if (tokens.empty()) continue;

        const std::string_view dir = tokens[0];

        if (dir == "newmtl") {
            mats.emplace_back();
            cur = static_cast<int>(mats.size()) - 1;
            mats[cur].name = joinFrom(tokens, 1);
            continue;
        }

        // 其余都是"作用于当前材质"的指令：**已知**指令没有 newmtl 在前是结构性错误（D6——
        // 没有这道守卫，mats[cur] 就是 mats[-1] 的 UB）。**未知**指令即使在 newmtl 之前也走
        // D3（忽略 + 计数 + 警告，终审 P8）：它污染不了一个尚不存在的材质，报错只会白白毙掉
        // 头部带散落属性的真实文件。路由分界在这里，两条各有测试钉住。
        const bool known = dir == "Kd" || dir == "Ka" || dir == "Ks" || dir == "Ns" ||
                           dir == "d" || dir == "Tr" || dir == "map_Kd";
        if (cur < 0 && known)
            return fail(lineTag(lineNo) + "'" + std::string(dir) + "' 出现在任何 newmtl 之前");

        if (dir == "Kd" || dir == "Ka" || dir == "Ks") {
            if (tokens.size() < 4)
                return fail(lineTag(lineNo) + std::string(dir) + " 需要 3 个数字参数");
            float rgb[3];
            for (int k = 0; k < 3; ++k) {
                if (!parseFloat(tokens[k + 1], rgb[k]))
                    return fail(lineTag(lineNo) + std::string(dir) + " 的数字参数 '" +
                                std::string(tokens[k + 1]) + "' 解析失败");
            }
            // 三者都是 display-referred，走同一个浮点 sRGB 解码（D5 / Ruling 52）。
            const LinearColor lin = srgbToLinear(LinearColor{rgb[0], rgb[1], rgb[2]});
            if (dir == "Kd") mats[cur].diffuse = lin;
            else if (dir == "Ka") mats[cur].ambient = lin;
            else mats[cur].specular = lin;
            continue;
        }

        if (dir == "Ns" || dir == "d" || dir == "Tr") {
            if (tokens.size() < 2)
                return fail(lineTag(lineNo) + std::string(dir) + " 需要 1 个数字参数");
            float v;
            if (!parseFloat(tokens[1], v))
                return fail(lineTag(lineNo) + std::string(dir) + " 的数字参数 '" +
                            std::string(tokens[1]) + "' 解析失败");
            if (dir == "Ns") mats[cur].shininess = v;        // 原样存，语义归 M6
            else if (dir == "d") mats[cur].opacity = clampOpacity(v);          // d 是 opacity
            else mats[cur].opacity = clampOpacity(1.0f - v); // Tr 方向相反（Ruling 52），范围夹 [0,1]（M9）
            continue;
        }

        if (dir == "map_Kd") {
            // D17 状态机：开头的 -选项 连同参数一起丢，第一个非 - token 起重拼成文件名。
            size_t i = 1;
            while (i < tokens.size() && !tokens[i].empty() && tokens[i][0] == '-') {
                const std::string_view opt = tokens[i];
                ++i;
                ++stats.optionTokensSkipped;
                const int arity = optionArity(opt);
                if (arity >= 0) {
                    for (int k = 0; k < arity && i < tokens.size(); ++k) {
                        ++i;
                        ++stats.optionTokensSkipped;
                    }
                } else {
                    warnings.push_back(lineTag(lineNo) + "map_Kd 未知选项 '" + std::string(opt) + "'");
                    // 兜底必须比被兜底的表更稳：吞掉紧随其后的**整串**数字 token（一串，不是一个），
                    // 一直吃到第一个不以数字开头的 token。否则 -weird 1 2 3 file.png 会剩下
                    // "2 3 file.png" —— 一条静默给出错文件名的路径。判据用 startsWithDigit（首字符是
                    // 数字），不是"可解析为有限数"：已知限制（D17 修订，有意为之）是以数字开头的文件名
                    // （4k.png）也会被吞 ⇒ 报缺文件名，真实导出器不产生这种名字，支持它得违反 D2 做 IO。
                    while (i < tokens.size() && startsWithDigit(tokens[i])) {
                        ++i;
                        ++stats.optionTokensSkipped;
                    }
                }
            }
            if (i >= tokens.size())
                return fail(lineTag(lineNo) +
                            "map_Kd 之后没有文件名（只有 -s 等选项是真实导出器的常见形状，但缺了文件名）");
            mats[cur].diffuseMap = joinFrom(tokens, i);
            continue;
        }

        // 未知指令：D3 忽略 + 计数 + 警告，不是错误。
        ++stats.unrecognized;
        warnings.push_back(lineTag(lineNo) + "未知材质属性 '" + std::string(dir) + "'");
    }

    out = std::move(mats);
    return true;
}

}  // namespace sr
