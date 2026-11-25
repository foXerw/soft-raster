#include "loader/model.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <utility>

#include "loader/image_stb.h"   // loadImageFile（第三方解码器，非 .tga 走这里）
#include "loader/image_tga.h"   // loadTgaFile（我们自己的解析器，.tga 走这里，Ruling 63）
#include "loader/lex.h"         // lex::displayPath（Ruling 62：消息里的路径不用 .string()）
#include "render/builtin_shaders.h"  // transformVS / texturedFS
#include "render/lights.h"      // MaterialShade（M6.1：着色需要的四个数）
#include "render/sampler.h"     // makeTexture

namespace sr {
namespace {

// 读整个文件进字符串。ifstream(fs::path) 走宽字符打开（Ruling 62 / D12），中文目录能读。
bool readWholeFile(const std::filesystem::path& p, std::string& out, std::string& err) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) {
        err = lex::displayPath(p) + ": 打不开文件";
        return false;
    }
    const std::streamoff len = f.tellg();
    if (len < 0) {
        err = lex::displayPath(p) + ": 读取失败";
        return false;
    }
    // 终审 M2：先问再分配（D13 哲学）。无上限的 resize 对一个多 GB 文件会抛 bad_alloc，
    // 而本层契约是 false + err，不是 terminate。上限在分配**之前**问，消息带常量名可归因。
    if (static_cast<size_t>(len) > kMaxModelTextBytes) {
        err = lex::displayPath(p) + ": 文件超过 kMaxModelTextBytes（" +
              std::to_string(kMaxModelTextBytes / (1024u * 1024u)) + " MiB），拒绝读取";
        return false;
    }
    // catch 是第二道：上限之内（256 MiB）的分配在内存紧张/32 位进程下仍可能失败，
    // 同样转成 false + err，不让任何异常逃出加载器（契约）。
    try {
        out.resize(static_cast<size_t>(len));
    } catch (const std::exception&) {
        err = lex::displayPath(p) + ": 读取失败（内存不足）";
        return false;
    }
    f.seekg(0, std::ios::beg);
    if (len > 0 && !f.read(out.data(), len)) {
        err = lex::displayPath(p) + ": 读取失败";
        return false;
    }
    return true;
}

// 文本内容里的文件名（mtllib 名、diffuseMap）是 **UTF-8 字节串**（Ruling 65）。按 UTF-8 解码
// 成 path：用 char8_t 源的 u8string_view 构造（C++20 起 path 对 char8_t 源按 UTF-8 解码）。
// 为什么是这个写法（两侧实测，零警告硬地板）：
//   - path(std::string)：MSVC 按**本地 ANSI 码页**解释字节，中文 UTF-8 名被误解码 ⇒ 打不开文件
//     （实测 MSVC 下 BAD）。这是"中文目录能载入、中文贴图名却找不到"的根源。
//   - std::filesystem::u8path：C++20 已废弃 ⇒ MSVC C4996/STL4021、g++ -Wdeprecated-declarations
//     两侧都告警，打破零警告地板。
//   - path(u8string) / path(char8_t 迭代器)：与本法等价且干净，但 u8string 会多一次临时分配；
//     u8string_view 零分配，故选它。
// 注意：这与 M5.6 nativePath（转 argv/拖放路径）是**两处不同的转换** —— 文件内容里的名字
// M5.6 碰不到，只能在这里转。reinterpret_cast 到 char8_t 是无损的字节重解释（UTF-8 → char8_t）。
std::filesystem::path pathFromUtf8(std::string_view name) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

