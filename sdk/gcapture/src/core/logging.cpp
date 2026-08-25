#include "logging.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    std::atomic<gcap_log_callback_t> g_callback{nullptr};
    std::atomic<void *> g_callback_user{nullptr};

    const char *safe_message(const char *message_utf8)
    {
        return message_utf8 ? message_utf8 : "";
    }

#ifdef _WIN32
    std::string wide_to_utf8(const wchar_t *ws)
    {
        if (!ws)
            return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};
        std::string out(static_cast<size_t>(needed), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr) <= 0)
            return {};
        out.pop_back();
        return out;
    }
#endif
}

namespace gcap
{
    void log_message(gcap_log_level_t level, const char *message_utf8)
    {
        const char *msg = safe_message(message_utf8);
        if (auto cb = g_callback.load(std::memory_order_acquire))
            cb(level, msg, g_callback_user.load(std::memory_order_acquire));

#ifdef _WIN32
        OutputDebugStringA(msg);
        const size_t len = std::strlen(msg);
        if (len == 0 || msg[len - 1] != '\n')
            OutputDebugStringA("\n");
#endif
    }

    void log_message_w(gcap_log_level_t level, const wchar_t *message_wide)
    {
#ifdef _WIN32
        const std::string utf8 = wide_to_utf8(message_wide);
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

        char stackBuf[1024];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
        va_end(args);

        if (n < 0)
        {
            log_message(level, fmt);
            return;
        }
        if (static_cast<size_t>(n) < sizeof(stackBuf))
        {
            log_message(level, stackBuf);
            return;
        }

        std::vector<char> heapBuf(static_cast<size_t>(n) + 1u);
        va_start(args, fmt);
        std::vsnprintf(heapBuf.data(), heapBuf.size(), fmt, args);
        va_end(args);
        log_message(level, heapBuf.data());
    }
}

extern "C" GCAP_API void gcap_set_log_callback(gcap_log_callback_t cb, void *user)
{
    g_callback_user.store(user, std::memory_order_release);
    g_callback.store(cb, std::memory_order_release);
}
