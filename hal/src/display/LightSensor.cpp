#include "LightSensor.h"

#include <windows.h>

// initguid.h 必须在传感器头之前。SENSOR_CATEGORY_LIGHT 与 SENSOR_DATA_TYPE_* 是
// DEFINE_GUID / DEFINE_PROPERTYKEY，默认只声明不定义，定义在 SDK 的 sensorsapi.lib 里。
// 就地定义它们，链接时便不必再多一个库。接口 IID 走 __uuidof，MIDL 已经把 GUID 写进
// 声明，同样不需要 uuid 库。
#include <initguid.h>

#include <sensors.h>
#include <sensorsapi.h>
#include <portabledevicetypes.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <new>

namespace Gaokun::Display {

namespace {

// 最小的事件接收器。注册它是为了让传感器开始采样——没有订阅者时传感器不工作，GetData
// 永远读到空报告。回调本身什么都不做：数据由帧循环按自己的节奏轮询，这里要的只是那个
// 「有人在听」的状态。
//
// 不用引用计数控制生命周期：这个对象与 LightSensor 同生共死，AddRef/Release 只需保证
// 传感器持有期间不被销毁，所以计数从 1 起、由 LightSensor 在析构时撤销订阅后释放。
class SensorEventSink final : public ISensorEvents {
public:
    // ── IUnknown ──
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISensorEvents)) {
            *out = static_cast<ISensorEvents *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG left = InterlockedDecrement(&m_refs);
        if (left == 0) delete this;
        return static_cast<ULONG>(left);
    }

    // ── ISensorEvents ──
    // 值只能在这里取。这个传感器是变化触发上报，光照不动就不发新报告，而 ISensor::GetData
    // 给的是「本客户端收到的最后一份报告」——没收到过就是空的，六个字段一律 E_FAIL。
    // 轮询 GetData 因此永远读不到东西，厂商的 SensorLightEventHandle::OnDataUpdated 也是
    // 在回调里读。
    HRESULT STDMETHODCALLTYPE OnStateChanged(ISensor *, SensorState) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataUpdated(ISensor *, ISensorDataReport *report) override {
        if (!report) return S_OK;
        double v = 0.0;
        if (ReadField(report, SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, v)) m_lux.store(v);
        if (ReadField(report, SENSOR_DATA_TYPE_LIGHT_TEMPERATURE_KELVIN, v)) m_cct.store(v);
        m_updates.fetch_add(1);
        return S_OK;
    }

    [[nodiscard]] uint32_t Updates() const noexcept { return m_updates.load(); }
    [[nodiscard]] double Lux() const noexcept { return m_lux.load(); }
    [[nodiscard]] double Cct() const noexcept { return m_cct.load(); }
    HRESULT STDMETHODCALLTYPE OnEvent(ISensor *, REFGUID, IPortableDeviceValues *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLeave(REFSENSOR_ID) override { return S_OK; }

private:
    // 回调在传感器自己的线程上来，取值的帧循环在另一条线程，所以计数和两个读数都用 atomic。
    static bool ReadField(ISensorDataReport *report, REFPROPERTYKEY key, double &out) noexcept;

    LONG m_refs = 1;
    std::atomic<uint32_t> m_updates{0};
    std::atomic<double> m_lux{0.0};
    std::atomic<double> m_cct{0.0};
};

[[nodiscard]] bool PropVariantToDoubleValue(const PROPVARIANT &pv, double &out) noexcept {
    switch (pv.vt) {
    case VT_R4: out = pv.fltVal; return true;
    case VT_R8: out = pv.dblVal; return true;
    case VT_UI4: out = static_cast<double>(pv.ulVal); return true;
    case VT_I4: out = static_cast<double>(pv.lVal); return true;
    case VT_UI2: out = static_cast<double>(pv.uiVal); return true;
    default: return false;
    }
}

bool SensorEventSink::ReadField(ISensorDataReport *report, REFPROPERTYKEY key,
                                double &out) noexcept {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const bool ok = SUCCEEDED(report->GetSensorValue(key, &pv)) &&
                    PropVariantToDoubleValue(pv, out);
    PropVariantClear(&pv);
    return ok;
}

} // namespace

LightSensor::~LightSensor() noexcept {
    if (m_sensor) {
        // 先撤订阅再放接收器：传给 SetEventSink(nullptr) 之后传感器才不会再回调过来，
        // 顺序反了会在传感器还持有指针时把对象销毁掉。
        if (m_events) (void)static_cast<ISensor *>(m_sensor)->SetEventSink(nullptr);
        static_cast<ISensor *>(m_sensor)->Release();
    }
    if (m_events) static_cast<ISensorEvents *>(m_events)->Release();
}

