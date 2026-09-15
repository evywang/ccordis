#ifndef CCORDIS_PLUGINREGISTRY_H
#define CCORDIS_PLUGINREGISTRY_H

#include <ccordis/Context.h>
#include <ccordis/PluginManifest.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ccordis {

/**
 * @brief Scans directories for plugin manifests and loads them into a
 *        Context (P1 §3.3, v1.2).
 *
 * Lifecycle:
 *   1. addSearchPath("plugins/")   — register scan roots
 *   2. scan()                      — find manifest.json files (depth ≤ 2)
 *   3. loadAll(ctx) / load(ctx,n)  — load into the Context
 *
 * NOTE (v1.1, 审查 H2): 本类 **不做拓扑排序**。内核的 Deferred 机制已实现
 * "requires 缺失 → Deferred，服务出现时自启" 的完整依赖生命周期，比静态
 * 排序更强（能处理运行时才出现的服务）。loadAll 按扫描发现的任意顺序
 * 加载，依赖序由内核兜底。
 *
 * 所有权/生命周期纪律：
 *   - 库映射归 Context 所有（pluginFromLibrary → §7.3 退役纪律）；
 *   - 本类只持有 manifest 元数据，可在 Context 之前析构。
 *
 * Thread safety: host-thread only (同 Context)。
 */
class PluginRegistry
{
public:
    using LogFn = std::function<void(const std::string &)>;

    PluginRegistry() = default;
    ~PluginRegistry() = default;

    PluginRegistry(const PluginRegistry &) = delete;
    PluginRegistry &operator=(const PluginRegistry &) = delete;

    /** Add a directory to scan (recursive, max depth 2). */
    void addSearchPath(const std::string &path) { m_searchPaths.push_back(path); }

    /** Forget all discovered manifests (libraries stay with their Context). */
    void clear() { m_manifests.clear(); }

    /** Scan all search paths; @return number of valid manifests found. */
    std::size_t scan();

    /**
     * @brief Load all scanned plugins into ctx. 顺序无关：requires 未满足
     *        的插件由内核保持 Deferred，依赖上线后自启。
     * @return number of plugins successfully registered (含 Deferred)
     */
    std::size_t loadAll(Context &ctx);

    /**
     * @brief Load one plugin by name.
     *  - 已扫描 manifest → 动态 .so（optionsOverride 浅合并覆盖 manifest
     *    默认 options）；
     *  - 无 manifest → Context 工厂回退（loadRegistered, class-form 内置）；
     *  - 两者皆无 → false + lastError。
     * 重复加载同名 → 幂等成功（已注册即跳过）。
     */
    bool load(Context &ctx, const std::string &name,
              const Value &optionsOverride = Value());

    /** Admin-unload（服务下线 + 记录遗忘；库代码驻留，§7.3）。 */
    bool unload(Context &ctx, const std::string &name) { return ctx.unload(name); }

    const PluginManifest *manifest(const std::string &name) const;

    std::vector<PluginManifest> allManifests() const;

    std::size_t manifestCount() const { return m_manifests.size(); }

    /** Custom log sink (default: ccordis::log). */
    void setLogSink(const LogFn &sink) { m_log = sink; }

    /** Last error from scan/load operations (sticky until next op). */
    const std::string &lastError() const { return m_lastError; }

private:
    void scanDirectory(const std::string &dir, int depth);
    bool loadSingle(Context &ctx, const PluginManifest &m,
                    const Value &optionsOverride);
    void logLine(const std::string &line);

    std::vector<std::string> m_searchPaths;
    std::map<std::string, PluginManifest> m_manifests;   // name → manifest
    std::string m_lastError;
    LogFn m_log;
};

} // namespace ccordis

#endif // CCORDIS_PLUGINREGISTRY_H
