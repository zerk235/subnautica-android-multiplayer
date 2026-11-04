#pragma once

#include <stdarg.h>

bool mod_log_open();
void mod_log_write(const char* level, const char* fmt, va_list args);
void mod_logf(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_INFO(...) mod_logf("I", __VA_ARGS__)
#define LOG_WARN(...) mod_logf("W", __VA_ARGS__)
#define LOG_ERROR(...) mod_logf("E", __VA_ARGS__)