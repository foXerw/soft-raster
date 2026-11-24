#pragma once
// 手写 OBJ 解析器（M5.2）。文本 → Model，**不做 IO**（D2，与 mtl.h 同一条理由）：
// 给定字符串就能单测，畸形输入（CRLF、BOM、坏数字、越界索引）全部内联在测试里当夹具写。
// mtllib / map_Kd 的路径解析与文件读取是 loader/model.cpp（M5.4）的事，这里只认文本。
//
// 分层（C2）：本文件进 soft_raster_core，**不知道像素的存在**，也不碰那第三方图像头
// （grep 守卫仍只命中 src/loader/image_stb.*）。Model 里只有几何 + 材质表 + 分组。
//
// 已定死的裁决（spec §8，不重开）：
// - D6 全有或全无：任何结构性错误 ⇒ false、out 清空、err 带行号与肇事 token。
//   没有"跳过这一面继续"。半解析的模型比没有更糟（A1）。
// - D7 顶点身份是 (v, vt, vn) **三元组**，unordered_map 去重（只在加载期，不在热路径）；
//   缺 vn 的面 unroll 成独占顶点 + 面法线 —— 共享顶点无法携带两个面法线，这是几何事实。
//   两条规则对顶点数的作用方向**相反**：去重让它变少，unroll 让它变多，测试两个方向都钉。
// - D8 n-gon 从第一个角扇形分解：f a b c d ⇒ (a,b,c) (a,c,d)。
// - D9 vt 的 v 翻转（uvOut = {u, 1 - v}）发生在解析器的最后一站（obj.cpp），
//   契约见 mesh.cpp:49-52："格式的差异在格式的门口处理掉，不留进管线"。
//   缺 vt 的角 uv = (0,0) 且**不翻转**（D7）。
// - D10 不改角序、不翻转法线：OBJ 的"从正面看 CCW"与我们的 NDC 约定一致。
//   导出器写成 CW 的症状（模型内表面朝外）由 M5.6 的肉眼门抓，不做静默自动修复。
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loader/mtl.h"
#include "scene/mesh.h"

namespace sr {

// 一段索引区间 + 用哪个材质。**单位是索引数组**（与 BackendA::drawRange 的
// firstIndex/indexCount 同一个单位），半开区间 [indexOffset, indexOffset+indexCount)。
// indexCount 恒为 3 的倍数（D8 的扇形分解天生如此；drawRange 仍防御非 3 倍数，Ruling 45）。
struct MeshGroup {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    int32_t material = 0;   // 下标进 Model::materials
};

struct Model {
    Mesh mesh;                          // 全部几何一份 buffer；colors 保持空（见 parseObj 注释）
    std::vector<Material> materials;    // = parseObj 入参的拷贝 + 可能的 default 兜底，见下
    std::vector<MeshGroup> groups;      // 只合并**相邻**的同材质 run，不重排（spec §7.5）
};

// 计数的口径（手算 kitchen sink 期望值时按这个来）：
// - faceCorners：所有**结构合法**的 f 的角数总和，含随后被丢弃的退化面（它度量文件输入）。
// - triangles / uniqueVertices：实际产出（ indices.size()/3 与 positions.size() ）。
// - trianglesFromQuads / trianglesFromNGons：4 角面贡献 2 / ≥5 角面贡献 n-2；三角面不计。
// - facesWithoutNormals：**所有角都缺 vn** 的面数；facesUnrolled：被 unroll 的面数
//   （任一角缺 vn 就整面 unroll，所以 facesWithoutNormals ≤ facesUnrolled）。
//   只统计保留下来的面（退化面在 unroll 判定之前就被丢掉）。
// - uvOutOfRange：**按 vt 声明行**计，u 或 v 越出 [0,1] 记 1（一行两个分量都越界也只记 1；
//   未被引用的 vt 也计 —— 文件"说了奇怪的话"这件事发生在声明处）。
// - negativeIndices：按**分量**计，f 角里每个负索引（v/vt/vn 都算）记 1。
// - unrecognized：s/vp/bevel/cset/未知指令的条数（D3：忽略 + 计数 + 警告）。
//   o/g/mtllib 是已知指令，不计（o/g 的名字不切分几何，分组只按材质；mtllib 归 scanObjMtllib）。
//   parseObj 本身只计 OBJ 一侧；**经 loadModel 后本字段是 OBJ+MTL 合计**——parseMtl 的未知
//   属性计数（MtlStats.unrecognized）由 loadModel 在 parseObj 之前并入（终审 I1），
//   HUD 的 UNRECOGNIZED n 读的是这个合计。optionTokensSkipped 不并（口径不同，见 model.cpp）。
struct ObjStats {
    int triangles = 0, faceCorners = 0, uniqueVertices = 0;
    int trianglesFromQuads = 0, trianglesFromNGons = 0;
    int facesWithoutNormals = 0, facesUnrolled = 0;
    int uvOutOfRange = 0, negativeIndices = 0;
    int unrecognized = 0;                // s/vp/bevel/cset/未知指令
};

// 解析 OBJ 文本。成功返回 true；失败（D6）返回 false、**out 清空**、err 非空且带行号与肇事文本。
// warnings/stats 是累加出参，本函数**不得**重置它们。调用方按文件给全新的 warnings；
// stats 则可能已带着别处的计数（loadModel 在调用本函数前把 MTL 的 unrecognized 合并进来，
// 见 model.cpp 的合并点），所以"调用方一定给空表"这个假设是错的。
//
// **材质表的两条兜底规则（Ruling 46 / 收紧 3，materials 入参是只读 span，追加发生在 out 里）**：
// (a) materials.empty() ⇒ out.materials **恰好一个** defaultMaterial()，全部 group 指它；
//     此时即使出现 usemtl X（未知名）也只警告、复用这一个条目，**不再追加**（"恰好一个"优先）。
// (b) materials 非空而 usemtl X 不在表里 ⇒ **警告**（不是 D6 错误 —— 缺材质只损失 tint，
//     不损失几何），并在 out.materials **末尾追加一次** defaultMaterial()，之后所有未知名
//     复用同一个追加项。不得每个未知名追加一份：M5.4 数 group、去重贴图，N 份相同的默认
//     材质会把两边都弄脏。
// 第一条 usemtl 之前的面用 materials[0]（表空时即那个唯一的默认材质）。
//
// mesh.colors 保持**空**：OBJ 几何没有顶点色，render_backend.cpp:118 对缺失 color 兜底白色。
// 成功返回前调用 mesh.recomputeBounds()（fit 相机消费包围球）。
bool parseObj(std::string_view text, std::span<const Material> materials, Model& out,
              std::vector<std::string>& warnings, ObjStats& stats, std::string& err);

// 只扫 mtllib 名字，不解析几何：给 loadModel 决定"该读哪个 .mtl"用（D2：parse 不做 IO）。
// 一行 `mtllib a.mtl b.mtl` 的每个 token 是一个文件名（OBJ 规范允许一行多个）；
// 词法容忍与 parseObj 相同（BOM/CRLF/裸 CR/注释）。out 先清空再收集；没有失败模式，恒 true。
bool scanObjMtllib(std::string_view text, std::vector<std::string>& out);

}  // namespace sr
