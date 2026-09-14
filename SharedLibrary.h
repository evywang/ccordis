#ifndef CCORDIS_SHAREDLIBRARY_H
#define CCORDIS_SHAREDLIBRARY_H

#include <string>

namespace ccordis {

/**
 * @brief Minimal dlopen/LoadLibrary wrapper — the kernel's ONLY OS contact
 *        point (dependency red line §0). windows.h/dlfcn.h never appear in
 *        this header; the handle is opaque.
 *
 * Semantics deliberately match QLibrary/Boost.DLL shared_library so hosts
 * can swap either in one line (see design §7.6 interop rules):
 *   - load()   RTLD_LAZY|RTLD_LOCAL on Unix (aligned with QLibrary default)
 *   - resolve() raw symbol address, nullptr when missing
 *   - unload() one-shot; single-owner discipline is enforced by Context,
 *     which unloads only at host-scope teardown (design §7.3)
 */
class SharedLibrary
{
public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(const SharedLibrary &) = delete;
    SharedLibrary &operator=(const SharedLibrary &) = delete;
    SharedLibrary(SharedLibrary &&other) noexcept;
    SharedLibrary &operator=(SharedLibrary &&other) noexcept;

    /** @param path UTF-8; canonical/absolute paths keep OS refcounting
     *  deterministic when other loaders touch the same file. */
    bool load(const std::string &path);
    void *resolve(const char *symbol) const;
    bool unload();
    bool isLoaded() const { return m_handle != nullptr; }

    /** Last OS error text (dlerror/GetLastError captured immediately). */
    std::string errorString() const { return m_error; }

private:
    void *m_handle = nullptr;
    std::string m_error;
};

} // namespace ccordis

#endif // CCORDIS_SHAREDLIBRARY_H
