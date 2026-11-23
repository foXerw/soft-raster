#include "loader/obj.h"

#include <string>
#include <string_view>
#include <vector>

#include "harness.h"

namespace {

// 内联字符串夹具，无 IO（D2）：每次调用都给一套全新的 out/warnings/stats/err，
// 这样"out 清空""err 非空"这类断言不会被上一个用例的残留污染（与 test_mtl 同一形状）。
struct ObjResult {
    sr::Model out;
    std::vector<std::string> warnings;
    sr::ObjStats stats;
    std::string err;
    bool ok = false;
};

ObjResult parse(std::string_view text, std::vector<sr::Material> mats = {}) {
    ObjResult r;
    r.ok = sr::parseObj(text, mats, r.out, r.warnings, r.stats, r.err);
    return r;
}

sr::Material namedMaterial(const char* name, float red) {
    sr::Material m;
    m.name = name;
    m.diffuse = sr::LinearColor{red, 0.0f, 0.0f};
    return m;
}

}  // namespace

// ---- Step 1: 位置/法线/uv 的读取与三元组去重 -------------------------------

SR_TEST(obj_parses_positions_normals_and_uvs_into_soa_arrays) {
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vn 0 0 1\n"
        "vt 0.25 0.25\n"
        "f 1/1/1 2/1/1 3/1/1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 3);
    SR_ASSERT_EQ(r.out.mesh.normals.size(), 3);
    SR_ASSERT_EQ(r.out.mesh.uvs.size(), 3);
    // OBJ 几何没有顶点色：colors 保持空，render_backend.cpp:118 兜底白色。
    SR_ASSERT(r.out.mesh.colors.empty());
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 3);
    // SR_ASSERT 失败不中止：下标访问一律先判尺寸，RED 状态下才不至于越界读成 UB。
    if (r.out.mesh.positions.size() == 3 && r.out.mesh.indices.size() == 3) {
        SR_ASSERT_NEAR(r.out.mesh.positions[1].x, 1.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.positions[2].y, 1.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.normals[0].z, 1.0f, 1e-5f);
        // 这里只钉 u：v 的翻转由 Step 6 的锚测试负责（变异①的红要落在那里，不散射）。
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].x, 0.25f, 1e-5f);
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
    }
    SR_ASSERT_EQ(r.stats.triangles, 1);
    SR_ASSERT_EQ(r.stats.faceCorners, 3);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 3);
}

SR_TEST(obj_dedups_vertices_by_the_v_vt_vn_triple) {
    // 同一个几何顶点 v1 在两个面里配**不同的 vt** ⇒ 必须是两个顶点：键是三元组不是 vIdx。
    // 共享角 (2,1,1)/(3,1,1) 被第二面复用 ⇒ uniqueVertices(4) < faceCorners(6)。
    // 变异②（键退化成只用 vIdx）会把 (1,2,1) 并进 (1,1,1) ⇒ 4→3、indices[3] 3→0，两条都红。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 1\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/1/1 3/1/1\n"
        "f 1/2/1 2/1/1 3/1/1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.faceCorners, 6);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 4);   // (1,1,1)(2,1,1)(3,1,1)(1,2,1)
    SR_ASSERT(r.stats.uniqueVertices < r.stats.faceCorners);   // map 真的合并了共享角
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 4);
    if (r.out.mesh.indices.size() == 6) {
        SR_ASSERT_EQ(r.out.mesh.indices[3], 3);   // v1+vt2 是新顶点，不是复用 0
        SR_ASSERT_EQ(r.out.mesh.indices[4], 1);   // (2,1,1) 共享
        SR_ASSERT_EQ(r.out.mesh.indices[5], 2);   // (3,1,1) 共享
    }
    if (r.out.mesh.uvs.size() == 4) {
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].x, 0.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.uvs[3].x, 1.0f, 1e-5f);   // 同位置不同 uv，各存各的
    }
}

SR_TEST(obj_negative_indices_count_from_the_end) {
    // OBJ 的负索引从**当前已声明的末尾**往回数：-1 = 最后一个。三个 v ⇒ -3/-2/-1 = 1/2/3。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f -3 -2 -1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.negativeIndices, 3);   // 按分量计：三个负 v 索引
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 3);
    if (r.out.mesh.indices.size() == 3 && r.out.mesh.positions.size() == 3) {
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
        SR_ASSERT_NEAR(r.out.mesh.positions[0].x, 0.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.positions[1].x, 1.0f, 1e-5f);
    }
}

SR_TEST(obj_trims_each_directive_to_the_fields_it_has) {
    // v 可带 w（忽略，不支持有理顶点）；vt 可只写 u（v 缺省 0）或带 w（忽略）；
    // f 的角混合四种写法：v/vt/vn、v/vt、v//vn、裸 v。每条指令只读它有的字段。
    const ObjResult r = parse(
        "v 0 0 0 1\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0.25\n"
        "vt 0.5 0.25 0\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2 3//1\n"
        "f 1 2 3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.faceCorners, 6);
    SR_ASSERT_EQ(r.stats.triangles, 2);
    // 两个面的缺项模式不同，角不跨面合并 ⇒ 6 个顶点（unroll 后同样是 6，见 Step 3）。
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 6);
    SR_ASSERT_EQ(r.out.mesh.uvs.size(), 6);
    if (r.out.mesh.uvs.size() == 6) {
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].x, 0.25f, 1e-5f);   // vt 只写了 u
        SR_ASSERT_NEAR(r.out.mesh.uvs[1].x, 0.5f, 1e-5f);    // vt 带 w，w 被忽略
        SR_ASSERT_NEAR(r.out.mesh.uvs[2].x, 0.0f, 1e-5f);    // 角缺 vt ⇒ (0,0)
        SR_ASSERT_NEAR(r.out.mesh.uvs[2].y, 0.0f, 1e-5f);    // 缺 vt 不翻转（D7/D9），恒 (0,0)
        SR_ASSERT_NEAR(r.out.mesh.uvs[3].x, 0.0f, 1e-5f);    // 裸 v 面 ⇒ 全 (0,0)
    }
    // Step 3 补：任一角缺 vn ⇒ 整面 unroll。面 1 有一个角缺 vn，面 2 全缺。
    SR_ASSERT_EQ(r.stats.facesUnrolled, 2);
    SR_ASSERT_EQ(r.stats.facesWithoutNormals, 1);   // 只有面 2 是"所有角都缺 vn"
    if (r.out.mesh.normals.size() == 6) {
        // 面 1 角 2 缺 vn ⇒ 补面法线 cross(v2-v1, v3-v1) = (0,0,1)，不是零也不是 (0,1,0)。
        SR_ASSERT_NEAR(r.out.mesh.normals[1].z, 1.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.normals[4].z, 1.0f, 1e-5f);   // 面 2 的面法线
    }
}

