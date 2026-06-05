#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Application.h"

#include "SingleInstance.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (!clipsync::acquireSingleInstance()) {
        clipsync::activateExistingInstance();
        MessageBoxW(nullptr, L"LAN-ClipSync 已在运行中，无法启动第二个实例。",
                    L"LAN-ClipSync", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    try {
        clipsync::Application app;
        return app.run(instance);
    } catch (const std::exception& ex) {
        const char* msg = ex.what();
        MessageBoxA(nullptr, msg ? msg : "unknown", "LAN-ClipSync 启动异常",
                    MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"发生未知错误。", L"LAN-ClipSync 启动异常", MB_OK | MB_ICONERROR);
        return 1;
    }
}
