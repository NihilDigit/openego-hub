#include "DiagnosticsWorkbench.h"
#include "ServiceProxy.h"
#include "imgui.h"
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

namespace App {

namespace {
constexpr const char* kExportRootDir = "C:/ProgramData/OpenEGoHub/exports";

bool DrawControlPanelConfigSyncGate(ServiceProxy* proxy) {
    if (proxy == nullptr || proxy->IsConfigAdjustmentAllowed()) {
        return true;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Config is not synchronized from Service; parameter adjustment is disabled.");
    const std::string message = proxy->GetConfigServiceSyncStatusMessage();
    if (!message.empty()) {
        ImGui::TextWrapped("%s", message.c_str());
    }
    if (proxy->IsConnected()) {
        if (ImGui::Button("Retry Config Sync##ControlPanel")) {
            proxy->SynchronizeConfigFromServiceForEditing();
        }
    }
    return false;
}
} // namespace

void DiagnosticsWorkbench::DrawControlPanel() {
    ImGui::Begin("Control Panel");

    const bool allowLiveControl = m_proxy && m_proxy->IsLiveControlAllowed();
    const bool configAdjustmentAllowed = m_proxy && m_proxy->IsConfigAdjustmentAllowed();
    if (!ImGui::BeginTabBar("ControlTabs")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabItem("Service")) {

    if (ImGui::Button("EXIT APPLICATION", ImVec2(-1, 30))) {
        if (m_proxy) {
            m_proxy->Disconnect();
        }
        ::PostQuitMessage(0);
    }
    ImGui::Separator();

    if (m_proxy) {
        // ── Service 连接状态 + 手动连接按钮 ──
        bool connected = m_proxy->IsConnected();
        const bool allowLiveControl = m_proxy->IsLiveControlAllowed();
        if (connected) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.f), "● Service: Connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.f), "● Service: Disconnected");
            ImGui::TextWrapped("Service is disconnected. Click Connect to Service to try a connection.");
        }

        if (connected) {
            // Disconnect button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.f));
            if (ImGui::Button("Disconnect from Service", ImVec2(-1, 0))) {
                m_proxy->Disconnect();
            }
            ImGui::PopStyleColor();

            ImGui::Separator();

            // Start/Stop Runtime
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
            if (ImGui::Button("Stop Runtime", ImVec2(-1, 0))) {
                m_proxy->StopRemoteRuntime();
            }
            ImGui::PopStyleColor();
        } else {
            // Manual connect button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.f));
            if (ImGui::Button("Connect to Service", ImVec2(-1, 0))) {
                m_proxy->TryConnect();
            }
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Service Actions");

        if (connected) {
            if (!allowLiveControl) ImGui::BeginDisabled();
            if (ImGui::Button("Toggle Runtime", ImVec2(-1, 0))) {
                m_proxy->StartRemoteRuntime();
            }
            if (!allowLiveControl) ImGui::EndDisabled();
        }


    }

    ImGui::Separator();
    ImGui::TextUnformatted("System Events");
    DrawSystemEventsPanel();

#ifdef _DEBUG
    if (!configAdjustmentAllowed) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Apply Global Parameters (Session Only)")) {
        m_proxy->SaveConfig();
    }
    ImGui::PopStyleColor();
    if (!configAdjustmentAllowed) ImGui::EndDisabled();

    bool masterParserOnly = (m_proxy != nullptr) && m_proxy->IsMasterParserOnlyMode();
    if (!m_proxy) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Master Parser Only (Raw Heatmap)", &masterParserOnly) && m_proxy) {
        m_proxy->SetMasterParserOnlyMode(masterParserOnly);
    }
    if (!m_proxy) ImGui::EndDisabled();
    if (masterParserOnly) {
        ImGui::TextUnformatted("Enabled: only Master Frame Parser is active.");
    }

