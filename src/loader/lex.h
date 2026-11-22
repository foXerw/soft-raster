#pragma once
// 两个文本解析器（mtl.cpp / obj.cpp）共用的**内部**词法层（M5.2 评审 Important 2 提炼）。
// header-only，随两个 .cpp 进 soft_raster_core，不新增编译单元、不进 CMake 源文件列表。
// **不是公共 API**：只给 src/loader 的解析器 include，不导出给 app/tests。
//
// 为什么共用一份而不是各写各的：tests/unit/test_util.h:16 已经写过这条教训 ——
// "副本的用法是先各自悄悄改样，然后两条测试对同一个函数给出不同的结论"。
// MTL 与 OBJ 对畸形输入的容忍必须同构（CRLF/裸 CR/BOM/tab/多空格/坏数字），
// 这份同构由"同一份代码"保证，而不是由"两份今天恰好一样的副本"保证。
// 提炼时以 obj.cpp 的帮手形态为准（nextLine/skipBom 比 mtl.cpp 原来的内联块更干净）。
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

namespace sr {
namespace lex {

// 整个 token 必须是一个有限数字才算成功：from_chars 不跳前导空白、不吃尾随垃圾
// （"0.5abc" 挡掉）。非有限值（inf/nan）也拒绝 —— NaN 进了材质 opacity 或顶点坐标
// 都会在后续阶段安静地长出来（Ruling 52 担心的那类延迟故障），不如在解析时就当坏参数报错。
inline bool parseFloat(std::string_view tok, float& v) {
    if (tok.empty()) return false;
    const char* first = tok.data();
    const char* last = first + tok.size();
    const auto r = std::from_chars(first, last, v, std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    return std::isfinite(v);
}

// 按空格/tab 切 token。行内的 \r\n 已在行切分阶段处理掉。
inline std::vector<std::string_view> tokenize(std::string_view line) {
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

// 行切分容忍 CRLF 与裸 CR：\r\n 算一个行尾，单独的 \r 或 \n 也算。
// Windows 导出器写 CRLF，老 Mac 写裸 CR。cursor 前进到下一行开头。
inline std::string_view nextLine(std::string_view text, size_t& cursor) {
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
// 返回正文起始偏移（有 BOM 为 3，否则 0）。
inline size_t skipBom(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        return 3;
    return 0;
}

}  // namespace lex
}  // namespace sr