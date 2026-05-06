#pragma once

#include "gcapture.h"
#include <cstdarg>


namespace gcap
{
    void log_message(gcap_log_level_t level, const char *message_utf8);
    void log_message_w(gcap_log_level_t level, const wchar_t *message_wide);
    void log_printf(gcap_log_level_t level, const char *fmt, ...);
}

inline void gcap_log_trace(const char *message_utf8) { gcap::log_message(GCAP_LOG_TRACE, message_utf8); }
inline void gcap_log_debug(const char *message_utf8) { gcap::log_message(GCAP_LOG_DEBUG, message_utf8); }
inline void gcap_log_info(const char *message_utf8)  { gcap::log_message(GCAP_LOG_INFO,  message_utf8); }
inline void gcap_log_warn(const char *message_utf8)  { gcap::log_message(GCAP_LOG_WARN,  message_utf8); }
inline void gcap_log_error(const char *message_utf8) { gcap::log_message(GCAP_LOG_ERROR, message_utf8); }
inline void gcap_log_debug_w(const wchar_t *message_wide) { gcap::log_message_w(GCAP_LOG_DEBUG, message_wide); }
