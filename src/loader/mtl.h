#pragma once
// 手写 MTL 解析器（M5.1）。文本 → Material，**不做 IO**（D2，与 image_tga.h:11 同一条理由）：
// 给定字符串就能单测，畸形输入（CRLF、BOM、坏数字、缺文件名）全部内联在测试里当夹具写。
//
// 分层（C2）：本文件进 soft_raster_core，**不知道像素的存在**。Material::diffuseMap 刻意是
// 一个**文件名字符串**而不是图像/纹理 —— 把贴图读成像素是 loader/model.cpp（M5.4）的事，
// core 只负责把"引用了哪张贴图"这件事记下来。这样 core 不链 stb，那条第三方头的 grep
// 守卫仍只命中 src/loader/image_stb.*。
#include <string>
#include <string_view>
#include <vector>

#include "render/color.h"

namespace sr {

// 一个材质。颜色字段一律是**线性空间**：Kd/Ka/Ks 在文件里是 display-referred 的 sRGB 浮点，
// 解析时就解码成线性（D5），因为 FragmentUniforms::baseColor（pipeline.h:39-46）是 texturedFS
// 乘进采样结果的线性 tint —— 到那里时 diffuse 必须已经是线性，否则整体偏亮/偏暗一档。
struct Material {
    std::string name;
    // **默认纯白 1 1 1**，不是 pipeline.h 的 0.8 灰。baseColor 是乘性 tint：0.8 默认会让每个
    // 没带 MTL 的模型整体暗一档，症状读起来像"光照错了"（M4 的教训，golden_scene.cpp:110-112）。
    LinearColor diffuse{1.0f, 1.0f, 1.0f};
    LinearColor ambient{0.0f, 0.0f, 0.0f};   // Ka：M5 存而不用，M6 的环境光项
    LinearColor specular{0.0f, 0.0f, 0.0f};  // Ks：M6
    float shininess = 0.0f;                  // Ns：**原样存**（MTL 的 0–1000 指数），语义归 M6
    // d / Tr：M5 存而不用（FragmentOutput 无 alpha 通路，deferred #4）。方向见 parseMtl 注释
    // （Ruling 52）；范围在解析时夹到 [0,1]（终审 M9）。M6 消费时必须重审语义，不是照单全收。
    float opacity = 1.0f;
    std::string diffuseMap;                  // map_Kd 的**文件名**；core 不知道像素（C2）
};

// 上面那组默认值的单例。返回的引用**进程生命周期内有效**（函数内 static），
// M5.2 的 parseObj 会把它追加进 Model::materials 作兜底，所以必须稳定、可反复取地址。
const Material& defaultMaterial();

struct MtlStats {
    int unrecognized = 0;        // 未知指令条数（D3：忽略 + 计数 + 警告）
    int optionTokensSkipped = 0; // map_Kd 里被丢掉的选项 token 数（含选项自身与其参数，D17）
};

// 解析 MTL 文本。成功返回 true；失败（D6 全有或全无）返回 false、**out 清空**、err 非空且
// 带行号与肇事 token。半解析的材质列表比没有更糟，所以任何一个已知指令的坏参数都整体失败。
// 空文本/纯注释合法 ⇒ true + out 为空（让 obj 侧兜 defaultMaterial）。
// warnings 收未知指令与未知选项的提示（含行号）；stats 累计计数。
bool parseMtl(std::string_view text, std::vector<Material>& out,
              std::vector<std::string>& warnings, MtlStats& stats, std::string& err);

}  // namespace sr
