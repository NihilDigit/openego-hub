cmake_minimum_required(VERSION 3.20)
project(EGoTouchRev VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 版本号在两个构建系统里各存一份:CMake 这边给服务与托盘,Common/include/AppVersion.h
# 给两个 .rc 的 VERSIONINFO(设置窗口由 MSBuild 单独编译,读不到 CMake 变量)。两份对不上
# 时「关于」页会显示一个和安装包不一致的版本,而那正是它唯一的用处,所以在这里挡一道。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Common/include/AppVersion.h" _app_version_header)
string(REGEX MATCH "OPENEGO_VERSION_STRING[ \t]+\"([0-9.]+)\"" _ "${_app_version_header}")
if(NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "版本号不一致: CMakeLists.txt 是 ${PROJECT_VERSION}, "
        "Common/include/AppVersion.h 是 ${CMAKE_MATCH_1}。两处要一起改。")
endif()
unset(_app_version_header)

# 自动生成 .clangd 配置文件以支持 clangd 自动识别系统头文件和 Windows Kits
if (WIN32 AND CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CLANGD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.clangd")
    message(STATUS "Checking/Generating .clangd config for clangd language server...")
    
    # 查找本地最新的 MSVC 包含路径
    set(MSVC_INCLUDE_DIRS "")
    file(GLOB vs_msvc_dirs 
        "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*"
        "C:/Program Files/Microsoft Visual Studio/18/*/VC/Tools/MSVC/*"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*"
    )
    if (vs_msvc_dirs)
        list(SORT vs_msvc_dirs ORDER DESCENDING)
        list(GET vs_msvc_dirs 0 latest_msvc_dir)
        if (EXISTS "${latest_msvc_dir}/include")
            list(APPEND MSVC_INCLUDE_DIRS 
                "${latest_msvc_dir}/include"
                "${latest_msvc_dir}/atlmfc/include"
            )
        endif()
    endif()

    # 查找本地最新的 Windows SDK 包含路径
    set(WIN_SDK_INCLUDE_DIRS "")
    file(GLOB sdk_dirs "C:/Program Files (x86)/Windows Kits/10/Include/*")
    if (sdk_dirs)
        list(SORT sdk_dirs ORDER DESCENDING)
        foreach(sdk_dir IN LISTS sdk_dirs)
            if (sdk_dir MATCHES "[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+")
                set(latest_sdk_dir "${sdk_dir}")
                break()
            endif()
        endforeach()
        
        if (latest_sdk_dir)
            list(APPEND WIN_SDK_INCLUDE_DIRS
                "${latest_sdk_dir}/ucrt"
                "${latest_sdk_dir}/shared"
                "${latest_sdk_dir}/um"
                "${latest_sdk_dir}/winrt"
                "${latest_sdk_dir}/cppwinrt"
            )
        endif()
    endif()
    
    # 构造 .clangd 的配置内容
    set(SYSTEM_INCLUDES ${MSVC_INCLUDE_DIRS} ${WIN_SDK_INCLUDE_DIRS})
    if (SYSTEM_INCLUDES)
        set(CLANGD_CONTENT "CompileFlags:\n")
        file(RELATIVE_PATH clangd_compilation_database "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
        string(REPLACE "\\" "/" clangd_compilation_database "${clangd_compilation_database}")
        string(APPEND CLANGD_CONTENT "  CompilationDatabase: ${clangd_compilation_database}\n")
        string(APPEND CLANGD_CONTENT "  Add:\n")
        
        foreach(dir IN LISTS SYSTEM_INCLUDES)
            string(REPLACE "\\" "/" dir_clean "${dir}")
            string(APPEND CLANGD_CONTENT "    - /imsvc\n")
            string(APPEND CLANGD_CONTENT "    - ${dir_clean}\n")
        endforeach()
        
        file(WRITE "${CLANGD_FILE}" "${CLANGD_CONTENT}")
        message(STATUS "Auto-generated .clangd config successfully at: ${CLANGD_FILE}")
    else()
        message(WARNING "Could not auto-detect MSVC or Windows SDK directories. .clangd was not generated.")
    endif()
endif()

# --- Target architecture gate ---
# This project ships ARM64-native binaries only (no WOW64). The arm64-* presets select
# cl.exe and rely on the surrounding environment (vcvarsarm64.bat) to supply an ARM64
# toolchain, which CMake cannot verify on its own — "architecture": {"strategy":
# "external"} explicitly opts out of that check. Building them from an x64 developer
# prompt would otherwise emit x64 binaries under an arm64-named build directory and only
# fail at install time on the device. Fail here instead.
option(EGO_REQUIRE_ARM64 "Fail configuration unless the compiler targets ARM64" ON)
if (EGO_REQUIRE_ARM64 AND WIN32)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_TARGET)
        set(_ego_target "${CMAKE_CXX_COMPILER_TARGET}")
    else()
        set(_ego_target "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
    if (NOT _ego_target MATCHES "^([Aa][Rr][Mm]64|aarch64)")
        message(FATAL_ERROR
            "Compiler targets '${_ego_target}', not ARM64.\n"
            "Run the build from an ARM64 developer environment, e.g.\n"
            "  \"%ProgramFiles(x86)%\\Microsoft Visual Studio\\2022\\BuildTools"
            "\\VC\\Auxiliary\\Build\\vcvarsarm64.bat\"\n"
            "then re-run cmake --preset. Pass -DEGO_REQUIRE_ARM64=OFF to override.")
    endif()
    unset(_ego_target)
endif()

set(CMAKE_CTEST_ARGUMENTS "--verbose" CACHE STRING "Arguments passed to ctest by generated test targets")
include(CTest)
enable_testing()
# Use static MSVC runtime to avoid missing VCRUNTIME140.dll on target machines
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Platform compatibility ---
# Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
# Suppress MSVC CRT deprecation warnings (e.g. localtime -> localtime_s)
if (WIN32)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        # Sources are UTF-8 and carry Chinese comments/literals. Without /utf-8 cl.exe
        # decodes them with the system ANSI code page (936 on zh-CN), which corrupts
        # string literals into "C2001: newline in constant". Clang defaults to UTF-8,
        # which is why this only surfaces on MSVC.
        add_compile_options(/utf-8)
        # try/catch is used across the service entry and the test binaries; without /EHsc
        # MSVC warns C4530 and skips unwind semantics, so destructors may not run.
        add_compile_options(/EHsc)
    endif()
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(/Zc:preprocessor)
    endif()
endif()

# --- Global Optimization & Debug Options ---
option(EGO_GENERATE_PDB "Generate PDB debug symbols in Release build" ON)

if(MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    # 区分 MSVC 兼容的编译驱动（cl.exe / clang-cl.exe）与常规 GNU 风格的 clang 驱动
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC" OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        if(EGO_GENERATE_PDB)
            add_compile_options($<$<CONFIG:Release>:/Zi>)
            add_link_options($<$<CONFIG:Release>:/DEBUG:FULL>)
        endif()
        add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    else()
        # 常规 clang.exe/clang++.exe 驱动模式且目标为 MSVC 环境时
        if(EGO_GENERATE_PDB)
            add_compile_options($<$<CONFIG:Release>:-g>)
            add_link_options($<$<CONFIG:Release>:-Wl,-debug>)
        endif()
        add_link_options($<$<CONFIG:Release>:-Wl,-opt:ref> $<$<CONFIG:Release>:-Wl,-opt:icf>)
    endif()
endif()

# 启用全局 LTO (Link-Time Optimization)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT error)
if(ipo_supported)
    message(STATUS "Link-Time Optimization (LTO/IPO) is supported. Enabling for Release configuration...")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "Link-Time Optimization (LTO/IPO) is not supported: ${error}")
endif()

# --- SIMD options ---
# This project targets a specific platform; enable NEON by default.
option(HIMAX_ENABLE_NEON "Enable ARM NEON intrinsics when available" ON)

# --- Build Controls ---
option(EGO_BUILD_TESTS "Build local test executables" ON)
option(EGO_BUILD_TOOLS "Build the standalone diagnostic tools" ON)

# --- Common Library ---
# Use the source directory as the root for subprojects/resources so paths
# resolve correctly whether building in-tree or out-of-tree.
set(COMMON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Common")
file(GLOB COMMON_SOURCES "${COMMON_ROOT}/source/*.cpp")
file(GLOB COMMON_HEADERS "${COMMON_ROOT}/include/*.h")

# Common library: Pure utilities (Logger, SharedFrameBuffer)
add_library(Common STATIC
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

target_sources(Common PRIVATE
    "${COMMON_ROOT}/source/PenStatusChannel.cpp"
    "${COMMON_ROOT}/source/PenControlChannel.cpp"
    "${COMMON_ROOT}/source/PenInkShortcut.cpp"
    "${COMMON_ROOT}/source/ManagedResource.cpp"
    "${COMMON_ROOT}/source/config/ConfigStore.cpp"
    "${COMMON_ROOT}/source/config/ConfigBinder.cpp"
    "${COMMON_ROOT}/source/config/ConfigSchemaSnapshot.cpp"
    "${COMMON_ROOT}/source/config/SchemaValidator.cpp"
    "${COMMON_ROOT}/source/config/ConfigTlv.cpp"
    "${COMMON_ROOT}/source/config/ConfigKeyMap.cpp"
    "${COMMON_ROOT}/source/config/ConfigCatalog.cpp"
)

target_include_directories(Common PUBLIC
    "${COMMON_ROOT}/include"
)
target_compile_definitions(Common PRIVATE
    $<$<CONFIG:Release>:EGOTOUCH_LOGGER_SYNC_FILE_ONLY=1>
)

# 启用预编译头 (PCH) 以减少编译时的重复解析开销
target_precompile_headers(Common PRIVATE
    <vector>
    <string>
    <memory>
    <unordered_map>
    <algorithm>
    <Windows.h>
)

if (WIN32)
    target_link_libraries(Common PUBLIC advapi32)
endif()

# --- Device Module (Hardware Abstraction) ---
set(DEVICE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/EGoTouchService/Device")
file(GLOB_RECURSE DEVICE_SOURCES "${DEVICE_ROOT}/*.cpp")
file(GLOB_RECURSE DEVICE_HEADERS "${DEVICE_ROOT}/*.h")
list(FILTER DEVICE_SOURCES EXCLUDE REGEX "[/\\\\]tests[/\\\\]")
list(FILTER DEVICE_HEADERS EXCLUDE REGEX "[/\\\\]tests[/\\\\]")

add_library(Device STATIC
    ${DEVICE_SOURCES}
    ${DEVICE_HEADERS}
)

target_compile_definitions(Device PUBLIC
    $<$<BOOL:${HIMAX_ENABLE_NEON}>:HIMAX_ENABLE_NEON=1>
    $<$<NOT:$<BOOL:${HIMAX_ENABLE_NEON}>>:HIMAX_ENABLE_NEON=0>
)


target_include_directories(Device PUBLIC
    "${DEVICE_ROOT}"
)
target_link_libraries(Device PUBLIC Common Host setupapi hid)

# --- Host Module (System Integration) ---
set(HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/EGoTouchService/Host")
file(GLOB HOST_SOURCES "${HOST_ROOT}/*.cpp")
file(GLOB HOST_HEADERS "${HOST_ROOT}/*.h")

add_library(Host STATIC
    ${HOST_SOURCES}
    ${HOST_HEADERS}
)
target_include_directories(Host PUBLIC "${HOST_ROOT}")
target_link_libraries(Host PUBLIC Common)

if (WIN32)
    target_link_libraries(Host PUBLIC advapi32)
endif()

function(ego_copy_config_to_output target)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config/default.yaml")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target}>/config"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/config/default.yaml"
                "$<TARGET_FILE_DIR:${target}>/config/default.yaml"
            COMMENT "Copy config/default.yaml for ${target}"
        )
    endif()
endfunction()

# --- EGoTouchService (Production Service Shell) ---
set(SERVICE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/EGoTouchService")

add_library(EGoTouchServiceCore STATIC
    "${SERVICE_ROOT}/source/ConfigRuntime.cpp"
    "${SERVICE_ROOT}/source/ServiceConfigCore.cpp"
    "${SERVICE_ROOT}/source/ServiceEntryCore.cpp"
)
target_include_directories(EGoTouchServiceCore PUBLIC
    "${SERVICE_ROOT}/include"
)
target_link_libraries(EGoTouchServiceCore PUBLIC Common advapi32)


add_executable(EGoTouchService
    "${SERVICE_ROOT}/source/ServiceEntry.cpp"
    "${SERVICE_ROOT}/source/ServiceShell.cpp"
    "${SERVICE_ROOT}/source/ServiceHost.cpp"
    "${SERVICE_ROOT}/source/PenSettingsStore.cpp"
    "${SERVICE_ROOT}/source/TouchProviderCoordinator.cpp"
    "${SERVICE_ROOT}/source/VendorServices.cpp"
    "${SERVICE_ROOT}/Service.rc"
)
# 与托盘同理：rc.exe 没有 /showIncludes，CMake 不为 .rc 生成 depfile，改了 AppVersion.h
# 也不会重编这个资源。见 EGoTouchTray 那处的说明。
set_source_files_properties("${SERVICE_ROOT}/Service.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Common/include/AppVersion.h")
# gaokun-hal：凡是与华为 x64 DLL 打交道的部分都在那边。这里链接的只是触控宿主的进程
# 控制器，它是原生 ARM64 且不加载任何厂商 DLL，因此本服务保持原生 ARM64——真正需要 EC 的
# GaokunThpHost.exe 是一个独立进程，由 hal 自己的 build.ps1 构建，不经过这里的 preset。
# 两个仓库尚未拆分，暂时经 hal 符号链接引用。
set(GAOKUN_HAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/hal")
# 按配置选库：静态库要与调用方用同一套运行库，否则链接期报 LNK2038（_ITERATOR_DEBUG_LEVEL
# 与 RuntimeLibrary 不匹配）。hal 的 build.ps1 为此分别产出 Debug 与 Release 两份。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(GAOKUN_HAL_CONFIG "Debug")
else()
    set(GAOKUN_HAL_CONFIG "Release")
endif()
set(GAOKUN_HAL_LIB "${GAOKUN_HAL_ROOT}/build/${GAOKUN_HAL_CONFIG}/GaokunHal.lib")
if(NOT EXISTS "${GAOKUN_HAL_LIB}")
    message(FATAL_ERROR
        "gaokun-hal is not built for ${GAOKUN_HAL_CONFIG}: ${GAOKUN_HAL_LIB}\n"
        "Run scripts\\build.ps1 -Config ${GAOKUN_HAL_CONFIG} in the gaokun-hal repository first.")
endif()

target_include_directories(EGoTouchService PRIVATE
    "${SERVICE_ROOT}/include"
    "${GAOKUN_HAL_ROOT}/include"
)
target_link_libraries(EGoTouchService PRIVATE EGoTouchServiceCore Device Host Common advapi32 setupapi hid shell32 "${GAOKUN_HAL_LIB}")

# 服务开发构建必须使用与 GaokunHal.lib 同一配置生成的笔宿主。ResolveHostPath 的历史回退固定
# 指向 hal/build/Release，Debug 服务因而会悄悄启动旧宿主；新增 IPC 后表现为服务端代码已更新、
# 命令管道却永远不存在。把宿主放到服务同目录，部署路径和开发路径便使用同一条优先规则。
set(GAOKUN_PEN_HOST_EXE
    "${GAOKUN_HAL_ROOT}/build/${GAOKUN_HAL_CONFIG}/GaokunPenHost.exe")
set(EGO_PEN_HOST_EXE "${CMAKE_CURRENT_BINARY_DIR}/GaokunPenHost.exe")
add_custom_command(
    OUTPUT "${EGO_PEN_HOST_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${GAOKUN_PEN_HOST_EXE}" "${EGO_PEN_HOST_EXE}"
    DEPENDS "${GAOKUN_PEN_HOST_EXE}"
    COMMENT "Copying Gaokun pen host beside OpenEGoHubService"
    VERBATIM
)
add_custom_target(EGoTouchPenRuntime ALL DEPENDS "${EGO_PEN_HOST_EXE}")
add_dependencies(EGoTouchService EGoTouchPenRuntime)

ego_copy_config_to_output(EGoTouchService)

install(TARGETS EGoTouchService
    RUNTIME DESTINATION "."
    COMPONENT Runtime
)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config/default.yaml")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/default.yaml"
        DESTINATION "config"
        COMPONENT Runtime
    )
endif()

# EGoTouchService uses wmain (console subsystem) for service/console dual mode
if(MSVC)
    set_target_properties(EGoTouchService PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
elseif(WIN32)
    target_link_options(EGoTouchService PRIVATE -municode)
endif()

# --- EGoTouchTray (pen status companion) ---
# Built in every configuration, not gated on EGO_BUILD_TOOLS: unlike the diagnostic
# tools this ships alongside the service. It runs unelevated by design, so no
# requireAdministrator manifest here.
if(WIN32 AND MSVC)
    find_program(EGO_MSBUILD_EXECUTABLE
        NAMES MSBuild.exe
        HINTS
            "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/arm64"
            "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/arm64"
            "$ENV{VSINSTALLDIR}/MSBuild/Current/Bin/arm64"
            "$ENV{VSINSTALLDIR}/MSBuild/Current/Bin"
        REQUIRED
    )

    set(EGO_SETTINGS_PROJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EGoTouchSettings/EGoTouchSettings.vcxproj")
    set(EGO_SETTINGS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}")
    set(EGO_SETTINGS_INTERMEDIATE
        "${CMAKE_CURRENT_BINARY_DIR}/EGoTouchSettings-int/${CMAKE_BUILD_TYPE}")
    set(EGO_SETTINGS_SOURCES
        Tools/EGoTouchSettings/App.xaml
        Tools/EGoTouchSettings/App.xaml.cpp
        Tools/EGoTouchSettings/App.xaml.h
        Tools/EGoTouchSettings/BatteryIndicator.xaml
        Tools/EGoTouchSettings/BatteryIndicator.xaml.cpp
        Tools/EGoTouchSettings/BatteryIndicator.xaml.h
        Tools/EGoTouchSettings/LogExport.cpp
        Tools/EGoTouchSettings/LogExport.h
        Tools/EGoTouchSettings/MainWindow.xaml
        Tools/EGoTouchSettings/MainWindow.xaml.cpp
        Tools/EGoTouchSettings/MainWindow.xaml.h
        Tools/EGoTouchSettings/NotificationWindow.xaml
        Tools/EGoTouchSettings/NotificationWindow.xaml.cpp
        Tools/EGoTouchSettings/NotificationWindow.xaml.h
        Tools/EGoTouchSettings/Types.idl
        Tools/EGoTouchSettings/app.manifest
        Tools/EGoTouchSettings/pch.cpp
        Tools/EGoTouchSettings/pch.h
        # 资源那一路也要列进来。少了它们，换图标或改版本号不会触发重建，产物却看着是新的
        # ——这类「构建成功但跑的是旧东西」在这个仓库已经咬过好几次。
        Tools/EGoTouchSettings/App.rc
        Tools/EGoTouchSettings/EGoTouchSettings.vcxproj
        Tools/EGoTouchSettings/Assets/OpenEGoHub.ico
        Common/include/AppIconResource.h
        Common/include/AppVersion.h
        Common/include/EGoTouchTrayIpc.h
        Common/include/ManagedResource.h
        Common/include/PenButtonConfig.h
        Common/include/PenStatusChannel.h
        Common/source/PenStatusChannel.cpp
        Common/source/ManagedResource.cpp
    )

    add_custom_command(
        OUTPUT "${EGO_SETTINGS_OUTPUT}/OpenEGoHubSettings.exe"
        COMMAND "${EGO_MSBUILD_EXECUTABLE}" "${EGO_SETTINGS_PROJECT}"
            /restore
            /nologo
            /verbosity:minimal
            /p:Configuration=${CMAKE_BUILD_TYPE}
            /p:Platform=ARM64
            /p:OutDir=${EGO_SETTINGS_OUTPUT}/
            /p:IntDir=${EGO_SETTINGS_INTERMEDIATE}/
        DEPENDS ${EGO_SETTINGS_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EGoTouchSettings"
        COMMENT "Building EGoTouchSettings (WinUI 3, ARM64)"
        VERBATIM
        USES_TERMINAL
    )
    add_custom_target(EGoTouchSettings ALL
        DEPENDS "${EGO_SETTINGS_OUTPUT}/OpenEGoHubSettings.exe")
endif()

add_executable(EGoTouchTray WIN32
    Tools/EGoTouchTray/EGoTouchTray.cpp
    Tools/EGoTouchTray/Tray.rc
)
# rc.exe 没有 /showIncludes，CMake 因此不为 .rc 生成 depfile：改了 AppVersion.h,ninja
# 看不出 Tray.rc 需要重编,托盘的 VERSIONINFO 就静默留在上一个版本号上。上面那道断言只
# 比对 CMakeLists 与头文件两处的字面值,抓不到这种情况——实际发生过一次,0.1.1 跟着
# 0.2.0 的包一起发了出去。
set_source_files_properties(Tools/EGoTouchTray/Tray.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Common/include/AppVersion.h")
target_include_directories(EGoTouchTray PRIVATE "${COMMON_ROOT}/include")
target_link_libraries(EGoTouchTray PRIVATE Common advapi32 comctl32 d2d1 dwrite dwmapi
    shell32 shcore user32 gdi32 uiautomationcore uxtheme)
set_target_properties(EGoTouchTray PROPERTIES OUTPUT_NAME OpenEGoHubTray)
if(TARGET EGoTouchSettings)
    add_dependencies(EGoTouchTray EGoTouchSettings)
endif()

# 色温与护眼命令由用户会话里的托盘执行，GaokunDisplay 因而必须和托盘在同一目录；
# qdcmlib 也必须紧挨 GaokunDisplay，不能误加载 System32 里的不兼容版本。安装脚本已有同样
# 的布局，这里补齐直接从 build 目录运行的 Debug/Release 开发链路。
set(GAOKUN_DISPLAY_EXE
    "${GAOKUN_HAL_ROOT}/build/${GAOKUN_HAL_CONFIG}/GaokunDisplay.exe")
set(GAOKUN_QDCM_DLL
    "${GAOKUN_HAL_ROOT}/build/${GAOKUN_HAL_CONFIG}/qdcmlib.dll")
set(EGO_DISPLAY_RUNTIME_EXE "${CMAKE_CURRENT_BINARY_DIR}/GaokunDisplay.exe")
set(EGO_DISPLAY_RUNTIME_DLL "${CMAKE_CURRENT_BINARY_DIR}/qdcmlib.dll")
add_custom_command(
    OUTPUT "${EGO_DISPLAY_RUNTIME_EXE}" "${EGO_DISPLAY_RUNTIME_DLL}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${GAOKUN_DISPLAY_EXE}" "${EGO_DISPLAY_RUNTIME_EXE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${GAOKUN_QDCM_DLL}" "${EGO_DISPLAY_RUNTIME_DLL}"
    DEPENDS "${GAOKUN_DISPLAY_EXE}" "${GAOKUN_QDCM_DLL}"
    COMMENT "Copying Gaokun display runtime beside OpenEGoHubTray"
    VERBATIM
)
add_custom_target(EGoTouchDisplayRuntime ALL
    DEPENDS "${EGO_DISPLAY_RUNTIME_EXE}" "${EGO_DISPLAY_RUNTIME_DLL}")
add_dependencies(EGoTouchTray EGoTouchDisplayRuntime)

# 面向用户的可执行文件名随产品走，CMake target 名保持不变：target 名只出现在构建脚本、
# CI 与 add_dependencies 里，改它没有用户可见的收益，却会牵连每一处引用。
set_target_properties(EGoTouchService PROPERTIES OUTPUT_NAME OpenEGoHubService)

install(TARGETS EGoTouchTray
    RUNTIME DESTINATION "."
    COMPONENT Runtime
)
if(TARGET EGoTouchSettings)
    install(FILES
        "${EGO_SETTINGS_OUTPUT}/OpenEGoHubSettings.exe"
        "${EGO_SETTINGS_OUTPUT}/OpenEGoHubSettings.pri"
        "${EGO_SETTINGS_OUTPUT}/EGoTouchSettings.winmd"
        "${EGO_SETTINGS_OUTPUT}/App.xbf"
        "${EGO_SETTINGS_OUTPUT}/BatteryIndicator.xbf"
        "${EGO_SETTINGS_OUTPUT}/MainWindow.xbf"
        "${EGO_SETTINGS_OUTPUT}/NotificationWindow.xbf"
        "${EGO_SETTINGS_OUTPUT}/Microsoft.WindowsAppRuntime.Bootstrap.dll"
        DESTINATION "."
        COMPONENT Runtime
    )
endif()

# --- PointerCancelProbe (one-shot VHF confidence-bit experiment) ---
if (EGO_BUILD_TOOLS)
    add_executable(PointerCancelProbe Tools/PointerCancelProbe/PointerCancelProbe.cpp)
    target_link_libraries(PointerCancelProbe PRIVATE setupapi user32 gdi32)
endif()

if (EGO_BUILD_TESTS)
    function(ego_add_common_test target source)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE "${COMMON_ROOT}/include")
        target_link_libraries(${target} PRIVATE Common)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES
            LABELS "Common;unit;device-free"
            TIMEOUT 10
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
        )
    endfunction()

    ego_add_common_test(CommonLoggerTest Common/tests/LoggerTest.cpp)
    ego_add_common_test(CommonGuiLogSinkTest Common/tests/GuiLogSinkTest.cpp)
    ego_add_common_test(CommonFrameLayoutTest Common/tests/FrameLayoutTest.cpp)
    ego_add_common_test(CommonPenButtonConfigTest Common/tests/PenButtonConfigTest.cpp)
    ego_add_common_test(ConfigTlvTest Common/tests/ConfigTlvTest.cpp)
    ego_add_common_test(CommonHeaderCompileTest Common/tests/HeaderCompileTest.cpp)
    ego_add_common_test(CommonConfigCatalogTest Common/tests/ConfigCatalogTest.cpp)
    ego_add_common_test(CommonPenStatusChannelTest Common/tests/PenStatusChannelTest.cpp)
    ego_add_common_test(CommonPenControlChannelTest Common/tests/PenControlChannelTest.cpp)

    add_executable(TouchProviderCoordinatorTest
        EGoTouchService/tests/unit/TouchProviderCoordinatorTest.cpp
        EGoTouchService/source/TouchProviderCoordinator.cpp
    )
    target_include_directories(TouchProviderCoordinatorTest PRIVATE
        "${SERVICE_ROOT}/include"
        "${COMMON_ROOT}/include"
    )
    target_link_libraries(TouchProviderCoordinatorTest PRIVATE Common)
    add_test(NAME TouchProviderCoordinatorTest COMMAND TouchProviderCoordinatorTest)
    set_tests_properties(TouchProviderCoordinatorTest PROPERTIES
        LABELS "service;unit;device-free"
        TIMEOUT 10
    )

    add_executable(HostSystemStateMonitorTest
        EGoTouchService/Host/tests/SystemStateMonitorNamedEventTest.cpp
    )
    target_link_libraries(HostSystemStateMonitorTest PRIVATE Host Common)

    add_executable(ConfigRuntimeTest
        EGoTouchService/tests/unit/ConfigRuntimeTest.cpp
    )
    target_link_libraries(ConfigRuntimeTest PRIVATE EGoTouchServiceCore)
    ego_copy_config_to_output(ConfigRuntimeTest)
    add_test(NAME ConfigRuntimeTest COMMAND ConfigRuntimeTest)
    set_tests_properties(ConfigRuntimeTest PROPERTIES
        LABELS "service;config;unit;device-free"
        TIMEOUT 10
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:ConfigRuntimeTest>"
    )


    add_subdirectory("${DEVICE_ROOT}/tests")

    add_test(NAME HostSystemStateMonitorTest COMMAND HostSystemStateMonitorTest)
    set_tests_properties(HostSystemStateMonitorTest PROPERTIES
        LABELS "host;integration;windows;device-free;named-event"
        TIMEOUT 15
        RESOURCE_LOCK "host-system-state-named-events"
    )

    set(SERVICE_TEST_ROOT "${SERVICE_ROOT}/tests")

    add_executable(ServiceEntryArgumentRoutingTest
        "${SERVICE_TEST_ROOT}/unit/ServiceEntryArgumentRoutingTest.cpp"
    )
    target_include_directories(ServiceEntryArgumentRoutingTest PRIVATE "${SERVICE_TEST_ROOT}/support")
    target_link_libraries(ServiceEntryArgumentRoutingTest PRIVATE EGoTouchServiceCore)

    add_executable(ServiceHostLifecycleFakeIntegrationTest
        "${SERVICE_TEST_ROOT}/integration/ServiceHostLifecycleFakeIntegrationTest.cpp"
    )
    target_include_directories(ServiceHostLifecycleFakeIntegrationTest PRIVATE
        "${SERVICE_TEST_ROOT}/support"
        "${HOST_ROOT}"
    )
    target_link_libraries(ServiceHostLifecycleFakeIntegrationTest PRIVATE EGoTouchServiceCore)

    add_test(NAME ServiceEntryArgumentRoutingTest COMMAND ServiceEntryArgumentRoutingTest)
    add_test(NAME ServiceHostLifecycleFakeIntegrationTest COMMAND ServiceHostLifecycleFakeIntegrationTest)

    set_tests_properties(ServiceEntryArgumentRoutingTest PROPERTIES
        LABELS "unit;service;windows"
        TIMEOUT 10
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:ServiceEntryArgumentRoutingTest>")
    set_tests_properties(ServiceHostLifecycleFakeIntegrationTest PROPERTIES
        LABELS "integration;service;windows"
        TIMEOUT 10
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:ServiceHostLifecycleFakeIntegrationTest>")
endif()