    ImGui::Separator();
    bool vhfReportingEnabled = (m_proxy != nullptr) && m_proxy->IsVhfEnabled();
    if (!m_proxy || !allowLiveControl) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Enable VHF Output", &vhfReportingEnabled) && m_proxy) {
        m_proxy->SetVhfEnabled(vhfReportingEnabled);
    }
    if (m_proxy) {
        ImGui::Text("VHF: %s", m_proxy->IsVhfEnabled() ? "Enabled" : "Disabled");
    }
    if (!m_proxy || !allowLiveControl) ImGui::EndDisabled();

    // Global Service Options
    ImGui::Separator();
    ImGui::TextUnformatted("Global Config");
    if (m_proxy) {
        const bool globalConfigEnabled = DrawControlPanelConfigSyncGate(m_proxy);
        if (!globalConfigEnabled) ImGui::BeginDisabled();
        const bool desiredFull = m_proxy->IsSrvModeFull();
        const bool activeFull = m_proxy->IsSrvActiveModeFull();
        bool isTouchOnly = !desiredFull;
        if (ImGui::Checkbox("Touch-Only Mode (Pure Finger, Disable Pen)", &isTouchOnly)) {
            m_proxy->SetSrvModeFull(!isTouchOnly);
        }
        const char* activeModeText = activeFull ? "Full" : "Touch-Only";
        const char* desiredModeText = desiredFull ? "Full" : "Touch-Only";
        if (desiredFull == activeFull) {
            ImGui::TextDisabled("Mode: active=%s", activeModeText);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Mode pending: desired=%s, active=%s",
                               desiredModeText,
                               activeModeText);
        }

        ImGui::TextDisabled("service.mode is staged and requires Service restart.");

        bool autoMode = m_proxy->IsSrvAutoMode();
        if (ImGui::Checkbox("Auto-Mode (Hardware handshake)", &autoMode)) {
            m_proxy->SetSrvAutoMode(autoMode);
        }
        ImGui::TextDisabled("service.auto_mode is live-applied by Apply Global.");

        if (ImGui::Button("Apply Global")) {
            m_proxy->ApplyConfigStoreGlobally();
        }
        DrawApplyConfigResultStatus();
        if (!globalConfigEnabled) ImGui::EndDisabled();
    }
#else
    ImGui::Separator();
    ImGui::TextUnformatted("Global Config");
    ImGui::TextWrapped("Release builds can live-apply supported Service config keys. Persistence depends on Service/build support; service.mode still requires restart.");
    if (m_proxy) {
        const bool globalConfigEnabled = DrawControlPanelConfigSyncGate(m_proxy);
        if (!globalConfigEnabled) ImGui::BeginDisabled();
        const char* activeModeText = m_proxy->IsSrvActiveModeFull() ? "Full" : "Touch-Only";
        const char* desiredModeText = m_proxy->IsSrvModeFull() ? "Full" : "Touch-Only";
        ImGui::TextDisabled("Mode: desired=%s active=%s (restart required)", desiredModeText, activeModeText);
        ImGui::TextDisabled("Auto-Mode: %s (live apply)", m_proxy->IsSrvAutoMode() ? "Enabled" : "Disabled");
        ImGui::TextDisabled("VHF: %s", m_proxy->IsVhfEnabled() ? "Enabled" : "Disabled");
        if (ImGui::Button("Apply Global")) {
            m_proxy->ApplyConfigStoreGlobally();
        }
        DrawApplyConfigResultStatus();
        if (!globalConfigEnabled) ImGui::EndDisabled();
    }
#endif


    ImGui::Checkbox("Auto-refresh Heatmap", &m_autoRefresh);
    ImGui::Checkbox("Render Visualization", &m_renderVisualization);
    if (!m_renderVisualization) {
        ImGui::TextUnformatted("Visualization disabled: acquisition/processing threads keep running.");
    }
    ImGui::Checkbox("Fullscreen Heatmap", &m_fullscreen);
    ImGui::SliderInt("Heatmap Scale", &m_heatmapScale, 1, 30);
    ImGui::SliderFloat("Color Max Range", &m_colorRange, 100.0f, 10000.0f, "%.0f");

    ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("DVR")) {
        DrawDvrPanel();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

} // namespace App
