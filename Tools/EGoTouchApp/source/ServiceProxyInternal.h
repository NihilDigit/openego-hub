#pragma once

#include "ServiceProxy.h"

namespace App {

TouchPipelineModuleEnableState CaptureTouchPipelineModuleEnableState(
    const Solvers::TouchPipeline& pipeline);
void ApplyTouchPipelineModuleEnableState(
    Solvers::TouchPipeline& pipeline,
    const TouchPipelineModuleEnableState& state);

struct ServiceRuntimeConfigState {
    bool desiredModeFull = true;
    bool activeModeFull = true;
    bool autoMode = true;
    bool stylusVhfEnabled = true;
    PenButtonMode penButtonMode = PenButtonMode::WindowsInk;
    PenButtonRoute penButtonRoute = PenButtonRoute::VhfOnly;
};

struct AppRuntimeConfigState {
    bool vhfEnabled = true;
    bool vhfTranspose = false;
    bool masterParserOnly = false;
};

DvrRuntimeConfigSnapshot BuildRuntimeConfigSnapshotFromState(
    const ServiceRuntimeConfigState& serviceState,
    const AppRuntimeConfigState& appRuntimeState,
    const Solvers::TouchPipeline& touchPipeline,
    const Solvers::StylusPipeline& stylusPipeline);

Config::ConfigSchemaSnapshot BuildServiceProxyConfigSchemaSnapshotForTest();

} // namespace App
