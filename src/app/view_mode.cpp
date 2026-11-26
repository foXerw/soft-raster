#include "app/view_mode.h"

#include "render/builtin_shaders.h"   // sr::debugNormalFS

namespace app {

// 路由表（D23）：视图 → "FS 选择 + 帧末 sink"。两种机制的分界见 view_mode.h / sink.h。
ViewPlan planView(ViewMode mode, sr::FragmentShaderFn defaultFS) {
    switch (mode) {
        case ViewMode::Normal:
            // 法线是普通 FS 视图：覆写 FS，sink 保持 Color（恒等）——
            // 再过后处理会把 (N+1)/2 的编码改写掉（D23）。
            return {sr::debugNormalFS, sr::DebugViewSink::View::Color, "NORMAL"};
        case ViewMode::Depth:
            // 深度/灰度天生是后处理：FS 不动（defaultFS 原样），帧末走对应 sink。
            return {defaultFS, sr::DebugViewSink::View::Depth, "DEPTH"};
        case ViewMode::Gray:
            return {defaultFS, sr::DebugViewSink::View::Gray, "GRAY"};
        case ViewMode::Color:
            break;
    }
    return {defaultFS, sr::DebugViewSink::View::Color, "COLOR"};
}

}  // namespace app