bool LightSensor::Open(bool subscribe) noexcept {
    ISensorManager *manager = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(SensorManager), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(ISensorManager), reinterpret_cast<void **>(&manager));
    if (FAILED(hr)) {
        // 传感器访问被隐私设置关掉时这里就是 ERROR_ACCESS_DISABLED_BY_POLICY，不是
        // GetSensorsByCategory 才失败。
        m_error = (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY))
                      ? L"sensor access is disabled in Privacy settings"
                      : L"cannot create the sensor manager";
        return false;
    }

    ISensorCollection *collection = nullptr;
    hr = manager->GetSensorsByCategory(SENSOR_CATEGORY_LIGHT, &collection);
    manager->Release();
    if (FAILED(hr) || !collection) {
        m_error = L"no ambient light sensor on this machine";
        return false;
    }

    ULONG count = 0;
    (void)collection->GetCount(&count);
    for (ULONG i = 0; i < count; ++i) {
        ISensor *sensor = nullptr;
        if (FAILED(collection->GetAt(i, &sensor)) || !sensor) continue;

        // 挑第一个能报色温的。本机的传感器同时上报照度与色温，若只有照度，自然色彩没有
        // 输入，此时宁可明确报错，也不要拿照度去反推一个色温。
        VARIANT_BOOL supported = VARIANT_FALSE;
        if (SUCCEEDED(sensor->SupportsDataField(SENSOR_DATA_TYPE_LIGHT_TEMPERATURE_KELVIN,
                                                &supported)) &&
            supported == VARIANT_TRUE) {
            m_sensor = sensor;
            break;
        }
        sensor->Release();
    }
    collection->Release();

    if (!m_sensor) {
        m_error = L"the ambient light sensor does not report colour temperature";
        return false;
    }

    SensorState state = SENSOR_STATE_ERROR;
    if (SUCCEEDED(static_cast<ISensor *>(m_sensor)->GetState(&state))) {
        m_state = static_cast<int>(state);
        if (state == SENSOR_STATE_ACCESS_DENIED) {
            m_error = L"no permission to use the ambient light sensor";
            return false;
        }
    }

    // 订阅之后传感器才开始采样。失败不算致命：有别的进程正订阅着同一个传感器时，
    // 不订阅照样读得到，先前那次实测就是这种情形。
    auto *sink = subscribe ? new (std::nothrow) SensorEventSink() : nullptr;
    if (sink) {
        if (SUCCEEDED(static_cast<ISensor *>(m_sensor)->SetEventSink(sink))) {
            m_events = sink;
        } else {
            sink->Release();
        }
    }

    m_ready = true;
    return true;
}

bool LightSensor::ReadScalar(const void *key, double &out) noexcept {
    if (!m_ready || !m_sensor) return false;

    // 传感器要有人开始要数据之后才采样，第一次 GetData 往往什么都没有。守护进程是持续
    // 轮询的，跑起来之后每次都有值，所以这个坑只有一次性调用会踩到——`--natural-color on`
    // 当场就报「传感器没有数据」，而同一台机器上守护进程读得好好的。
    //
    // 等一个上报周期。本机实测第一次就能拿到，留出十次是给冷启动的余量；拿到就立刻返回，
    // 正常路径上不会真的等满。
    constexpr int kAttempts = 10;
    constexpr DWORD kIntervalMs = 100;

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        ISensorDataReport *report = nullptr;
        const HRESULT dataHr = static_cast<ISensor *>(m_sensor)->GetData(&report);
        if (SUCCEEDED(dataHr) && report) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            const HRESULT hr =
                report->GetSensorValue(*static_cast<const PROPERTYKEY *>(key), &pv);
            report->Release();
            // 记的是取字段这一步，不是 GetData。两者是不同的失败：报告拿到了却没有这个
            // 字段，说明传感器声称支持而实际不上报；先前只记 GetData 的结果，一直是 S_OK，
            // 把排查引到了「传感器没在采样」上。
            m_lastHr = hr;
            m_lastVt = pv.vt;
            if (SUCCEEDED(hr)) {
                const bool ok = PropVariantToDoubleValue(pv, out);
                PropVariantClear(&pv);
                if (ok) return true;
            } else {
                PropVariantClear(&pv);
            }
        } else {
            m_lastHr = dataHr;
        }
        if (attempt + 1 < kAttempts) Sleep(kIntervalMs);
    }

    m_error = L"the sensor returned no data";
    return false;
}