// ---- Step 2: 面类型 ---------------------------------------------------------

SR_TEST(obj_triangulates_quads_as_fans) {
    // D8：f a b c d ⇒ (a,b,c) (a,c,d)，从第一个角扇形分解。
    // 变异④（改成 (a,b,c),(b,c,d)）会让 indices[3..5] 从 0,2,3 变成 1,2,3 ⇒ 红。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "vn 0 0 1\n"
        "f 1//1 2//1 3//1 4//1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.triangles, 2);
    SR_ASSERT_EQ(r.stats.trianglesFromQuads, 2);
    SR_ASSERT_EQ(r.stats.trianglesFromNGons, 0);
    SR_ASSERT_EQ(r.stats.faceCorners, 4);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 4);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 6);
    if (r.out.mesh.indices.size() == 6) {
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
        SR_ASSERT_EQ(r.out.mesh.indices[3], 0);   // 扇形回到第一个角
        SR_ASSERT_EQ(r.out.mesh.indices[4], 2);
        SR_ASSERT_EQ(r.out.mesh.indices[5], 3);
    }
}

SR_TEST(obj_triangulates_pentagons_as_fans_and_counts) {
    // 5 角 ⇒ 3 个三角形：(a,b,c) (a,c,d) (a,d,e)，全部计进 trianglesFromNGons。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1.5 1 0\n"
        "v 0.5 1.5 0\n"
        "v -0.5 1 0\n"
        "vn 0 0 1\n"
        "f 1//1 2//1 3//1 4//1 5//1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.triangles, 3);
    SR_ASSERT_EQ(r.stats.trianglesFromNGons, 3);
    SR_ASSERT_EQ(r.stats.trianglesFromQuads, 0);
    SR_ASSERT_EQ(r.stats.faceCorners, 5);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 9);
    if (r.out.mesh.indices.size() == 9) {
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
        SR_ASSERT_EQ(r.out.mesh.indices[3], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[4], 2);
        SR_ASSERT_EQ(r.out.mesh.indices[5], 3);
        SR_ASSERT_EQ(r.out.mesh.indices[6], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[7], 3);
        SR_ASSERT_EQ(r.out.mesh.indices[8], 4);
    }
}

SR_TEST(obj_drops_degenerate_faces_without_failing) {
    // **退化 ≠ 畸形**：≥3 个角但位置引用重复（导出器常写）⇒ 丢弃 + 警告，不是 D6 错误；
    // 少于 3 个角才是错误（下一条用例）。退化判定在 unroll 之前：被丢的面不产生顶点、
    // 也不计 facesWithoutNormals/facesUnrolled，但 faceCorners 计（它度量文件输入）。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vn 0 0 1\n"
        "f 1 1 1\n"          // 三点重合（裸 v，缺 vn 也不该 unroll）
        "f 1//1 2//1 2//1\n" // 两点重合
        "f 1//1 2//1 3//1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.warnings.size(), 2);          // 每个被丢的面一条警告
    SR_ASSERT_EQ(r.stats.triangles, 1);
    SR_ASSERT_EQ(r.stats.faceCorners, 9);        // 3+3+3，含被丢的两个
    SR_ASSERT_EQ(r.stats.uniqueVertices, 3);
    SR_ASSERT_EQ(r.stats.facesUnrolled, 0);      // 退化面先丢，轮不到 unroll
    SR_ASSERT_EQ(r.stats.facesWithoutNormals, 0);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 3);
    if (r.out.mesh.indices.size() == 3) {
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
    }
}

SR_TEST(obj_mid_group_degenerate_face_keeps_groups_contiguous) {
    // P9（M5.2 遗留，M5.4 补）：上一条退化面夹具丢的是**最前面**的面（group 还没建）。
    // 这条把退化面放在 group **中间**（已发射若干索引之后）：丢弃必须不打断 run 的相邻性、
    // 不留缝隙，indexOffset/indexCount 仍首尾相接且 %3==0 —— drawModel 正是按这些偏移喂
    // drawRange，失去同步就是越界或漏画。变异：若退化面不被丢（或丢弃挪到发射之后），
    // indices.size()/triangles/indexCount 会失衡 ⇒ 下面的连续性断言红。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "v 1 1 0\n"
        "usemtl A\n"
        "f 1 2 3\n"        // 保留：发射 3 索引，group A 建立
        "f 1 1 1\n"        // 退化：丢弃（group 已存在、已有索引）
        "f 1 3 4\n",       // 保留：再发射 3 索引 ⇒ 同组累计 6
        {namedMaterial("A", 1.0f)});
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.groups.size(), 1);      // 退化面不打断 run
    SR_ASSERT_EQ(r.stats.triangles, 2);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 6);
    SR_ASSERT_EQ(r.warnings.size(), 1);        // 仅退化面一条警告
    uint32_t covered = 0;
    for (const sr::MeshGroup& g : r.out.groups) {
        SR_ASSERT_EQ(g.indexCount % 3, 0);
        SR_ASSERT_EQ(g.indexOffset, covered);  // 首尾相接，无缝隙无重叠
        covered += g.indexCount;
    }
    SR_ASSERT_EQ(covered, r.out.mesh.indices.size());
}

SR_TEST(obj_rejects_faces_with_fewer_than_three_vertices) {
    // 少于 3 个角是 D6 结构性错误：false + out 清空 + err 带行号。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "f 1 2\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.mesh.positions.empty());
    SR_ASSERT(r.out.mesh.indices.empty());
    SR_ASSERT(r.out.groups.empty());
    SR_ASSERT(!r.err.empty());
    SR_ASSERT(r.err.find("第 3 行") != std::string::npos);

    // 光杆 f 同样报错（0 个角）。
    const ObjResult bare = parse("v 0 0 0\nf\n");
    SR_ASSERT_FALSE(bare.ok);
    SR_ASSERT(bare.out.mesh.indices.empty());
    SR_ASSERT(bare.err.find("第 2 行") != std::string::npos);
}

// ---- Step 3: 缺 vn 的面走 unroll（D7）--------------------------------------

