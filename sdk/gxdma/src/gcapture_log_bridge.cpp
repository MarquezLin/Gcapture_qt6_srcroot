#include "gcapture.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gcap
{
    void log_message(gcap_log_level_t, const char *message_utf8)
    {
#ifdef _WIN32
        const char *msg = message_utf8 ? message_utf8 : "";
        OutputDebugStringA(msg);
        const size_t len = std::strlen(msg);
        if (len == 0 || msg[len - 1] != '\n')
            OutputDebugStringA("\n");
#else
        (void)message_utf8;
#endif
    }

    void log_message_w(gcap_log_level_t level, const wchar_t *message_wide)
    {
#ifdef _WIN32
        if (!message_wide)
        {
            log_message(level, "");
            return;
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, message_wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return;
        std::string utf8(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, message_wide, -1, utf8.data(), needed, nullptr, nullptr);
        log_message(level, utf8.c_str());
#else
        (void)level;
        (void)message_wide;
#endif
    }

    void log_printf(gcap_log_level_t level, const char *fmt, ...)
    {
        if (!fmt)
        {
            log_message(level, "");
            return;
        }

        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_message(level, buf);
    }
}
