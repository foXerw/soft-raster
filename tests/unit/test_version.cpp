#include "version.h"

#include "harness.h"

// buildInfoString 的存在理由：M8/M9 的每张性能表、每个 golden 都必须能归因到
// "哪个版本 + 哪种构建配置 + 哪个编译器"，否则优化数字无法复现也无法比较。

SR_TEST(build_info_string_is_nonempty) {
    SR_ASSERT(sr::buildInfoString()[0] != '\0');
}

SR_TEST(build_info_string_carries_the_project_name) {
    const char* s = sr::buildInfoString();
    // 前缀必须是项目名：HUD 与 notes 里的表格直接取这一行开头做标签。
    SR_ASSERT(s[0] == 's' && s[1] == 'o' && s[2] == 'f' && s[3] == 't');
}