SR_TEST(obj_unrolls_faces_that_have_no_normals) {
    // 两个**完全相同**的缺 vn 面 ⇒ 各拿 3 个独占顶点，共 6 个（不是去重成 3 个）：
    // 共享顶点无法携带两个面法线，这是几何事实不是风格选择（D7）。
    // 顶点"存在属性"不等于"属性正确"：render_backend.cpp:116-118 对缺失属性有兜底
    // （normal→(0,1,0)、uv→(0,0)），**但那是安全网不是功能** —— 缺 vn 必须在解析器里
    // 补真法线，否则 M6 一上光照，整个模型会朝 +Y 打光。
    // 变异③（删掉 unroll 分支 ⇒ 哨兵键去重）会把 6 个顶点并成 3 个 ⇒ 顶点数断言红。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"
        "f 1 2 3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.faceCorners, 6);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 6);      // unroll 让顶点数**膨胀**（与去重方向相反）
    SR_ASSERT_EQ(r.stats.facesWithoutNormals, 2);
    SR_ASSERT_EQ(r.stats.facesUnrolled, 2);
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 6);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 6);
    if (r.out.mesh.indices.size() == 6) {
        SR_ASSERT_EQ(r.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(r.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(r.out.mesh.indices[2], 2);
        SR_ASSERT_EQ(r.out.mesh.indices[3], 3);   // 第二面独占，不复用 0/1/2
        SR_ASSERT_EQ(r.out.mesh.indices[4], 4);
        SR_ASSERT_EQ(r.out.mesh.indices[5], 5);
    }
    if (r.out.mesh.normals.size() == 6) {
        for (int k = 0; k < 6; ++k) {
            SR_ASSERT_NEAR(r.out.mesh.normals[k].x, 0.0f, 1e-5f);
            SR_ASSERT_NEAR(r.out.mesh.normals[k].y, 0.0f, 1e-5f);
            SR_ASSERT_NEAR(r.out.mesh.normals[k].z, 1.0f, 1e-5f);   // 归一化面法线
        }
    }
    if (r.out.mesh.uvs.size() == 6) {
        for (int k = 0; k < 6; ++k) {
            SR_ASSERT_NEAR(r.out.mesh.uvs[k].x, 0.0f, 1e-5f);   // 缺 vt ⇒ (0,0)，不翻转
            SR_ASSERT_NEAR(r.out.mesh.uvs[k].y, 0.0f, 1e-5f);
        }
    }
}

SR_TEST(obj_face_normal_matches_the_winding_of_the_face) {
    // 面法线 = normalize(cross(b-a, c-a))，右手方向跟着角的绕序走：
    // 1 2 3 是 CCW ⇒ +Z；1 3 2 是 CW ⇒ -Z（D10：解析器**不**自动翻转）；
    // 1 4 5 的叉积长 4 ⇒ 归一化后仍是 +Z。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "v 2 0 0\n"
        "v 0 2 0\n"
        "f 1 2 3\n"
        "f 1 3 2\n"
        "f 1 4 5\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.mesh.normals.size(), 9);
    if (r.out.mesh.normals.size() == 9) {
        SR_ASSERT_NEAR(r.out.mesh.normals[0].z, 1.0f, 1e-5f);    // CCW ⇒ +Z
        SR_ASSERT_NEAR(r.out.mesh.normals[3].z, -1.0f, 1e-5f);   // CW ⇒ -Z，不自动修复
        SR_ASSERT_NEAR(r.out.mesh.normals[6].z, 1.0f, 1e-5f);    // 非单位叉积被归一化
        SR_ASSERT_NEAR(r.out.mesh.normals[6].x, 0.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.normals[6].y, 0.0f, 1e-5f);
    }
}

SR_TEST(obj_normals_and_uv_stay_shared_when_vn_is_present) {
    // 与 Step 1 去重断言**方向相反**的守卫：vn 齐全时绝不 unroll。
    // 同一位置配两个不同 vn ⇒ 三元组不同 ⇒ 6 个顶点全保留（去重在这里帮不上忙，也不该帮）。
    // 变异②（键退化成 vIdx）会把 6 并成 3 ⇒ 红；变异③（删 unroll）不影响本条（vn 齐全）。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0.25 0.25\n"
        "vn 0 0 1\n"
        "vn 1 0 0\n"
        "f 1/1/1 2/1/1 3/1/1\n"
        "f 1/1/2 2/1/2 3/1/2\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.faceCorners, 6);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 6);   // vn 不同 ⇒ 不合并
    SR_ASSERT_EQ(r.stats.facesUnrolled, 0);    // vn 齐全 ⇒ 不 unroll
    SR_ASSERT_EQ(r.out.mesh.normals.size(), 6);
    if (r.out.mesh.normals.size() == 6) {
        SR_ASSERT_NEAR(r.out.mesh.normals[0].z, 1.0f, 1e-5f);   // 第一面用 vn1
        SR_ASSERT_NEAR(r.out.mesh.normals[3].x, 1.0f, 1e-5f);   // 第二面用 vn2，不被平均/覆盖
        SR_ASSERT_NEAR(r.out.mesh.normals[3].z, 0.0f, 1e-5f);
    }
    if (r.out.mesh.uvs.size() == 6) {
        for (int k = 0; k < 6; ++k)
            SR_ASSERT_NEAR(r.out.mesh.uvs[k].x, 0.25f, 1e-5f);   // uv 共享同一 vt
    }
}

// ---- Step 4: 材质分组（usemtl）与 group 边界 --------------------------------

SR_TEST(obj_groups_coalesce_only_adjacent_runs) {
    // A B A ⇒ **3** 个 group，不重排（spec §7.5 "最小的 batching"）。
    // 相邻的同材质 run（含冗余的 usemtl A 重复开关）合并成一段。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "v 1 1 0\n"
        "usemtl A\n"
        "f 1 2 3\n"
        "usemtl A\n"        // 冗余开关：仍是同一个 run
        "f 1 3 4\n"
        "usemtl B\n"
        "f 2 4 3\n"
        "usemtl A\n"
        "f 1 2 4\n",
        {namedMaterial("A", 1.0f), namedMaterial("B", 0.5f)});
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.groups.size(), 3);
    SR_ASSERT_EQ(r.out.materials.size(), 2);   // 入参原样拷贝，没有追加
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 12);
    if (r.out.groups.size() == 3) {
        SR_ASSERT_EQ(r.out.groups[0].indexOffset, 0);
        SR_ASSERT_EQ(r.out.groups[0].indexCount, 6);   // 两个相邻 A 面合并
        SR_ASSERT_EQ(r.out.groups[0].material, 0);
        SR_ASSERT_EQ(r.out.groups[1].indexOffset, 6);
        SR_ASSERT_EQ(r.out.groups[1].indexCount, 3);
        SR_ASSERT_EQ(r.out.groups[1].material, 1);
        SR_ASSERT_EQ(r.out.groups[2].indexOffset, 9);
        SR_ASSERT_EQ(r.out.groups[2].indexCount, 3);
        SR_ASSERT_EQ(r.out.groups[2].material, 0);     // 回到 A：新 group，不与第 0 组合并
    }
    if (r.out.materials.size() == 2) {
        SR_ASSERT(r.out.materials[0].name == "A");
        SR_ASSERT(r.out.materials[1].name == "B");
    }
}

