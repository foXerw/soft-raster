#pragma once
// M6.1：光照的数据边界 —— LightRig（每帧一份）+ MaterialShade（逐组一份，drawModel 栈上复用）。
// 两个阶段（VS/FS）经 uniforms 里的**指针**共享同一份对象（D15 单一真源）；
// 用指针而不是拷贝/std::function：C5 逐像素不分配、不虚派发。
// 本文件只放数据与解码帮手，**不放光照数学**（那在 shading.h，M6.2）。
#include "math/vec.h"
#include "render/color.h"

namespace sr {

// 两盏灯（设计 §6.1 形态 C）：一盏方向光 + 一盏跟随相机的点光。
// 颜色字段一律**已解码到线性**（入口是下面的 decodeLightColor）；
// "关掉一盏" = intensity 走到 0（D24 的加法步进保证可达），不用 bool。
struct LightRig {
    Vec3 keyDirWorld{};         // 方向光"照向"的方向（单位向量）
    LinearColor keyColor{};     // 线性
    float keyIntensity = 1.0f;
    Vec3 pointPosWorld{};       // 默认 = eyeWorld（M6.6 组装）
    LinearColor pointColor{};   // 线性
    float pointIntensity = 1.0f;
    bool attenuate = false;
    float attenuateRadius = 1.0f;
    LinearColor ambient{};      // 环境光颜色（线性），乘 Ka（D9）
    Vec3 eyeWorld{};            // 视点（高光需要视点相关项才看得见，§6.1）
};

// D15："着色需要的四个数"。**为什么不直接传 const Material***：
// Material（loader/mtl.h）还带着 name/diffuseMap/opacity —— 着色层不该读的东西
// （A1：抽象止步于阶段边界）。Material → MaterialShade 的抄写发生在 drawModel
// （loader 侧，它同时认识两个类型）；render 侧只认识这四个线性量。
struct MaterialShade {
    LinearColor diffuse{1.0f, 1.0f, 1.0f};
    LinearColor ambient{0.0f, 0.0f, 0.0f};
    LinearColor specular{0.0f, 0.0f, 0.0f};
    // 原样存（含 0 = 无高光）。夹取与 Ns==0 的判断都在 shading.h，
    // 一处规则、两个消费者（M6.2/M6.4）。
    float shininess = 0.0f;
};

// 程序几何的 lit 默认（D15）。**为什么不能沿用 Material 的默认**：
// Material 的 Ks=0/Ka=0/Ns=0（mtl.h:24-27）对"没写材质文件的 OBJ"是对的（尊重文件），
// 对"我要演示高光"的程序球是错的 —— lit golden 会烘进一张零高光的球，
// G-M6.2 与肉眼门一起静默失败（设计 §14 第 1 条）。
// 函数内 static（defaultMaterial() 的先例，mtl.cpp:52-56）：地址进程生命周期内稳定，
// 可反复取地址挂进 uniforms。
inline const MaterialShade& litDefaultMaterialShade() {
    static const MaterialShade kLitDefault{
        LinearColor{1.0f, 1.0f, 1.0f},     // diffuse
        LinearColor{0.02f, 0.02f, 0.02f},  // ambient
        LinearColor{0.35f, 0.35f, 0.35f},  // specular：非零，高光看得见（D19 的峰值不饱和由 M6.6/M6.8 调强度保证）
        64.0f};                            // shininess
    return kLitDefault;
}

// 命令行/参数给的光色是 display-referred 的 sRGB 浮点（与 MTL 的 Kd 同源）。
// 进线性世界必须经**同一个** srgbToLinear(float)（color.h，D5 / Review Focus 5）：
// 解码一次、也只解码一次。M6.6 的 buildLightRig 调它；强度是线性乘子，不走这里。
inline LinearColor decodeLightColor(float r, float g, float b) {
    return srgbToLinear(LinearColor{r, g, b});
}

}  // namespace sr
