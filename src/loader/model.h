#pragma once
// 装配层（M5.4）：把"一个 OBJ + 它的 MTL + 它的贴图"变成一个可画对象。
// 这是 M5 里**唯一**同时需要 OBJ 数据与像素解码的地方，所以它只能进 soft_raster_loader：
//   - soft_raster_core 不知道存在一个第三方解码器（C2），故 core 不能读贴图；
//   - tests 链 core + loader 但不链 app，app 不可单测；
//   - 于是"几何 + 材质文本 + 贴图像素"的汇合点落在 loader，恰好能被 tests 直接判定。
//
// 分层事实（与 obj.h/mtl.h 同侧的说明）：parseObj/parseMtl 在 core，只认文本、不做 IO（D2）；
// 本文件是 IO 的唯一发生处 —— 读 OBJ、读 MTL、读贴图、打印 warnings 都在这里。
//
// 路径纪律（Ruling 62，硬性）：**任何**对 std::filesystem::path 调 .string() 都算缺陷。
// MSVC 英文 locale 下 path.string() 遇非 ASCII 路径会抛 system_error，未捕获异常 = CRT abort。
// 打开文件一律 std::ifstream(path)/ofstream(path)（原生宽字符）；消息里的路径一律
// sr::lex::displayPath(path)（u8string，无损）。本 TU 不出现 .string()。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "loader/obj.h"          // Model / MeshGroup / ObjStats（core，只认文本）
#include "render/pipeline.h"     // VertexUniforms / FragmentUniforms
#include "render/render_backend.h"  // RenderBackend / BackendA::drawRange
#include "render/texture.h"      // Texture / ImageRGBA8 / kMaxTextureSide

namespace sr {

// 一个载入完成的模型。**move-only**：贴图可能几百 MB，别按值传来传去。
//
// **textures 的生命周期不变式（设计 §11，硬性）**：FragmentUniforms::texture 是裸
// const Texture*（pipeline.h:44）。loadModel 返回后**不得再往 textures 里 push** ——
// vector 扩容会让所有已借出的指针悬垂。所有权：LoadedModel 拥有 textures 与 model；
// drawModel 每次按组从**当前** textures 取地址填进 FragmentUniforms，借用语义只活在那一次
// drawRange 调用内。只要 LoadedModel 活到帧末且 textures 不再增长，裸指针就成立。
// 这条不变式由 drawModel 入口的一道 **O(1)** 检查守（Ruling 66）：loadModel 成功末尾把
// 当时的 textures.size() 冻进 texturesFrozenAt；drawModel 入口比较 textures.size() 是否仍等于
// 它。要守的命题只有一句——"loadModel 返回后 textures 没有再增长过"——它需要的状态是**装载时
// 的那个 size**，不是内容。post-load 追加（或缩短）会让 size 失衡 ⇒ 打印消息并拒绝绘制，
// 而不是带着可能悬垂的指针继续画（与 drawRange 丢尾计数同一种"防御而非断言"的房风）。
// 零分配、零 map、每次 draw 一次比较。
struct LoadedModel {
    LoadedModel() = default;
    LoadedModel(const LoadedModel&) = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;
    LoadedModel(LoadedModel&&) = default;
    LoadedModel& operator=(LoadedModel&&) = default;