SR_TEST(obj_every_group_index_count_is_a_multiple_of_three) {
    // 三角/quad/五边形混着材质切换：每个 group 的 indexCount 都是 3 的倍数，
    // 区间首尾相接、并集恰好覆盖整个索引数组（单位与 drawRange 的 firstIndex/indexCount 相同）。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "v 0.5 1.5 0\n"
        "usemtl A\n"
        "f 1 2 3\n"          // 1 三角
        "f 1 2 3 4\n"        // quad ⇒ 2
        "usemtl B\n"
        "f 1 2 3 4 5\n"      // 五边形 ⇒ 3
        "f 3 4 5\n",         // 1 三角
        {namedMaterial("A", 1.0f), namedMaterial("B", 0.5f)});
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.groups.size(), 2);
    SR_ASSERT_EQ(r.stats.triangles, 7);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 21);
    uint32_t covered = 0;
    for (const sr::MeshGroup& g : r.out.groups) {
        SR_ASSERT_EQ(g.indexCount % 3, 0);
        SR_ASSERT_EQ(g.indexOffset, covered);   // 首尾相接，无缝隙无重叠
        covered += g.indexCount;
    }
    SR_ASSERT_EQ(covered, r.out.mesh.indices.size());
}

SR_TEST(obj_unknown_material_name_warns_and_uses_default) {
    // 收紧 3：usemtl 指向不存在的材质是**警告不是 D6 错误** —— 缺材质只损失 tint，
    // 不损失几何。default 只在末尾追加**一次**，两个不同的未知名复用同一个追加项
    // （M5.4 数 group、去重贴图，N 份重复默认材质会把两边都弄脏）。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"          // 第一条 usemtl 之前的面 ⇒ materials[0]
        "usemtl ghost\n"
        "f 1 2 3\n"
        "usemtl phantom\n"
        "f 1 2 3\n",
        {namedMaterial("A", 1.0f)});
    SR_ASSERT(r.ok);                              // 不是错误
    SR_ASSERT_EQ(r.warnings.size(), 2);           // 每个未知 usemtl 一条
    SR_ASSERT_EQ(r.out.materials.size(), 2);      // A + 一个追加的 default（不是 3）
    SR_ASSERT_EQ(r.out.groups.size(), 2);         // A 一组，两个未知名共用一组
    if (r.out.materials.size() == 2) {
        SR_ASSERT(r.out.materials[1].name == sr::defaultMaterial().name);
        SR_ASSERT_NEAR(r.out.materials[1].diffuse.r, 1.0f, 1e-5f);   // 默认纯白 tint
        SR_ASSERT_NEAR(r.out.materials[1].diffuse.g, 1.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.materials[1].diffuse.b, 1.0f, 1e-5f);
    }
    if (r.out.groups.size() == 2) {
        SR_ASSERT_EQ(r.out.groups[0].material, 0);
        SR_ASSERT_EQ(r.out.groups[1].material, 1);   // 两个未知名都指同一个追加项
        SR_ASSERT_EQ(r.out.groups[1].indexCount, 6);
    }
}

SR_TEST(obj_material_table_gets_default_when_no_mtl) {
    // Ruling 46 规则 (a)：入参空 ⇒ out.materials **恰好一个** defaultMaterial()。
    // 此时即使出现未知名 usemtl 也只警告、复用这一个条目，不再追加（"恰好一个"优先）。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"
        "usemtl whatever\n"
        "f 1 2 3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.materials.size(), 1);
    SR_ASSERT_EQ(r.out.groups.size(), 1);   // 两个面都指材质 0，相邻合并
    SR_ASSERT(!r.warnings.empty());         // 未知名仍然警告
    if (!r.out.materials.empty()) {
        const sr::Material& d = sr::defaultMaterial();
        SR_ASSERT(r.out.materials[0].name == d.name);
        SR_ASSERT_NEAR(r.out.materials[0].diffuse.r, d.diffuse.r, 1e-5f);
        SR_ASSERT_NEAR(r.out.materials[0].opacity, d.opacity, 1e-5f);
        SR_ASSERT(r.out.materials[0].diffuseMap.empty());
    }
    for (const sr::MeshGroup& g : r.out.groups) SR_ASSERT_EQ(g.material, 0);
}

SR_TEST(obj_empty_text_is_valid_and_yields_default_material) {
    // 空文件/纯注释合法（与 parseMtl 同一约定）：true + 空几何 + 一个默认材质，
    // 不是错误。包围盒保持空（BBox 的 empty 语义，fit 侧由 Ruling 49 兜底）。
    const ObjResult empty = parse("");
    SR_ASSERT(empty.ok);
    SR_ASSERT(empty.err.empty());
    SR_ASSERT(empty.out.mesh.positions.empty());
    SR_ASSERT(empty.out.mesh.indices.empty());
    SR_ASSERT(empty.out.groups.empty());
    SR_ASSERT_EQ(empty.out.materials.size(), 1);
    SR_ASSERT(empty.out.mesh.bounds.empty());

    const ObjResult comments = parse("# nothing here\n\n   \n");
    SR_ASSERT(comments.ok);
    SR_ASSERT(comments.out.mesh.indices.empty());
    SR_ASSERT_EQ(comments.out.materials.size(), 1);
}

// ---- Step 5: uv 越界统计 + 越界值原样保留 ----------------------------------

SR_TEST(obj_keeps_out_of_range_uv_values_and_counts_them) {
    // vt 1.5 -0.2 ⇒ 数值**照存**（翻转之后 = (1.5, 1.2)），只计数不 clamp：
    // clamp 会把"文件写错了"这件事藏起来，而 wrap 模式本来就是设计给越界 uv 用的。
    // 计数按 vt **声明行**：一行两个分量都越界也只记 1；未被引用的 vt 也计。
    // 边界值 0 与 1 算界内。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 1.5 -0.2\n"     // u、v 都越界 ⇒ 计 1
        "vt 0.25 0.25\n"    // 界内
        "vt 0 1\n"          // 边界 ⇒ 界内
        "vt 2 2\n"          // 越界且未被引用 ⇒ 仍计 1（声明处计数）
        "f 1/1 2/2 3/3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.uvOutOfRange, 2);
    SR_ASSERT_EQ(r.out.mesh.uvs.size(), 3);
    if (r.out.mesh.uvs.size() == 3) {
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].x, 1.5f, 1e-5f);    // 不 clamp
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].y, 1.2f, 1e-5f);    // 1 - (-0.2)，翻转后照存
        SR_ASSERT_NEAR(r.out.mesh.uvs[1].x, 0.25f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.uvs[1].y, 0.75f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.uvs[2].x, 0.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.uvs[2].y, 0.0f, 1e-5f);    // vt (0,1) ⇒ (0,0)
    }
}

