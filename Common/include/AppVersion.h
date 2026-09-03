#pragma once
// 产品版本。可执行文件的 VERSIONINFO 资源按它填，「关于」页显示的就是从那里读回来的。
//
// 改版本要同时改两处:本文件、CMakeLists.txt 的 project(VERSION)。配置阶段有一道断言
// 盯着,对不上直接报错——设置窗口由 MSBuild 单独编译,读不到 CMake 的变量,所以只能这样
// 兜。pack_release_version.bat 不带参数时从 CMakeLists.txt 读版本,不必改它。
//
// .rc 与 .cpp 都能包含这个头:资源编译器只认预处理指令,这里除了 #define 什么都不能有。

#define OPENEGO_VERSION_MAJOR 0
#define OPENEGO_VERSION_MINOR 3
#define OPENEGO_VERSION_PATCH 0
#define OPENEGO_VERSION_STRING "0.3.0"
