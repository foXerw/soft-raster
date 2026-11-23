#pragma once
// app 侧的编码转换入口（D18）。Win32-only、不可单测 ⇒ 每个函数只做一件事、保持极小；
// 可测的两半各有测试（loader 的中文目录用例、cmdline 的 ASCII 用例），argv 那一半由
// 产品方肉眼门兜底（亲手在 Git Bash 里跑一次带中文路径的 --obj）。
#include <filesystem>
#include <string>

namespace app {

// UTF-8 → 原生路径。**全仓库唯一的这类转换点**（D18 第 3 条）：MSVC 的
// path(std::string) 按本地 ANSI 码页（本机 936）解释字节，Git Bash/MSYS2 传来的
// UTF-8 argv 直接构造 path 会 mojibake 成"打不开文件"。只转 **argv 与拖放**；
// 文件内容里的文件名（mtllib/map_Kd）是另一处转换（loadModel 的 pathFromUtf8，
// Ruling 65）——两个角色不同，别"统一"它们。
std::filesystem::path nativePath(const std::string& utf8);

// UTF-16 → UTF-8（反方向帮手）：argv（CommandLineToArgvW）与拖放（DragQueryFileW）
// 两个 Win32 边界给的都 UTF-16，在这里统一成 UTF-8 再交给 parseCmdline / InputState。
// 与 nativePath 同文件：仓库里所有编码转换都在这一个文件里看得见。
std::string utf8FromWide(const std::wstring& wide);

}  // namespace app
