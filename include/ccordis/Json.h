#ifndef CCORDIS_JSON_H
#define CCORDIS_JSON_H

#include <ccordis/Value.h>

#include <string>

namespace ccordis {

/**
 * @brief Minimal UTF-8 JSON → Value parser (host-side convenience module).
 *
 * The kernel red line (§0) bans Qt/logging/OS in kernel headers; a JSON
 * reader is pure std:: and complements the JSON-shaped Value (whose own
 * comment defers *parsing* to host adapters — PluginRegistry/manifest
 * loading is that host, and it must stay Qt-free because it links into
 * Qt-less libccordis.so). Qt hosts may keep using QJson via adapters/qt.h;
 * both produce the same Value.
 *
 * Supported: objects (insertion-order preserving), arrays, strings with
 * standard escapes incl. \uXXXX (surrogate pairs → UTF-8), numbers
 * (int64 when integral, double otherwise), true/false/null.
 * Not supported (by design): comments, trailing commas, NaN/Infinity.
 * Nesting is capped (kMaxDepth) so hostile input cannot overflow the stack.
 */
bool parseJson(const std::string &text, Value &out, std::string *error = nullptr);

/** Read a whole file (binary-safe). ok=false when the file can't be opened. */
bool readFileUtf8(const std::string &path, std::string &out);

} // namespace ccordis

#endif // CCORDIS_JSON_H