// ---- Step 6: D9 的 v 翻转（锚 1 的内联版）----------------------------------

SR_TEST(obj_flips_vt_because_obj_v_zero_is_the_image_bottom) {
    // OBJ 的 vt 原点在图片**左下**，而全栈契约（mesh.cpp:49-52）是
    // "图像第 0 行 = 顶部、采样器 v=0 = 第 0 行、mesh 的 v=0 = 这一面的顶边"
    // ⇒ 那一次 v = 1 - v 由 OBJ 解析器在出口做（D9），不留进管线。
    // **夹具必须非对称**：vt 写 0.25 而不是 0.5 —— 0.5 在翻转下不变，是假测试。
    // 这条是锚 1 的内联版：spec §1 的"两个错误互相抵消"（obj 忘翻 + tga 忘翻 ⇒
    // 合成画面看着完全正确）要求每一次翻转各自对着非对称基准被钉住。
    // 变异①（删掉 1.0f - v）⇒ 0.25/1.0/0.0 原样出来，三条全红。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0.25 0.25\n"
        "vt 0.5 1.0\n"
        "vt 0.5 0.0\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.mesh.uvs.size(), 3);
    if (r.out.mesh.uvs.size() == 3) {
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].x, 0.25f, 1e-5f);   // u 不动
        SR_ASSERT_NEAR(r.out.mesh.uvs[0].y, 0.75f, 1e-5f);   // 0.25 → 0.75（非自映射）
        SR_ASSERT_NEAR(r.out.mesh.uvs[1].y, 0.0f, 1e-5f);    // 1.0 → 0.0
        SR_ASSERT_NEAR(r.out.mesh.uvs[2].y, 1.0f, 1e-5f);    // 0.0 → 1.0
    }
}

// ---- Step 7: 语法层的容错 ---------------------------------------------------

SR_TEST(obj_accepts_crlf_and_bare_cr) {
    // Windows 导出器写 CRLF，老 Mac 写裸 CR：行切分必须都吃（与 parseMtl 同一种容忍）。
    const ObjResult crlf = parse("v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n");
    SR_ASSERT(crlf.ok);
    SR_ASSERT_EQ(crlf.out.mesh.positions.size(), 3);
    SR_ASSERT_EQ(crlf.stats.triangles, 1);

    const ObjResult cr = parse("v 0 0 0\rv 1 0 0\rv 0 1 0\rf 1 2 3\r");
    SR_ASSERT(cr.ok);
    SR_ASSERT_EQ(cr.out.mesh.positions.size(), 3);
    SR_ASSERT_EQ(cr.stats.triangles, 1);
    if (cr.out.mesh.indices.size() == 3) {
        SR_ASSERT_EQ(cr.out.mesh.indices[0], 0);
        SR_ASSERT_EQ(cr.out.mesh.indices[1], 1);
        SR_ASSERT_EQ(cr.out.mesh.indices[2], 2);
    }
}

SR_TEST(obj_strips_a_utf8_bom) {
    // Blender/某些导出器会写 BOM：剥掉后第一行仍是 v，不是未知指令。
    const ObjResult r = parse("\xEF\xBB\xBFv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.unrecognized, 0);
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 3);
    SR_ASSERT_EQ(r.stats.triangles, 1);
}

SR_TEST(obj_accepts_tabs_and_repeated_spaces) {
    const ObjResult r = parse("v\t0  0\t0\n \tv 1\t0 0\nv 0 1 0\nf\t1  2\t3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 3);
    SR_ASSERT_EQ(r.stats.triangles, 1);
    if (r.out.mesh.positions.size() == 3) {
        SR_ASSERT_NEAR(r.out.mesh.positions[1].x, 1.0f, 1e-5f);
        SR_ASSERT_NEAR(r.out.mesh.positions[2].y, 1.0f, 1e-5f);
    }
}

SR_TEST(obj_ignores_unknown_directives_and_counts) {
    // s/vp/bevel/cset/未知指令 ⇒ D3：忽略 + 计数 + 警告，不是错误（A4：不许静默忽略）。
    // o/g/mtllib 是**已知**指令：o/g 的名字不切分几何（分组只按材质），mtllib 归
    // scanObjMtllib（parseObj 不做 IO）—— 三者不计进 unrecognized。
    const ObjResult r = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "s 1\n"
        "vp 0.5 0.5\n"
        "bevel on\n"
        "cset bspline\n"
        "g top\n"
        "o cube\n"
        "mtllib foo.mtl\n"
        "f 1 2 3\n");
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.unrecognized, 4);   // 只有 s/vp/bevel/cset
    SR_ASSERT_EQ(r.warnings.size(), 4);
    SR_ASSERT_EQ(r.stats.triangles, 1);
    SR_ASSERT_EQ(r.out.groups.size(), 1);    // o/g 不切分几何
}

SR_TEST(obj_reports_the_line_number_of_a_bad_number) {
    // 已知指令的坏参数是 D6 错误：err 必须指名到行 + 肇事 token（错误消息的质量就是可用性）。
    const ObjResult r = parse(
        "# comment\n"
        "v 0 0 0\n"
        "v x y z\n");
    SR_ASSERT_FALSE(r.ok);
    SR_ASSERT(r.out.mesh.positions.empty());
    SR_ASSERT(!r.err.empty());
    SR_ASSERT(r.err.find("第 3 行") != std::string::npos);
    SR_ASSERT(r.err.find("x") != std::string::npos);

    // f 角里的坏索引同样带行号与 token。
    const ObjResult badIdx = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 z\n");
    SR_ASSERT_FALSE(badIdx.ok);
    SR_ASSERT(badIdx.err.find("第 4 行") != std::string::npos);
    SR_ASSERT(badIdx.err.find("z") != std::string::npos);
}

