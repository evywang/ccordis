#ifndef CCORDIS_LOG_H
#define CCORDIS_LOG_H

#include <functional>
#include <string>

namespace ccordis {

/**
 * @brief Injectable logger — the kernel's only diagnostics channel
 *        (dependency red line §0: no Qt, no logging library).
 *
 * Hosts route this into their own sink:
 *   ccordis::setLogger([](const std::string &line) { qWarning() << line.c_str(); });
 * Default writes to stderr with a "[ccordis]" prefix.
 * Thread-safe: the swap is mutexed; the callback itself is invoked
 * unserialized (callbacks must be reentrant or externally synchronized).
 */
using LogFn = std::function<void(const std::string &line)>;

void setLogger(LogFn fn);              // nullptr restores the stderr default
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace ccordis

#endif // CCORDIS_LOG_H
