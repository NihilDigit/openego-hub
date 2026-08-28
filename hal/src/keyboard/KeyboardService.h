#pragma once

#include "GaokunKeyboard.h"

#include <cstdint>

// KeyboardService.dll 的封装。该 DLL 是 x64，所以本组件必须编译为 ARM64EC。
//
// 调用模型与 PenService.dll 完全同构：两个导出各是一个阻塞消息循环，需要各自的线程；
// 命令由 CommandSendXxx 发出，结果经事先注册的回调返回。两处必须照做而不能省的细节：
//
//   设备打开之前发命令会访问违例而不是超时，所以要先等消息循环线程停止消耗 CPU 周期；
//   StopProcPipeMsg / StopLoop / FreeLibrary 在循环线程仍在库内时调用同样会访问违例，
//   原厂工具解析了这些符号却从不使用，原因即在于此。
namespace Gaokun::Keyboard {

class Service {
public:
    Service() noexcept = default;
    ~Service() noexcept;

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    [[nodiscard]] bool Start() noexcept;

    // 发出全部查询命令，结果异步经回调回来。
    void RequestRefresh() noexcept;

    [[nodiscard]] Snapshot GetSnapshot() const noexcept;
    [[nodiscard]] bool PopEvent(Event &out) noexcept;

    // 分离后无线连接开关。原厂不等回应，写入是否生效要靠随后的快照或事件确认。
    void SetDetachSupport(bool enable) noexcept;

    // 仅供一次性的命令行用法：发出查询并等一个回应，超时返回 false。
    [[nodiscard]] bool QueryDetachSupport(bool &enabled) noexcept;

private:
    void *m_module = nullptr;
    void *m_procThread = nullptr;
    void *m_interruptThread = nullptr;
};

} // namespace Gaokun::Keyboard
