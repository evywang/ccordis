#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>

namespace ccordis {

namespace {
std::mutex g_mutex;
LogFn g_logger;   // empty → stderr default

void dispatch(const std::string &line)
{
    // Copy the target under the lock; invoke outside it so a logger may
    // itself call setLogger without deadlocking.
    LogFn target;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        target = g_logger;
    }
    if (target)
        target(line);
    else
        std::fprintf(stderr, "[ccordis] %s\n", line.c_str());
}
} // namespace

void setLogger(LogFn fn)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_logger = std::move(fn);
}

void log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dispatch(buf);
}

} // namespace ccordis
