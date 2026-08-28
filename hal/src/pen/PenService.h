#pragma once

#include "GaokunPen.h"

#include <cstdint>

// PenService.dll 的封装。该 DLL 是 x64，所以本组件必须编译为 ARM64EC。
//
// 调用模型与 KeyboardService.dll 完全同构：ProcPipeMsg 与 GetInterruptPipeMsg 各是一个
// 阻塞消息循环，需要各自的线程；命令由 CommandSendXxx 发出，结果经事先注册的回调返回；
// 设备打开之前发命令会访问违例而不是超时，因此必须先等消息循环线程进入阻塞态。
// 那些细节见 src/keyboard/KeyboardService.cpp 的注释，此处不再重复。
namespace Gaokun::Pen {

class Service {
public:
    Service() noexcept = default;
    ~Service() noexcept;

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    // 加载 DLL、起消息循环线程、注册全部回调，并等待设备就绪。
    [[nodiscard]] bool Start() noexcept;

    // 发出全部查询命令。结果异步经回调回来，调用方随后读快照即可。
    void RequestRefresh() noexcept;

    // 当前状态的一份拷贝。
    [[nodiscard]] Snapshot GetSnapshot() const noexcept;

    // 取一条离散事件，队列为空时返回 false。
    [[nodiscard]] bool PopEvent(Event &out) noexcept;

    // 设置侧键功能。原厂同样不等回应，写入是否生效要靠随后的 KeyFuncChanged 事件确认。
    void SetKeyFunc(int32_t func) noexcept;

private:
    void *m_module = nullptr;
    void *m_procThread = nullptr;
    void *m_interruptThread = nullptr;
};

} // namespace Gaokun::Pen