SR_TEST(obj_negative_vt_vn_indices_resolve_against_their_own_table_lengths) {
    // OBJ 经典错法：把 v/vt/vn 三个负索引**都**拿 v 的表长去解。判别夹具让两表长度不同，
    // 使"按自己的表长解"与"按 v 的表长解"给出**不同结果**。每条注释写明两种预期。
    // 这里刻意选"错解仍落在 vt/vn 表内"的长度组合 ⇒ 变异下得到的是**断言级红**而不是
    // 越界读引发的进程 abort（MSVC Debug STL 会对 vector 越界下标直接 abort）；
    // 落在 (len(vt), len(v)] 间隙、错解必然越界读的夹具放在下一条 rejects 用例里。

    // 负 vt，len(v)=4 > len(vt)=2，"f 1/-3 2 3"：
    //   正确：-3 按 vt 自己的长度 2 解 ⇒ -3+2 = -1 ⇒ 越界**拒绝**；
    //   错用 v 的长度 4：-3+4 = 1 ⇒ vtTable[1] 合法（表长 2）⇒ 错误地**接受**。
    //   两种预期 false vs true ⇒ 本条抓住该错法，且错解不越界 ⇒ 断言级红。
    const ObjResult vtNeg = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
        "vt 0 0\nvt 1 1\n"
        "f 1/-3 2 3\n");
    SR_ASSERT_FALSE(vtNeg.ok);
    SR_ASSERT(vtNeg.out.mesh.indices.empty());
    SR_ASSERT(vtNeg.err.find("第 7 行") != std::string::npos);   // 4 个 v + 2 个 vt ⇒ f 在第 7 行

    // 负 vn 同构：len(v)=4 > len(vn)=2，"f 1//-3 2 3" ⇒ 正确拒绝（-3+2=-1），错用 v 长接受。
    const ObjResult vnNeg = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
        "vn 0 0 1\nvn 1 0 0\n"
        "f 1//-3 2 3\n");
    SR_ASSERT_FALSE(vnNeg.ok);
    SR_ASSERT(vnNeg.out.mesh.indices.empty());

    // 反方向（错界**过小**会误拒）：len(v)=3 < len(vt)=5，"f 1/5 2 3"：
    //   正确：vt5 存在（idx 4 < 5）⇒ **接受**；错用 v 的长度 3：idx 4 ≥ 3 ⇒ 错误地**拒绝**。
    //   这条与上一条一起把"vt 的界恰好是 vtTable.size()"两个方向都钉死。
    const ObjResult vtBig = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0.75 0.25\n"
        "f 1/5 2 3\n");
    SR_ASSERT(vtBig.ok);
    if (vtBig.out.mesh.uvs.size() == 3) {
        SR_ASSERT_NEAR(vtBig.out.mesh.uvs[0].x, 0.75f, 1e-5f);   // vt5 翻转后 (0.75, 0.75)
        SR_ASSERT_NEAR(vtBig.out.mesh.uvs[0].y, 0.75f, 1e-5f);
    }
    // vn 反方向：len(v)=3 < len(vn)=5，"f 1//5 2 3" ⇒ 正确接受，法线取 vn5=(0,1,0)。
    const ObjResult vnBig = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vn 0 0 1\nvn 1 0 0\nvn 0 0 -1\nvn -1 0 0\nvn 0 1 0\n"
        "f 1//5 2 3\n");
    SR_ASSERT(vnBig.ok);
    if (vnBig.out.mesh.normals.size() == 3) {
        SR_ASSERT_NEAR(vnBig.out.mesh.normals[0].y, 1.0f, 1e-5f);   // 角 1 保留声明的 vn5
        SR_ASSERT_NEAR(vnBig.out.mesh.normals[0].x, 0.0f, 1e-5f);
    }

    // 正控制（负索引在自身表内合法，必须解对、不能过度拒绝）：len(v)=3 > len(vt)=2，
    // "f 1/-2 2 3"：正确 -2+2=0 ⇒ vt1=(0,0) ⇒ uv (0,1)；错用 v 长 -2+3=1 ⇒ vt2=(1,1) ⇒ uv (1,0)。
    // 两种结果都合法但**值不同** ⇒ 用 uv 值判别（错解不越界，断言级红）。
    const ObjResult vtCtl = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 1\n"
        "f 1/-2 2 3\n");
    SR_ASSERT(vtCtl.ok);
    if (vtCtl.out.mesh.uvs.size() == 3) {
        SR_ASSERT_NEAR(vtCtl.out.mesh.uvs[0].x, 0.0f, 1e-5f);   // 必须是 vt1 不是 vt2
        SR_ASSERT_NEAR(vtCtl.out.mesh.uvs[0].y, 1.0f, 1e-5f);
    }
    // vn 正控制：len(v)=3 > len(vn)=2，"f 1//-2 2 3" ⇒ 正确取 vn1=(0,0,1)，错取 vn2=(1,0,0)。
    const ObjResult vnCtl = parse(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vn 0 0 1\nvn 1 0 0\n"
        "f 1//-2 2 3\n");
    SR_ASSERT(vnCtl.ok);
    if (vnCtl.out.mesh.normals.size() == 3) {
        SR_ASSERT_NEAR(vnCtl.out.mesh.normals[0].z, 1.0f, 1e-5f);   // vn1
        SR_ASSERT_NEAR(vnCtl.out.mesh.normals[0].x, 0.0f, 1e-5f);
    }
}

