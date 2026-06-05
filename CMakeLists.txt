cmake_minimum_required(VERSION 3.16)
project(LAN-ClipSync VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态 /MT：便于无 VC 运行库的虚拟机部署，但常驻内存通常更高
# 动态 /MD：内存更小，需目标机已安装 VC++ 运行库
option(LAN_CLIPSYNC_STATIC_RUNTIME "静态链接 MSVC 运行库 (/MT)" ON)
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    if(LAN_CLIPSYNC_STATIC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:DebugDLL>")
    endif()
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "LAN-ClipSync 仅支持 Windows 10/11 (Win32 API)。\n"
        "请在 Windows 上使用 Visual Studio 或 MSVC 工具链编译。")
endif()

enable_language(RC)

add_executable(LAN-ClipSync WIN32
    res/app.rc
    src/main.cpp
    src/app.manifest
    src/Application.cpp
    src/ClipboardManager.cpp
    src/NetworkManager.cpp
    src/PeerConfig.cpp
    src/PeerSettingsDialog.cpp
    src/QrDialog.cpp
    src/QrRender.cpp
    src/qrcodegen.cpp
    src/QrDecoder.cpp
    src/UiTheme.cpp
    src/JsonUtil.cpp
    src/Logger.cpp
    src/SingleInstance.cpp
    src/ThreadUtil.cpp
)

target_include_directories(LAN-ClipSync PRIVATE src res)

target_compile_definitions(LAN-ClipSync PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(LAN-ClipSync PRIVATE /W4 /utf-8
        $<$<CONFIG:Release>:/Os>)
    set_target_properties(LAN-ClipSync PROPERTIES VS_GLOBALS "DpiAwareness=PerMonitor")
else()
    # MinGW：静态链接 libgcc / libstdc++ / winpthread，避免目标机缺少 DLL
    target_compile_options(LAN-ClipSync PRIVATE -Wall -Wextra)
    target_link_options(LAN-ClipSync PRIVATE
        -municode
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

target_link_libraries(LAN-ClipSync PRIVATE
    ws2_32
    user32
    shell32
    comctl32
    iphlpapi
    advapi32
    dwmapi
    gdiplus
)