// warnings 在这一层打印（D3）：这是唯一同时看得见"文件"与"文本"的一层。
// Ruling 75（人门验收时发现的真实缺陷）：原先逐条全打 —— 6233 面的真实模型吐了 45 行，
// 10 万面、满是 s 1/s off 的模型会吐几万行，症状是"程序看起来卡死了"，而真正的新闻
// （载入成功）被埋掉。现在最多前 8 条 + 一条汇总；**总数**仍在 stats.unrecognized，
// HUD 的 UNRECOGNIZED n 读的是它，不读打印行数 —— 上限只压噪声，不压信息。
// 这句话对 MTL 一侧也成立的前提是 loadModel 把 MtlStats.unrecognized 并进 out.stats
// （终审 I1，见下方合并点）—— 否则 printWarnings 对两个半区都调用，而 HUD 只读 OBJ 半区。
void printWarnings(const std::filesystem::path& file, const std::vector<std::string>& warnings) {
    constexpr size_t kMaxPrintedWarnings = 8;
    const std::string display = lex::displayPath(file);
    const size_t printed = warnings.size() < kMaxPrintedWarnings ? warnings.size()
                                                                 : kMaxPrintedWarnings;
    for (size_t i = 0; i < printed; ++i)
        std::printf("[obj] %s: %s\n", display.c_str(), warnings[i].c_str());
    if (warnings.size() > kMaxPrintedWarnings)
        std::printf("[obj] %s: … 还有 %zu 条同类警告\n", display.c_str(),
                    warnings.size() - kMaxPrintedWarnings);
}

// 大小写不敏感的 .tga 后缀判定（Ruling 63 的分派键）。用 u8string（无损）而不是 .string()
// （Ruling 62：后者遇非 ASCII 路径会抛）。".tga" 是 ASCII，u8string 字节与窄字符一致。
bool hasTgaExtension(const std::filesystem::path& p) {
    const std::u8string ext = p.extension().u8string();
    if (ext.size() != 4) return false;
    const char* e = reinterpret_cast<const char*>(ext.data());
    return e[0] == '.' && (e[1] == 't' || e[1] == 'T') && (e[2] == 'g' || e[2] == 'G') &&
           (e[3] == 'a' || e[3] == 'A');
}

}  // namespace

