#include "loader/model.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <utility>

#include "loader/image_stb.h"   // loadImageFile（第三方解码器，非 .tga 走这里）
#include "loader/image_tga.h"   // loadTgaFile（我们自己的解析器，.tga 走这里，Ruling 63）
#include "loader/lex.h"         // lex::displayPath（Ruling 62：消息里的路径不用 .string()）
#include "render/builtin_shaders.h"  // transformVS / texturedFS
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
    out.resize(static_cast<size_t>(len));
    f.seekg(0, std::ios::beg);
    if (len > 0 && !f.read(out.data(), len)) {
        err = lex::displayPath(p) + ": 读取失败";
        return false;
    }
    return true;
}

// warnings 在这一层打印（D3）：这是唯一同时看得见"文件"与"文本"的一层。
void printWarnings(const std::filesystem::path& file, const std::vector<std::string>& warnings) {
    const std::string display = lex::displayPath(file);
    for (const std::string& w : warnings)
        std::printf("[obj] %s: %s\n", display.c_str(), w.c_str());
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

// textures 生命周期不变式的可观测代理（见 model.h）：texturePerMaterial 里**不同**的非负
// 下标个数必须等于 textures.size()。loadModel 给每张去重后的贴图恰好建一条、且至少被一个
// 材质引用 ⇒ 构造上相等。post-load 往 textures 追加会让 textures.size() 变大而去重计数不变
// ⇒ 失衡，drawModel 入口据此拒绝绘制（带着可能悬垂的指针继续画才是真危险）。
bool texturesInvariantHolds(const LoadedModel& m) {
    if (m.texturePerMaterial.size() != m.model.materials.size()) return false;
    std::map<int32_t, bool> seen;
    for (const int32_t slot : m.texturePerMaterial) {
        if (slot < -1 || slot >= static_cast<int32_t>(m.textures.size())) return false;
        if (slot >= 0) seen[slot] = true;
    }
    return seen.size() == m.textures.size();
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
        const std::filesystem::path mtlPath = objDir / name;
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
        printWarnings(mtlPath, mtlWarnings);
        const std::filesystem::path mtlDir = mtlPath.parent_path();
        for (Material& mat : parsed) {
            materials.push_back(std::move(mat));
            materialMtlDir.push_back(mtlDir);
        }
    }

    // parseObj 自己填 materials（Ruling 46 的两条兜底），并产出几何 + 分组。
    // 同样给全新的 warnings/stats（out.stats 此刻是全新的，因为 out 刚被清空）。
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
        std::filesystem::path key = std::filesystem::weakly_canonical(mtlDir / map, ec);
        if (ec) key = (mtlDir / map).lexically_normal();   // 不抛：canonical 失败仍尝试加载

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
    return true;
}

void drawModel(RenderBackend& backend, const LoadedModel& m, RenderTarget& target,
               const VertexUniforms& vu, const FragmentUniforms& base, FrameStats* stats) {
    // 入口守 textures 生命周期不变式（model.h）。违反 ⇒ 打印消息 + 拒绝绘制：带着可能
    // 悬垂的裸指针继续画才是真危险。与 drawRange 的丢尾计数同一种"防御而非断言"的房风
    // （用消息而不是 <cassert>，也不往冻结的 FrameStats 里塞新字段）。
    if (!texturesInvariantHolds(m)) {
        std::fprintf(stderr, "[model] %s: textures 不变式被破坏（载入后追加了贴图？），拒绝绘制\n",
                     lex::displayPath(m.source).c_str());
        return;
    }

    PipelineState st;
    st.target = &target;
    st.vertexShader = transformVS;
    st.fragmentShader = texturedFS;   // 无贴图时它自己退回 baseColor（Ruling 32）
    st.vu = vu;
    st.stats = stats;
    // cull / depthTest / depthWrite 用 PipelineState 的默认值（与生产 draw 调用点一致）。

    for (const MeshGroup& g : m.model.groups) {
        // 材质下标防御：parseObj 保证合法，越界只跳过该组，不越界读 materials。
        if (g.material < 0 || g.material >= static_cast<int32_t>(m.model.materials.size()))
            continue;
        const Material& mat = m.model.materials[static_cast<size_t>(g.material)];
        FragmentUniforms fu = base;          // 保留调用方的 sampler 等
        fu.baseColor = mat.diffuse;          // 线性 Kd（parseMtl 已解码，D5）
        const int32_t slot = m.texturePerMaterial[static_cast<size_t>(g.material)];
        // 每次按组从**当前** textures 取地址：借用只活在这一次 drawRange 内（model.h 不变式）。
        fu.texture = slot >= 0 ? &m.textures[static_cast<size_t>(slot)] : nullptr;
        st.fu = fu;
        backend.drawRange(m.model.mesh, st, g.indexOffset, g.indexCount);
    }
}

}  // namespace sr