#include "DiagnosticsWorkbench.h"
#include "ServiceProxy.h"
#include "DvrCsvExport.h"
#include "Logger.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace App {

namespace {
constexpr const char* kExportRootDir = "C:/ProgramData/OpenEGoHub/exports";
}

void DiagnosticsWorkbench::ExportCurrentFrameCsv(bool isAutoCapture) {
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_now = std::localtime(&time_now);

    std::string subFolder = isAutoCapture ? "auto" : "manual";
    std::filesystem::path dir(kExportRootDir);
    dir /= subFolder;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        m_lastCsvExportStatus = "Failed to create CSV export directory.";
        LOG_ERROR("App", __func__, "UI", "Failed to create directory: {}", dir.string());
        return;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    static uint32_t captureCounter = 0;

    std::ostringstream filename;
    filename << "heatmap_"
             << std::put_time(tm_now, "%Y%m%d_%H%M%S") << "_"
             << std::setfill('0') << std::setw(3) << ms.count() << "_"
             << captureCounter++
             << ".csv";

    const std::filesystem::path fullPath = dir / filename.str();
    const std::string captureMode = isAutoCapture
        ? (m_autoCaptureMode == 1 ? "PeakAppear" : "TouchDrop")
        : "Manual";

    const auto dynamicSchema = m_proxy ? m_proxy->GetCurrentDvrDynamicDebugSchema() : DvrDynamicDebugSchema{};
    const auto dynamicFrame = m_proxy ? m_proxy->GetCurrentDvrDynamicDebugFrame() : DvrDynamicDebugFrame{};
    const bool ok = WriteFrameCsvFile(fullPath,
                                      m_currentFrame,
                                      m_proxy ? &m_proxy->GetPipeline() : nullptr,
                                      true,
                                      true,
                                      true,
                                      captureMode,
                                      true,
                                      m_proxy ? m_proxy->GetPlaybackFormatVersion() : 0,
                                      "CurrentFrame",
                                      &dynamicSchema,
                                      &dynamicFrame);
    if (!ok) {
        m_lastCsvExportStatus = "Failed to write current frame CSV.";
        LOG_INFO("App", __func__, "UI", "Failed to open file for writing: {}", fullPath.string());
        return;
    }

    m_lastCsvExportStatus = "Current frame exported to " + fullPath.string();
    LOG_INFO("App", __func__, "UI", "Frame exported to {}", fullPath.string());
}

} // namespace App
