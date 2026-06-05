#include "ThreadUtil.h"

namespace clipsync {

HANDLE createSmallStackThread(LPTHREAD_START_ROUTINE proc, void* param,
                              DWORD stackReserveBytes) {
    return CreateThread(nullptr, stackReserveBytes, proc, param, STACK_SIZE_PARAM_IS_A_RESERVATION,
                        nullptr);
}

}  // namespace clipsync
