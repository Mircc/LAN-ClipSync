#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace clipsync {

/** 获取单实例锁；若已有实例在运行则返回 false */
bool acquireSingleInstance();

/** 第二实例启动时发往主窗口的自定义消息 */
constexpr UINT kSingleInstanceActivateMsg = WM_APP + 10;

/** 尝试将已有实例唤至前台 */
void activateExistingInstance();

}  // namespace clipsync