bool loadModel(const std::filesystem::path& objPath, LoadedModel& out, std::string& err) {
    out = LoadedModel{};
    err.clear();

    std::string text;
    if (!readWholeFile(objPath, text, err)) return false;   // err 已带 OBJ 路径
    const std::filesystem::path objDir = objPath.parent_path();

    // mtllib 名字相对 OBJ 目录。逐个读 MTL；读不到 ⇒ 警告（收紧 3），几何照常。
    std::vector<std::string> mtlNames;
    scanObjMtllib(text, mtlNames);
    std::vector<Material> materials;
    std::vector<std::filesystem::path> materialMtlDir;   // 与 materials 平行：map_Kd 相对它解析
    for (const std::string& name : mtlNames) {
        const std::filesystem::path mtlPath = objDir / pathFromUtf8(name);   // 文本名是 UTF-8（Ruling 65）
        std::string mtlText;
        std::string readErr;
        if (!readWholeFile(mtlPath, mtlText, readErr)) {
            // 收紧 3：缺 MTL 文件只损失 tint，不损失几何 ⇒ 警告而非 D6 错误。
            std::printf("[obj] %s: mtllib 读不到（%s），使用默认材质\n",
                        lex::displayPath(objPath).c_str(), readErr.c_str());
            continue;
        }
        // parseMtl 不重置 warnings/stats（累加出参）⇒ 每个文件给全新的一套。
        std::vector<Material> parsed;
        std::vector<std::string> mtlWarnings;
        MtlStats mtlStats;
        std::string mtlErr;
        if (!parseMtl(mtlText, parsed, mtlWarnings, mtlStats, mtlErr)) {
            // 文件存在但解析失败（坏参数）⇒ D6 错误：半解析的材质表比没有更糟。
            err = lex::displayPath(mtlPath) + ": " + mtlErr;
            out = LoadedModel{};
            return false;
        }
        // 终审 I1（整枝唯一的跨任务缺陷）：MTL 的未知属性计数并进 HUD 可见的总数。
        // HUD 的 UNRECOGNIZED n 读 ObjStats::unrecognized（main.cpp），而 parseObj 只计 OBJ
        // 指令 —— 不并的话 female02 的 44 条警告（MTL 12 + OBJ 32）在 HUD 上只显示 32。
        // 必须在 parseObj **之前**并：parseObj 对 stats 是累加（++/+=，不重置，已核对 obj.cpp），
        // 而 out 在函数入口刚被清空，所以这里并进去的 MTL 计数会被 parseObj 原样保留并叠加 OBJ 计数。
        // **optionTokensSkipped 不并**：它计的是已知指令 map_Kd 内部被跳过的选项 token（D17），
        // 其中多数是 -s/-o 这类合法选项 —— 不是"未知指令"。并进去会让 UNRECOGNIZED ≠ 警告条数，
        // 把正常解析标成"未识别"。未知选项的信息已由逐条 warning 承载（D17），不丢；
        // 计数本身留在 MtlStats 里，将来若要观测再单独立字段，不混进 D3 的口径。
        out.stats.unrecognized += mtlStats.unrecognized;
        printWarnings(mtlPath, mtlWarnings);
        const std::filesystem::path mtlDir = mtlPath.parent_path();
        for (Material& mat : parsed) {
            materials.push_back(std::move(mat));
            materialMtlDir.push_back(mtlDir);
        }
    }

    // parseObj 自己填 materials（Ruling 46 的两条兜底），并产出几何 + 分组。
    // warnings 是全新的一套；**stats 不是** —— out.stats 上面已经带着合并进来的 MTL 计数，
    // 这里传进去让 parseObj 继续往里累加。这正是 HUD 的 UNRECOGNIZED 覆盖 OBJ+MTL 两侧的机制，
    // 所以 parseObj **不得**重置 stats（它只 ++/+=，由 test_model 的合计断言钉住）。
    std::vector<std::string> objWarnings;
    if (!parseObj(text, materials, out.model, objWarnings, out.stats, err)) {
        err = lex::displayPath(objPath) + ": " + err;
        out = LoadedModel{};
        return false;
    }
    printWarnings(objPath, objWarnings);

    // 每个非空 map_Kd：相对 MTL 目录 → weakly_canonical → 查重 → 解码 → makeTexture。
    // parseObj 可能追加了兜底材质（diffuseMap 为空），所以按 out.model.materials 遍历。
    out.texturePerMaterial.assign(out.model.materials.size(), -1);
    std::map<std::filesystem::path, int32_t> dedup;
    for (size_t i = 0; i < out.model.materials.size(); ++i) {
        const std::string& map = out.model.materials[i].diffuseMap;
        if (map.empty()) continue;   // 无贴图 ⇒ slot 保持 -1
        // 兜底追加的材质 diffuseMap 必为空，走不到这里；i 越界只是防御。
        const std::filesystem::path mtlDir =
            i < materialMtlDir.size() ? materialMtlDir[i] : objDir;
        std::error_code ec;
        const std::filesystem::path texPath = mtlDir / pathFromUtf8(map);   // 文本名是 UTF-8（Ruling 65）
        std::filesystem::path key = std::filesystem::weakly_canonical(texPath, ec);
        if (ec) key = texPath.lexically_normal();   // 不抛：canonical 失败仍尝试加载

        const auto it = dedup.find(key);
        if (it != dedup.end()) {
            out.texturePerMaterial[i] = it->second;
            continue;
        }

        // Ruling 63 分派：.tga ⇒ 我们的解析器（锚 2 守的那条路径）；其余 ⇒ 第三方。
        ImageRGBA8 img;
        std::string imgErr;
        const bool loaded = hasTgaExtension(key) ? loadTgaFile(key, img, imgErr)
                                                 : loadImageFile(key, img, imgErr);
        if (!loaded) {
            // 收紧 3：贴图缺失是错误（症状是"看起来正常但纯色"，最难发现）。err 已带文件名。
            err = imgErr;
            out = LoadedModel{};
            return false;
        }
        // D5：只设 srgb 标志 + 建 mip 链，**不在加载时解码**（解码在采样时的 toTexelF）。
        out.textures.push_back(makeTexture(std::move(img), /*colorDataIsSrgb=*/true));
        const int32_t slot = static_cast<int32_t>(out.textures.size()) - 1;
        dedup.emplace(key, slot);
        out.texturePerMaterial[i] = slot;
    }

    out.source = objPath;
    out.texturesFrozenAt = out.textures.size();   // 冻住装载时的 size（Ruling 66，drawModel 入口守它）
    return true;
}

