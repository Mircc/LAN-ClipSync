#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

/** 创建预留栈较小的线程（默认 256KB，系统默认约 1MB） */
HANDLE createSmallStackThread(LPTHREAD_START_ROUTINE proc, void* param,
                              DWORD stackReserveBytes = 256 * 1024);

}  // namespace clipsync