    Model model;                          // 几何 + 材质表 + 分组（parseObj 产出）
    std::vector<Texture> textures;        // 去重后的贴图，每条含完整 mip 链
    std::vector<int32_t> texturePerMaterial;  // 与 model.materials 平行；下标进 textures，-1 = 无贴图
    ObjStats stats;                       // parseObj 的计数（loadModel 给全新对象，见下）
    std::filesystem::path source;         // 载入用的 OBJ 路径（消息归因用）
    size_t texturesFrozenAt = 0;          // loadModel 成功末尾的 textures.size()；drawModel 入口守它（Ruling 66）
};

// 读 OBJ → 扫 mtllib → 逐个读 MTL 并 parseMtl → parseObj → 每个非空 map_Kd 读贴图建纹理。
// 成功 true；失败 false、out 清空、err 非空（带文件名/行号）。warnings 在本层打印到 stdout
// （含文件名），因为这是唯一同时看得见"文件"与"文本"的一层（D3）。
//
// **路径解析**：mtllib 名字相对 **OBJ 所在目录**；map_Kd 文件名相对 **MTL 所在目录**
// （Blender 的 apply 修改器常把贴图放在 mtl 旁边而非 obj 旁边）。去重键是
// weakly_canonical(mtlDir / 文件名)（D11），于是 `checker.tga` 与 `./checker.tga` 共享一条 mip 链。
//
// **文本里的文件名是 UTF-8（Ruling 65）**：mtllib 的每个名字、每个 diffuseMap 都来自 OBJ/MTL
// **文件内容内部**，是 UTF-8 字节串 —— 这与 argv/拖放路径（M5.6 nativePath 转的那一半）不同，
// M5.6 碰不到文件内容里的名字，所以必须在这里转。用 pathFromUtf8（char8_t 源构造 path，
// 见 model.cpp）按 UTF-8 解码，**不能**用 path(std::string)（MSVC 按本地 ANSI 码页解释，
// 中文贴图名会被误解码成"打不开文件"，症状是所有贴图变纯色 —— 正是 G-M5.1 要防的那类）。
//
// **警告 vs 错误的分界（收紧 3，写死在这里）**：
//   - mtllib 指向的文件**读不到** ⇒ **警告** + 用 defaultMaterial()，几何照常显示。
//     "文件根本没发"在现实中意味着作者没打算给材质，默认白就是正确解释。
//   - map_Kd 指向的文件**读不到** ⇒ **错误**（不降级）。err 里必须带上那个文件名。
//   - MTL 文件**存在但解析失败**（坏参数）⇒ **错误**（D6：半解析的材质表比没有更糟）。
//   后两者同属一个原则：它们都让材质信息退化成默认白，症状同样是"看起来正常但内容是纯色"
//   —— 最难被发现的一类错，所以都必须响亮失败，宁可让人去补/修文件。这与"缺 map_Kd 文件 =
//   错误"是同一原则的两个应用，不矛盾；而"缺整个 MTL 文件"是作者意图问题，降级为警告。
//
// **map_Kd 走哪个解码器（Ruling 63，结构性而非性能）**：按**大小写不敏感**的 .tga 后缀分派 ——
// `.tga` ⇒ loadTgaFile（我们自己的解析器）；其余 ⇒ loadImageFile（第三方解码器）。
// 理由：本里程碑的验证设计（§1/§10）建立在**三条锚、各守一个方向约定**上，锚 2 守的正是
// **我们 TGA 解析器**对 origin bit 的处理。cube.mtl（M5.7）引用 checker.tga，那条必须与 M4
// 逐字节相同的 golden 走的就是这条路径 —— 若 .tga 被悄悄路由到第三方，锚 2 守的将是一条
// 没有任何 golden 经过的代码，"两次翻转互相抵消"的分析也就对着错了对象。
// 推论：4096 上限（kMaxTextureSide）原先只在第三方路径强制，现在 TGA 侧也在 parseTga 里
// **分配之前**强制（image_tga.cpp），两条路径对称（关闭 M5.3 的 P15 观察）。
// 手写 TGA 读取器同时也是第三方缺席时的回退路径（spec §7.4）。
//
// **sRGB（D5 / Review Focus 5）**：贴图字节是 sRGB 编码的颜色数据，makeTexture(img,
// /*colorDataIsSrgb=*/true) 只设标志 + 建 mip 链，**不在加载时解码** —— 解码发生在采样时
// 逐纹素（sampler.cpp 的 toTexelF）。Kd 则已在 parseMtl 里解码成线性。两处不对称是故意的。
//
// warnings/stats 纪律：parseObj/parseMtl **不重置** warnings/stats（累加出参），所以本函数
// 给**每个**被解析的文件一套全新的 warnings/stats，否则计数会失去意义。
bool loadModel(const std::filesystem::path& objPath, LoadedModel& out, std::string& err);

// 逐组提交：每个 MeshGroup 一次 drawRange（C5：虚调用按 draw call 一次）。
// 每组的 FragmentUniforms 从 base 拷贝，再**按该组材质**覆写 baseColor = Kd（线性）、
// texture = 该材质的贴图指针（无贴图 ⇒ nullptr，texturedFS 退回 baseColor 纯色，Ruling 32）。
// base.sampler 原样保留（调用方决定 ClampEdge/Trilinear 等）。vertexShader/fragmentShader
// 用内置的 transformVS/texturedFS。stats 透传给 drawRange（drawCalls 按组累加）。
//
// 入口先守 textures 生命周期不变式（见 LoadedModel 注释）：违反 ⇒ 打印消息 + 拒绝绘制。
void drawModel(RenderBackend& backend, const LoadedModel& m, RenderTarget& target,
               const VertexUniforms& vu, const FragmentUniforms& base, FrameStats* stats);

}  // namespace sr