#ifndef CCORDIS_PLUGINMANIFEST_H
#define CCORDIS_PLUGINMANIFEST_H

#include <ccordis/Export.h>
#include <ccordis/Value.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ccordis {

/**
 * @brief Parsed representation of a plugin's manifest.json (P1 §3.2).
 *
 * 轻量 POD —— 只存活于扫描/加载期；运行时契约是 PluginDef/IPlugin。
 * 双源关系（审查 H7 + 版本规约 §1.2）：PluginDef（代码内宏）是 requires/
 * version 的唯一强制源；manifest 字段仅为扫描期展示，加载期矛盾 WARN
 * （code wins）。缺省 "abi" 字段 → 取当前 CCORDIS_ABI_VERSION（展示用途，
 * 真正的 ABI 判定永远用 .so 内嵌的 PluginDef::abiVersion）。
 */
struct PluginManifest
{
    std::string path;           // manifest.json 完整路径
    std::string dir;            // 所在目录（entry 相对此目录解析）
    std::string name;           // 插件 id（如 "view.spectrum"）
    std::string label;          // 人类可读名
    std::string version;        // 语义版本（展示源；默认 "0.0.0"）
    std::uint32_t abiVersion = 0;
    std::string entry;          // .so 文件名（相对 dir；禁止绝对路径与 ".."）
    std::vector<std::string> required;   // manifest 声明的依赖（展示源）
    std::vector<std::string> provides;   // 文档用途
    Value options;              // 默认 options（app.json 同名项可覆盖）

    bool isValid() const
    {
        return !name.empty() && !entry.empty() && abiVersion > 0;
    }
};

/**
 * @brief 从 manifest.json 文本解析。失败时返回 isValid()==false 且
 *        *error（若非空）带原因。
 */
PluginManifest parseManifestJson(const std::string &jsonText,
                                 std::string *error = nullptr);

} // namespace ccordis

#endif // CCORDIS_PLUGINMANIFEST_H