// 把报告里的字段逐个列出来。传感器声称支持某个字段、实际却给 VT_EMPTY 时，只有把整份
// 报告摊开才看得出它到底在报什么。
void LightSensor::DumpReport(void (*sink)(const wchar_t *)) noexcept {
    if (!m_ready || !m_sensor || !sink) return;

    auto *sensor = static_cast<ISensor *>(m_sensor);
    wchar_t line[256];

    // 订阅失败是静默的（Open 里失败只是不记 m_events），先把它摊开：读不到数据时，
    // 「没订阅上」和「订阅了但传感器不报」是两回事。
    sink(m_events ? L"event sink: subscribed" : L"event sink: NOT subscribed");

    // 逐秒打一次回调计数。传感器是变化触发上报，静止不动时一次回调也不会有——所以这段
    // 时间里要让照度真的变一变（遮住传感器再放开），否则计数停在 0 说明不了故障。
    if (m_events) {
        auto *ev = static_cast<SensorEventSink *>(m_events);
        sink(L"waiting 10s for OnDataUpdated — cover the sensor and uncover it now");
        for (int i = 1; i <= 10; ++i) {
            Sleep(1000);
            std::swprintf(line, std::size(line), L"  t=%2ds  updates=%u  lux=%.1f  cct=%.0f", i,
                          static_cast<unsigned>(ev->Updates()), ev->Lux(), ev->Cct());
            sink(line);
        }
    }

    // 先看它自己声明支持哪些字段。
    IPortableDeviceKeyCollection *fields = nullptr;
    if (SUCCEEDED(sensor->GetSupportedDataFields(&fields)) && fields) {
        DWORD count = 0;
        (void)fields->GetCount(&count);
        std::swprintf(line, std::size(line), L"supported fields: %lu",
                      static_cast<unsigned long>(count));
        sink(line);

        ISensorDataReport *report = nullptr;
        const HRESULT dataHr = sensor->GetData(&report);
        std::swprintf(line, std::size(line), L"GetData hr = 0x%08lX",
                      static_cast<unsigned long>(dataHr));
        sink(line);

        for (DWORD i = 0; i < count; ++i) {
            PROPERTYKEY key{};
            if (FAILED(fields->GetAt(i, &key))) continue;

            PROPVARIANT pv;
            PropVariantInit(&pv);
            HRESULT hr = E_FAIL;
            if (report) hr = report->GetSensorValue(key, &pv);

            double value = 0.0;
            const bool numeric = SUCCEEDED(hr) && PropVariantToDoubleValue(pv, value);
            std::swprintf(line, std::size(line),
                          L"  field %lu pid=%lu hr=0x%08lX vt=%u%ls",
                          static_cast<unsigned long>(i),
                          static_cast<unsigned long>(key.pid),
                          static_cast<unsigned long>(hr), static_cast<unsigned>(pv.vt),
                          numeric ? L"" : L"  (not numeric)");
            sink(line);
            if (numeric) {
                std::swprintf(line, std::size(line), L"    value = %.3f", value);
                sink(line);
            }
            PropVariantClear(&pv);
        }
        if (report) report->Release();
        fields->Release();
    } else {
        sink(L"GetSupportedDataFields failed");
    }
}