SR_TEST(obj_rejects_out_of_range_index_with_a_message) {
    // **这条最关键，是内存安全守卫不是格式洁癖**：render_backend.cpp:115 的
    // `in.position = mesh.positions[idx]` **没有**边界保护；normals/uvs/colors 各自有
    // 兜底（116-118 行），唯独 position 是直接下标。放一个越界索引进 Mesh，渲染器就会
    // 越界读 —— 所以 D6 必须 outright 拒绝，谁也不许把这里"放宽"成 clamp。
    // 变异⑤（越界改成 clamp 到合法范围）会让下面所有拒绝断言红。
    const ObjResult vOver = parse(
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 4\n");        // 只有 3 个 v，索引 4 越界
    SR_ASSERT_FALSE(vOver.ok);
    SR_ASSERT(vOver.out.mesh.positions.empty());
    SR_ASSERT(vOver.out.mesh.indices.empty());
    SR_ASSERT(vOver.err.find("第 4 行") != std::string::npos);
    SR_ASSERT(vOver.err.find("4") != std::string::npos);

    // vt / vn 越界同样拒绝（它们也会变成顶点属性下标）。
    const ObjResult vtOver = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/9 2 3\n");
    SR_ASSERT_FALSE(vtOver.ok);
    SR_ASSERT(vtOver.out.mesh.indices.empty());
    SR_ASSERT(vtOver.err.find("第 5 行") != std::string::npos);

    const ObjResult vnOver = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//9 2 3\n");
    SR_ASSERT_FALSE(vnOver.ok);
    SR_ASSERT(vnOver.out.mesh.indices.empty());
    SR_ASSERT(vnOver.err.find("第 5 行") != std::string::npos);

    // 负索引越过开头（-4 在只有 3 个 v 时解析成 -1）与零索引都拒绝。
    const ObjResult negOver = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nf -4 2 3\n");
    SR_ASSERT_FALSE(negOver.ok);
    SR_ASSERT(negOver.out.mesh.indices.empty());

    const ObjResult zero = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n");
    SR_ASSERT_FALSE(zero.ok);
    SR_ASSERT(zero.out.mesh.indices.empty());

    // 极端负索引（INT64_MIN）：必须拒绝，且 count + raw 不得符号溢出（UB）。
    const ObjResult extreme = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nf -9223372036854775808 1 2\n");
    SR_ASSERT_FALSE(extreme.ok);
    SR_ASSERT(extreme.out.mesh.indices.empty());

    // 差集夹具（评审 Important 1）：索引落在 (len(vt), len(v)] 间隙 —— 对 v 合法、对 vt 越界。
    // len(v)=3、len(vt)=1，"f 1/2 2 3"：正确 ⇒ vt 索引 2 越界拒绝；若错用 positions.size()=3
    // 校验 ⇒ 索引 2 "合法" ⇒ vtTable[1] 越界读（MSVC Debug STL 会就此 abort —— 那正是内存安全
    // 缺陷本身）。断言级的双向判别由上一条 negative_..._own_table_lengths 用例给出（错解不越界）。
    const ObjResult vtGap = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/2 2 3\n");
    SR_ASSERT_FALSE(vtGap.ok);
    SR_ASSERT(vtGap.out.mesh.indices.empty());
    SR_ASSERT(vtGap.err.find("第 5 行") != std::string::npos);
    SR_ASSERT(vtGap.err.find("vt") != std::string::npos);

    // vn 侧同构：len(v)=3、len(vn)=1，"f 1//2 2 3" ⇒ 索引 2 对 v 合法、对 vn 越界。
    const ObjResult vnGap = parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//2 2 3\n");
    SR_ASSERT_FALSE(vnGap.ok);
    SR_ASSERT(vnGap.out.mesh.indices.empty());
    SR_ASSERT(vnGap.err.find("第 5 行") != std::string::npos);
    SR_ASSERT(vnGap.err.find("vn") != std::string::npos);
}

// ---- scanObjMtllib（M5.4 消费它决定"该读哪个 .mtl"）------------------------

SR_TEST(obj_scan_mtllib_collects_names_without_parsing_geometry) {
    // 只扫 mtllib 名字，不解析几何：混进一条会让 parseObj 报 D6 错误的 "f 1 2"，
    // scan 必须不受影响。一行多个名字 ⇒ 每个 token 一个文件名。BOM/CRLF/注释同样容忍。
    std::vector<std::string> names;
    const bool ok = sr::scanObjMtllib(
        "\xEF\xBB\xBF# header\r\n"
        "mtllib a.mtl b.mtl\r\n"
        "v 0 0 0\n"
        "f 1 2\n"               // parseObj 会报错，scan 不管几何
        "mtllib c.mtl\n",
        names);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(names.size(), 3);
    if (names.size() == 3) {
        SR_ASSERT(names[0] == "a.mtl");
        SR_ASSERT(names[1] == "b.mtl");
        SR_ASSERT(names[2] == "c.mtl");
    }

    // 空文本 ⇒ true + 空列表；out 先清空（不保留调用方残留）。
    std::vector<std::string> stale{"old"};
    SR_ASSERT(sr::scanObjMtllib("", stale));
    SR_ASSERT(stale.empty());
}

// ---- Step 8: 七项"病理标本"合成一份完整文本 --------------------------------

SR_TEST(obj_parses_a_real_world_kitchen_sink_file) {
    // 一份内联 OBJ 同时含七项病理标本（①..⑥ 在好文本里，⑦ 只出现在坏变体里）：
    //   ① 跨面共享的 (v,vt,vn) 三元组  ② 一个五边形  ③ 一个缺 vn 的面
    //   ④ 负索引  ⑤ 一条越界 vt  ⑥ usemtl 指向不存在的材质  ⑦ 尾部 f 1 2（应报错）
    //
    // ---- 期望值手算（先写在注释里，不是跑一遍抄回来）----
    // 行号:  1 #注释  2 mtllib ghost.mtl  3-7 五个 v  8-12 五个 vt  13 一个 vn
    //        14 usemtl red  15 quad  16 tri  17 usemtl ghost  18 五边形(负索引)  19 缺vn面
    // 声明表：posTable 5 项、vtTable 5 项、vnTable 1 项。
    // 行15 quad（red=材质0）：角 (1,1,1)(2,2,1)(3,3,1)(4,4,1)，vn 齐全 ⇒ 走去重，
    //   四键全新 ⇒ 顶点 0,1,2,3；扇形 (0,1,2)(0,2,3) ⇒ indices[0..5]。
    //   faceCorners=4，triangles=2（全进 trianglesFromQuads），group0{offset0,count6,mat0}。
    // 行16 tri（red）：(1,1,1)→顶点0、(3,3,1)→顶点2（①共享，复用）、(5,5,1)→新顶点4。
    //   indices[6..8]={0,2,4}。faceCorners=7，triangles=3，group0.count=9。
    // 行17 usemtl ghost：不在 {red} 里 ⇒ 警告1条 + 末尾追加一次 default ⇒
    //   materials={red,default}（size 2），currentMat=1。
    // 行18 五边形（ghost）：-5/-4/-3/-2/-1 ⇒ v1..v5（④，negativeIndices+=5），
    //   vt/vn 全正 ⇒ 键 (1,1,1)(2,2,1)(3,3,1)(4,4,1)(5,5,1) **全部已存在** ⇒ 零新顶点。
    //   扇形 (0,1,2)(0,2,3)(0,3,4) ⇒ indices[9..17]。faceCorners=12，triangles=6
    //   （+3 全进 trianglesFromNGons），新 group1{offset9,count9,mat1}。
    // 行19 f 1 2 3（ghost）：无 vt 无 vn ⇒ ③整面 unroll：新顶点 5,6,7，
    //   法线 = normalize(cross(v2-v1, v3-v1)) = normalize(cross((1,0,0),(1,1,0))) = (0,0,1)，
    //   uv 全 (0,0)（缺 vt 不翻转）。indices[18..20]={5,6,7}。faceCorners=15，
    //   triangles=7，facesWithoutNormals=1，facesUnrolled=1，group1.count=12。
    // ⑤ 越界 vt 只有行12 (0.5,1.5)：v=1.5>1 ⇒ uvOutOfRange=1（其余四条含边界 0/1 都界内）。
    // unrecognized=0（mtllib 已知、# 是注释）；warnings 恰好 1（ghost）。
    // 汇总：triangles=7, faceCorners=15, uniqueVertices=8（5共享+3独占，①<⑮ 且 ③膨胀）,
    //   trianglesFromQuads=2, trianglesFromNGons=3, facesWithoutNormals=1, facesUnrolled=1,
    //   uvOutOfRange=1, negativeIndices=5, unrecognized=0；
    //   indices 21 个、positions/normals/uvs 各 8 个、colors 空；
    //   groups=2：{0,9,mat0},{9,12,mat1}；materials=2；bounds min(0,0,0) max(1,1.5,0)。
    const std::string_view good =
        "# kitchen sink\n"                          // 1
        "mtllib ghost.mtl\n"                        // 2
        "v 0 0 0\n"                                 // 3
        "v 1 0 0\n"                                 // 4
        "v 1 1 0\n"                                 // 5
        "v 0 1 0\n"                                 // 6
        "v 0.5 1.5 0\n"                             // 7
        "vt 0 0\n"                                  // 8
        "vt 1 0\n"                                  // 9
        "vt 1 1\n"                                  // 10
        "vt 0 1\n"                                  // 11
        "vt 0.5 1.5\n"                              // 12  ⑤ 越界
        "vn 0 0 1\n"                                // 13
        "usemtl red\n"                              // 14
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n"               // 15  quad
        "f 1/1/1 3/3/1 5/5/1\n"                     // 16  ① 共享三元组
        "usemtl ghost\n"                            // 17  ⑥ 未知材质
        "f -5/1/1 -4/2/1 -3/3/1 -2/4/1 -1/5/1\n"    // 18  ②五边形 ④负索引
        "f 1 2 3\n";                                // 19  ③ 缺 vn
    const ObjResult r = parse(good, {namedMaterial("red", 1.0f)});
    SR_ASSERT(r.ok);
    SR_ASSERT_EQ(r.stats.triangles, 7);
    SR_ASSERT_EQ(r.stats.faceCorners, 15);
    SR_ASSERT_EQ(r.stats.uniqueVertices, 8);
    SR_ASSERT(r.stats.uniqueVertices < r.stats.faceCorners);   // ① 去重真的合并了
    SR_ASSERT_EQ(r.stats.trianglesFromQuads, 2);
    SR_ASSERT_EQ(r.stats.trianglesFromNGons, 3);
    SR_ASSERT_EQ(r.stats.facesWithoutNormals, 1);
    SR_ASSERT_EQ(r.stats.facesUnrolled, 1);
    SR_ASSERT_EQ(r.stats.uvOutOfRange, 1);
    SR_ASSERT_EQ(r.stats.negativeIndices, 5);
    SR_ASSERT_EQ(r.stats.unrecognized, 0);
    SR_ASSERT_EQ(r.warnings.size(), 1);
    SR_ASSERT_EQ(r.out.materials.size(), 2);
    SR_ASSERT_EQ(r.out.groups.size(), 2);
    SR_ASSERT_EQ(r.out.mesh.indices.size(), 21);
    SR_ASSERT_EQ(r.out.mesh.positions.size(), 8);
    SR_ASSERT_EQ(r.out.mesh.normals.size(), 8);
    SR_ASSERT_EQ(r.out.mesh.uvs.size(), 8);
    SR_ASSERT(r.out.mesh.colors.empty());
    SR_ASSERT_EQ(r.out.mesh.triangleCount(), 7);
    if (r.out.groups.size() == 2) {
        SR_ASSERT_EQ(r.out.groups[0].indexOffset, 0);
        SR_ASSERT_EQ(r.out.groups[0].indexCount, 9);
        SR_ASSERT_EQ(r.out.groups[0].material, 0);
        SR_ASSERT_EQ(r.out.groups[1].indexOffset, 9);
        SR_ASSERT_EQ(r.out.groups[1].indexCount, 12);
        SR_ASSERT_EQ(r.out.groups[1].material, 1);
    }
    if (r.out.materials.size() == 2) {
        SR_ASSERT(r.out.materials[0].name == "red");
        SR_ASSERT_NEAR(r.out.materials[1].diffuse.r, 1.0f, 1e-5f);   // 追加的 default 是白 tint
    }
    if (r.out.mesh.indices.size() == 21) {
        const uint32_t expected[21] = {0, 1, 2, 0, 2, 3,       // quad 扇形
                                       0, 2, 4,               // 共享 tri
                                       0, 1, 2, 0, 2, 3, 0, 3, 4,   // 五边形扇形
                                       5, 6, 7};              // unroll 独占
        for (int k = 0; k < 21; ++k) SR_ASSERT_EQ(r.out.mesh.indices[k], expected[k]);
    }
    if (r.out.mesh.normals.size() == 8) {
        SR_ASSERT_NEAR(r.out.mesh.normals[5].z, 1.0f, 1e-5f);   // unroll 的面法线
        SR_ASSERT_NEAR(r.out.mesh.normals[5].x, 0.0f, 1e-5f);
    }
    if (r.out.mesh.uvs.size() == 8) {
        SR_ASSERT_NEAR(r.out.mesh.uvs[5].x, 0.0f, 1e-5f);   // 缺 vt ⇒ (0,0)
        SR_ASSERT_NEAR(r.out.mesh.uvs[5].y, 0.0f, 1e-5f);
    }
    SR_ASSERT_NEAR(r.out.mesh.bounds.min.x, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(r.out.mesh.bounds.max.y, 1.5f, 1e-5f);   // v5 的 y 进了包围盒
    SR_ASSERT_NEAR(r.out.mesh.bounds.max.z, 0.0f, 1e-5f);

    // scan 与 parse 的分工：同一份文本，scan 只拿 mtllib 名字，不管几何。
    std::vector<std::string> libs;
    SR_ASSERT(sr::scanObjMtllib(good, libs));
    SR_ASSERT_EQ(libs.size(), 1);
    if (!libs.empty()) SR_ASSERT(libs[0] == "ghost.mtl");

    // ⑦ 变体：尾部加 "f 1 2"（第 20 行）⇒ D6 全有或全无：前 19 行全对也整体失败，
    // out 清空（半解析的模型比没有更糟，A1），err 带行号。
    const std::string bad = std::string(good) + "f 1 2\n";
    const ObjResult b = parse(bad, {namedMaterial("red", 1.0f)});
    SR_ASSERT_FALSE(b.ok);
    SR_ASSERT(b.out.mesh.positions.empty());
    SR_ASSERT(b.out.mesh.indices.empty());
    SR_ASSERT(b.out.groups.empty());
    SR_ASSERT(b.out.materials.empty());
    SR_ASSERT(b.err.find("第 20 行") != std::string::npos);
}
