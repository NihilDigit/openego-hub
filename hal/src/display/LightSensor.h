#pragma once

// 环境光传感器。走 Windows 标准的 Sensor API
// （ISensorManager::GetSensorsByCategory(SENSOR_CATEGORY_LIGHT)），不碰任何厂商代码。
//
// 厂商 WmiUtil.dll 的 BiosWmi::GetEnviromentLight 在本机返回 -13（不支持），那条路走不通，
// 不必再试。
//
// TODO 本机读不出数据，这套代码目前跑不通，留着是为了换机器或驱动修好之后能直接用。
//
// tcs3701 在驱动初始化时上报一帧，之后不再采样：遮光无反应，冷启动无用，WinRT 与 COM
// 两条栈拿到的是同一份陈旧缓存（时间戳几十秒不变）。GetData 返回 S_OK 而六个字段一律
// E_FAIL，是因为报告里根本没有值。
//
// 逐条实测排除过的：订阅 ISensorEvents（有无两种情形输出逐字相同）、
// SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL（读写都成功，10 ms 与默认 250 ms 无差别）、
// 挑错传感器（这个类别下只有一个）、隐私权限（location 为 Allow，lfsvc 在跑，状态是
// READY 而非 ACCESS_DENIED）、厂商服务与厂商用户态进程（全部 Running 时照样读不到）。
// 问题在高通传感器驱动一侧，从这里够不着。诊断入口是 GaokunDisplay --sensor-probe。
//
// 「首次实测 10143K」不足为凭：那个色温相当于晴天蓝天，不像室内读数，多半也是初始化的
// 那一帧。当时看到屏幕颜色变化，证明的只是 PCC 下发通道通，与传感器无关。
//
// 厂商 WmiUtil.dll 的 BiosWmi::GetEnviromentLight 在本机返回 -13（不支持），那条路同样
// 走不通。原厂走的是订阅：OneControlApi 里有 RTTI .?AUISensorEvents@@，见
// docs/display-manage.md 4.3。
//
// 值只能在 OnDataUpdated 回调里取——GetData 给的是「本客户端收到的最后一份报告」，从没
// 收到过就是空的。轮询在任何情况下都读不到东西。
namespace Gaokun::Display {

class LightSensor {
public:
    LightSensor() noexcept = default;
    ~LightSensor() noexcept;

    LightSensor(const LightSensor &) = delete;
    LightSensor &operator=(const LightSensor &) = delete;

    // 调用方负责本线程的 CoInitializeEx。
    // subscribe 决定是否注册 ISensorEvents。留成参数是为了让 --sensor-probe 能在同一次运行里
    // 对照两种情形——订阅曾被当成「传感器开始采样」的前提，而那个前提正在被怀疑。
    [[nodiscard]] bool Open(bool subscribe = true) noexcept;

    // 传感器是否已就绪。未就绪多半是「设置 - 隐私 - 位置/传感器」把访问关掉了。
    [[nodiscard]] bool Ready() const noexcept { return m_ready; }

    // 读一次环境光色温，单位开尔文。传感器只上报照度、不上报色温时返回 false——本机的
    // 判定见实现处的注释。
    [[nodiscard]] bool ReadCct(double &kelvin) noexcept;

    // 读一次照度，单位 lux。自然色彩用它判断是否暗到该停止调节。
    [[nodiscard]] bool ReadLux(double &lux) noexcept;

    // 诊断：把报告里的字段逐个交给 sink。声称支持某字段却给空值时，只有摊开整份报告
    // 才看得出传感器实际在报什么。
    void DumpReport(void (*sink)(const wchar_t *)) noexcept;

    [[nodiscard]] const wchar_t *LastError() const noexcept { return m_error; }

    // 诊断用。传感器存在却读不出数据时，光看错误串分不清是它没准备好、还是这台机器上
    // 根本没有真实的光感元件。
    [[nodiscard]] int State() const noexcept { return m_state; }
    [[nodiscard]] long LastHresult() const noexcept { return m_lastHr; }
    // 最后一次读到的 PROPVARIANT 类型。字段存在但类型不在预期之列时，靠它才能看出来。
    [[nodiscard]] unsigned short LastVarType() const noexcept { return m_lastVt; }

private:
    [[nodiscard]] bool ReadScalar(const void *key, double &out) noexcept;

    void *m_sensor = nullptr; // ISensor*
    void *m_events = nullptr; // ISensorEvents*，注册后传感器才开始采样
    bool m_ready = false;
    int m_state = -1;    // SensorState，-1 表示没问到
    long m_lastHr = 0;         // 最后一次取字段的 HRESULT
    unsigned short m_lastVt = 0;  // 以及取到的 PROPVARIANT 类型
    const wchar_t *m_error = L"";
};

// 诊断：枚举 SENSOR_CATEGORY_LIGHT 下的每一个传感器，逐个摊开名称、状态、支持字段与
// 实际读数。LightSensor::Open 只挑「第一个声称支持色温的」，挑错了在外面看不出来。
void DumpAllLightSensors(void (*sink)(const wchar_t *)) noexcept;

} // namespace Gaokun::Display