void DumpAllLightSensors(void (*sink)(const wchar_t *)) noexcept {
    if (!sink) return;

    wchar_t line[512];

    ISensorManager *manager = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(SensorManager), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ISensorManager),
                                reinterpret_cast<void **>(&manager)))) {
        sink(L"cannot create the sensor manager");
        return;
    }

    ISensorCollection *collection = nullptr;
    const HRESULT catHr = manager->GetSensorsByCategory(SENSOR_CATEGORY_LIGHT, &collection);
    manager->Release();
    if (FAILED(catHr) || !collection) {
        std::swprintf(line, std::size(line), L"GetSensorsByCategory hr = 0x%08lX",
                      static_cast<unsigned long>(catHr));
        sink(line);
        return;
    }

    ULONG count = 0;
    (void)collection->GetCount(&count);
    std::swprintf(line, std::size(line), L"light sensors in this category: %lu",
                  static_cast<unsigned long>(count));
    sink(line);

    for (ULONG i = 0; i < count; ++i) {
        ISensor *sensor = nullptr;
        if (FAILED(collection->GetAt(i, &sensor)) || !sensor) continue;

        BSTR name = nullptr;
        (void)sensor->GetFriendlyName(&name);
        SensorState state = SENSOR_STATE_ERROR;
        (void)sensor->GetState(&state);
        std::swprintf(line, std::size(line), L"[%lu] state=%d  name=%ls",
                      static_cast<unsigned long>(i), static_cast<int>(state),
                      name ? name : L"(none)");
        sink(line);
        if (name) SysFreeString(name);

        // 先看它当前的上报间隔，再按最小间隔设一次。WinRT 那条路正是设了 ReportInterval
        // 之后才读到值的，而这里从没验证过设置有没有真的成功——SetProperties 即使返回
        // S_OK，逐键的结果仍在 results 里，键没被接受时那才是失败。
        PROPVARIANT cur;
        PropVariantInit(&cur);
        const HRESULT curHr = sensor->GetProperty(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL, &cur);
        PROPVARIANT mn;
        PropVariantInit(&mn);
        const HRESULT mnHr = sensor->GetProperty(SENSOR_PROPERTY_MIN_REPORT_INTERVAL, &mn);
        std::swprintf(line, std::size(line),
                      L"     interval: cur hr=0x%08lX vt=%u val=%lu / min hr=0x%08lX vt=%u val=%lu",
                      static_cast<unsigned long>(curHr), static_cast<unsigned>(cur.vt),
                      static_cast<unsigned long>(cur.vt == VT_UI4 ? cur.ulVal : 0),
                      static_cast<unsigned long>(mnHr), static_cast<unsigned>(mn.vt),
                      static_cast<unsigned long>(mn.vt == VT_UI4 ? mn.ulVal : 0));
        sink(line);

        const ULONG wanted = (mn.vt == VT_UI4 && mn.ulVal > 0) ? mn.ulVal : 100;
        PropVariantClear(&cur);
        PropVariantClear(&mn);

        IPortableDeviceValues *props = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(PortableDeviceValues), nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&props)))) {
            (void)props->SetUnsignedIntegerValue(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL, wanted);
            IPortableDeviceValues *results = nullptr;
            const HRESULT setHr = sensor->SetProperties(props, &results);
            HRESULT keyHr = S_OK;
            if (results)
                (void)results->GetErrorValue(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL, &keyHr);
            std::swprintf(line, std::size(line),
                          L"     SetProperties(interval=%lu) hr=0x%08lX, per-key hr=0x%08lX",
                          static_cast<unsigned long>(wanted),
                          static_cast<unsigned long>(setHr), static_cast<unsigned long>(keyHr));
            sink(line);
            if (results) results->Release();
            props->Release();
        }

        Sleep(1500);

        ISensorDataReport *report = nullptr;
        const HRESULT dataHr = sensor->GetData(&report);

        IPortableDeviceKeyCollection *fields = nullptr;
        DWORD fieldCount = 0;
        if (SUCCEEDED(sensor->GetSupportedDataFields(&fields)) && fields) {
            (void)fields->GetCount(&fieldCount);
        }
        std::swprintf(line, std::size(line), L"     GetData hr=0x%08lX, fields=%lu",
                      static_cast<unsigned long>(dataHr),
                      static_cast<unsigned long>(fieldCount));
        sink(line);

        for (DWORD f = 0; f < fieldCount; ++f) {
            PROPERTYKEY key{};
            if (FAILED(fields->GetAt(f, &key))) continue;

            PROPVARIANT pv;
            PropVariantInit(&pv);
            HRESULT hr = E_FAIL;
            if (report) hr = report->GetSensorValue(key, &pv);

            double value = 0.0;
            const bool numeric = SUCCEEDED(hr) && PropVariantToDoubleValue(pv, value);
            // fmtid 一并打出来：pid 会在不同 fmtid 下重复，光看 pid 分不清是哪个字段。
            if (numeric) {
                std::swprintf(line, std::size(line),
                              L"     {%08lX-%04X-%04X} pid=%lu  = %.3f",
                              static_cast<unsigned long>(key.fmtid.Data1), key.fmtid.Data2,
                              key.fmtid.Data3, static_cast<unsigned long>(key.pid), value);
            } else {
                std::swprintf(line, std::size(line),
                              L"     {%08lX-%04X-%04X} pid=%lu  hr=0x%08lX vt=%u",
                              static_cast<unsigned long>(key.fmtid.Data1), key.fmtid.Data2,
                              key.fmtid.Data3, static_cast<unsigned long>(key.pid),
                              static_cast<unsigned long>(hr), static_cast<unsigned>(pv.vt));
            }
            sink(line);
            PropVariantClear(&pv);
        }

        if (fields) fields->Release();
        if (report) report->Release();
        sensor->Release();
    }
    collection->Release();
}

bool LightSensor::ReadCct(double &kelvin) noexcept {
    return ReadScalar(&SENSOR_DATA_TYPE_LIGHT_TEMPERATURE_KELVIN, kelvin);
}

bool LightSensor::ReadLux(double &lux) noexcept {
    return ReadScalar(&SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, lux);
}

} // namespace Gaokun::Display
