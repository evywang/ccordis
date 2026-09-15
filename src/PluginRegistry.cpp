#include <ccordis/PluginRegistry.h>
#include <ccordis/Json.h>
#include <ccordis/Log.h>

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace ccordis {

namespace {

constexpr int kMaxScanDepth = 2;
const char kManifestName[] = "manifest.json";

bool isDirectory(const std::string &path)
{
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool isRegularFile(const std::string &path)
{
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string joinPath(const std::string &a, const std::string &b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    if (a.back() == '/' || a.back() == '\\')
        return a + b;
    return a + "/" + b;
}

// 目录枚举：对每个 entry 调 fn(name, isDir)。false 返回值中止遍历。
template <typename Fn>
bool listDirectory(const std::string &dir, Fn &&fn)
{
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        const char *n = fd.cFileName;
        if (std::strcmp(n, ".") == 0 || std::strcmp(n, "..") == 0)
            continue;
        if (!fn(n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
            break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return false;
    while (struct dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (std::strcmp(n, ".") == 0 || std::strcmp(n, "..") == 0)
            continue;
        bool isDir = false;
        const std::string full = joinPath(dir, n);
        struct stat st;
        isDir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        if (!fn(n, isDir))
            break;
    }
    ::closedir(d);
    return true;
#endif
}

} // namespace

void PluginRegistry::logLine(const std::string &line)
{
    if (m_log)
        m_log(line);
    else
        ccordis::log("%s", line.c_str());
}

std::size_t PluginRegistry::scan()
{
    m_lastError.clear();
    m_manifests.clear();
    for (const std::string &root : m_searchPaths)
        scanDirectory(root, 0);
    return m_manifests.size();
}

void PluginRegistry::scanDirectory(const std::string &dir, int depth)
{
    if (depth > kMaxScanDepth)
        return;

    const std::string manifestPath = joinPath(dir, kManifestName);
    if (isRegularFile(manifestPath)) {
        std::string text;
        std::string err;
        if (!readFileUtf8(manifestPath, text)) {
            m_lastError = "cannot read " + manifestPath;
            logLine("WARN: PluginRegistry: " + m_lastError);
            return;
        }
        PluginManifest m = parseManifestJson(text, &err);
        m.path = manifestPath;
        m.dir = dir;
        if (!m.isValid()) {
            m_lastError = "invalid manifest at " + dir
                        + (err.empty() ? "" : (": " + err));
            logLine("WARN: PluginRegistry: " + m_lastError);
            return;
        }
        const auto it = m_manifests.find(m.name);
        if (it != m_manifests.end()) {
            logLine("WARN: PluginRegistry: duplicate plugin name '" + m.name
                    + "' (" + it->second.path + " kept, " + manifestPath
                    + " ignored)");
            return;
        }
        m_manifests.emplace(m.name, std::move(m));
        return;   // 含 manifest 的目录不再下钻
    }

    if (depth == kMaxScanDepth)
        return;   // 叶子深度只认 manifest 文件
    listDirectory(dir, [this, &dir, depth](const char *name, bool isDir) {
        if (isDir)
            scanDirectory(joinPath(dir, name), depth + 1);
        return true;
    });
}

bool PluginRegistry::loadSingle(Context &ctx, const PluginManifest &m,
                                const Value &optionsOverride)
{
    // 幂等：同名已注册（任意形态）即跳过 —— 防重复 record（first-provider-
    // wins 语义下重复 provide 会互相踩踏，见契约 §8）。
    if (ctx.pluginState(m.name) != Context::State::Disposed) {
        logLine("PluginRegistry: '" + m.name + "' already registered — skip");
        return true;
    }

    const std::string soPath = joinPath(m.dir, m.entry);
    const Value opts = shallowMerged(m.options, optionsOverride);
    if (!ctx.pluginFromLibrary(soPath, opts)) {
        m_lastError = "failed to load plugin '" + m.name + "' from " + soPath;
        logLine("ERROR: PluginRegistry: " + m_lastError);
        return false;
    }

    // 双源一致性校验（审查 H7 + 版本规约 §1.2）：代码（PluginDef）为准。
    if (m.version != "0.0.0") {
        const std::string codeVer = ctx.pluginVersion(m.name);
        if (codeVer != m.version) {
            logLine("WARN: PluginRegistry: manifest/code version mismatch for '"
                    + m.name + "' (" + m.version + " vs " + codeVer
                    + ") — code wins");
        }
    }
    if (ctx.pluginRequires(m.name) != m.required) {
        logLine("WARN: PluginRegistry: manifest/code requires mismatch for '"
                + m.name + "' — code wins");
    }
    return true;
}

std::size_t PluginRegistry::loadAll(Context &ctx)
{
    m_lastError.clear();
    std::size_t ok = 0;
    for (const auto &kv : m_manifests) {   // 任意顺序（map 字典序），Deferred 兜底
        if (loadSingle(ctx, kv.second, Value()))
            ++ok;
    }
    return ok;
}

bool PluginRegistry::load(Context &ctx, const std::string &name,
                          const Value &optionsOverride)
{
    m_lastError.clear();
    const auto it = m_manifests.find(name);
    if (it != m_manifests.end())
        return loadSingle(ctx, it->second, optionsOverride);

    // 无 manifest → class-form 工厂回退
    if (ctx.pluginState(name) != Context::State::Disposed)
        return true;   // 已注册，幂等
    if (!ctx.loadRegistered(name, optionsOverride)) {
        m_lastError = "unknown plugin '" + name
                    + "' (no scanned manifest, no registered factory)";
        logLine("ERROR: PluginRegistry: " + m_lastError);
        return false;
    }
    return true;
}

const PluginManifest *PluginRegistry::manifest(const std::string &name) const
{
    const auto it = m_manifests.find(name);
    return it == m_manifests.end() ? nullptr : &it->second;
}

std::vector<PluginManifest> PluginRegistry::allManifests() const
{
    std::vector<PluginManifest> out;
    out.reserve(m_manifests.size());
    for (const auto &kv : m_manifests)
        out.push_back(kv.second);
    return out;
}

} // namespace ccordis