void drawModel(RenderBackend& backend, const LoadedModel& m, RenderTarget& target,
               const VertexUniforms& vu, const FragmentUniforms& base, FrameStats* stats,
               FragmentShaderFn fsOverride, VertexShaderFn vsOverride) {
    // 入口守 textures 生命周期不变式（model.h，Ruling 66）：要守的命题是"loadModel 返回后
    // textures 没有再增长过"，只需比较装载时冻住的 size —— O(1)、零分配。违反（追加或缩短）
    // ⇒ 打印消息 + 拒绝绘制：带着可能悬垂的裸指针继续画才是真危险（FragmentUniforms::texture
    // 是裸 const Texture*，vector 扩容会让旧指针失效）。与 drawRange 丢尾计数同一种"防御而非
    // 断言"的房风（用消息而不是 <cassert>，也不往冻结的 FrameStats 里塞新字段）。
    if (m.textures.size() != m.texturesFrozenAt) {
        std::fprintf(stderr, "[model] %s: textures 不变式被破坏（载入后追加了贴图？），拒绝绘制\n",
                     lex::displayPath(m.source).c_str());
        return;
    }

    PipelineState st;
    st.target = &target;
    // vsOverride（M6.6）：着色模式的 VS 覆写（Gouraud 的 lambertVS）；nullptr = 既有 transformVS。
    st.vertexShader = (vsOverride != nullptr) ? vsOverride : transformVS;
    // fsOverride（M6.5/D23）：视图计划的 FS 覆写（法线视图）；nullptr = 既有语义。
    // 无贴图时 texturedFS 自己退回 baseColor（Ruling 32）。
    st.fragmentShader = (fsOverride != nullptr) ? fsOverride : texturedFS;
    st.vu = vu;
    st.stats = stats;
    // cull / depthTest / depthWrite 用 PipelineState 的默认值（与生产 draw 调用点一致）。

    // M6.1（D15）：逐组一份 MaterialShade，栈上存储、循环外复用同一块 ——
    // 指针只活在这一次 drawRange 内（同步调用），不 push 进容器（C5：无谓的分配）。
    // vu.mat 与 fu.mat 指向**同一个**对象（单一真源）：M6.3 的 lambertVS 要在 VS 侧
    // 读 diffuse（albedo = 顶点色 × Kd），两个阶段必须读同一份。
    // 为什么不是 const Material*：Material 还带着 name/diffuseMap/opacity，
    // 着色层不该读（见 lights.h 的 MaterialShade 声明处注释）。
    MaterialShade shade;
    for (const MeshGroup& g : m.model.groups) {
        // 材质下标防御：parseObj 保证合法，越界只跳过该组，不越界读 materials。
        if (g.material < 0 || g.material >= static_cast<int32_t>(m.model.materials.size()))
            continue;
        const Material& mat = m.model.materials[static_cast<size_t>(g.material)];
        FragmentUniforms fu = base;          // 保留调用方的 sampler 等
        fu.baseColor = mat.diffuse;          // 线性 Kd（parseMtl 已解码，D5）
        // 四个数原样抄（颜色已在 parseMtl 解码，这里不再动曲线）；
        // shininess 原样直传（含 0）—— 夹取与 Ns==0 的判断都在 shading.h（M6.2）。
        shade.diffuse = mat.diffuse;
        shade.ambient = mat.ambient;
        shade.specular = mat.specular;
        shade.shininess = mat.shininess;
        fu.mat = &shade;
        st.vu.mat = &shade;
        // 槽位防御（终审 M1/P21）：上面 g.material 只对 materials.size() 做了界检查，slot 还需
        // 自己对 textures.size() 做界检查 —— 冻结不变式（Ruling 66）只禁 textures 载入后增长，
        // 不禁这几个 public vector 互相失配（M8 的 binning 直接消费它们）。texturePerMaterial
        // 同理：loadModel 把它 assign 成 materials.size()，但载入后无人守它不缩短。
        // 越界/失配一律当"无贴图"（texturedFS 退回 Kd 纯色），与材质下标检查同一种
        // "防御而非断言"的房风：跳过贴图，不越界读。
        const bool matInTable = static_cast<size_t>(g.material) < m.texturePerMaterial.size();
        const int32_t slot = matInTable ? m.texturePerMaterial[static_cast<size_t>(g.material)] : -1;
        // 每次按组从**当前** textures 取地址：借用只活在这一次 drawRange 内（model.h 不变式）。
        fu.texture = (slot >= 0 && static_cast<size_t>(slot) < m.textures.size())
                         ? &m.textures[static_cast<size_t>(slot)]
                         : nullptr;
        st.fu = fu;
        backend.drawRange(m.model.mesh, st, g.indexOffset, g.indexCount);
    }
}

}  // namespace sr
