#pragma once

#include <string>

namespace clipsync {

class Logger {
public:
    static void init();
    static void info(const std::wstring& msg);
    static void warn(const std::wstring& msg);
    static void error(const std::wstring& msg);

private:
    static void write(const wchar_t* level, const std::wstring& msg);
};

}  // namespace clipsync
