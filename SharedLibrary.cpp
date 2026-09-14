#include "SharedLibrary.h"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace ccordis {

SharedLibrary::~SharedLibrary()
{
    unload();
}

SharedLibrary::SharedLibrary(SharedLibrary &&other) noexcept
    : m_handle(other.m_handle), m_error(std::move(other.m_error))
{
    other.m_handle = nullptr;
}

SharedLibrary &SharedLibrary::operator=(SharedLibrary &&other) noexcept
{
    if (this != &other) {
        unload();
        m_handle = other.m_handle;
        m_error = std::move(other.m_error);
        other.m_handle = nullptr;
    }
    return *this;
}

bool SharedLibrary::load(const std::string &path)
{
    unload();
#ifdef _WIN32
    // UTF-8 → UTF-16; the CRT/Win32 loader is wide-char native.
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1,
                                           nullptr, 0);
    std::wstring wide(static_cast<size_t>(wlen > 0 ? wlen : 1), L'\0');
    if (wlen > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wlen);
    HMODULE h = ::LoadLibraryW(wide.c_str());
    m_handle = static_cast<void *>(h);
    if (!h)
        m_error = "LoadLibraryW failed, GetLastError=" + std::to_string(::GetLastError());
#else
    // RTLD_LAZY|RTLD_LOCAL — aligned with QLibrary's default (verified on
    // Qt 5.15.13, see selection report): symbols never leak into the
    // global namespace; cross-plugin lookups go through the kernel.
    m_handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!m_handle) {
        const char *err = ::dlerror();   // thread-local, cleared on next call
        m_error = err ? std::string(err) : "dlopen failed";
    }
#endif
    return m_handle != nullptr;
}

void *SharedLibrary::resolve(const char *symbol) const
{
    if (!m_handle || !symbol)
        return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void *>(::GetProcAddress(
        static_cast<HMODULE>(m_handle), symbol));
#else
    return ::dlsym(m_handle, symbol);
#endif
}

bool SharedLibrary::unload()
{
    if (!m_handle)
        return false;
#ifdef _WIN32
    const BOOL ok = ::FreeLibrary(static_cast<HMODULE>(m_handle));
    m_handle = nullptr;
    return ok != 0;
#else
    const int rc = ::dlclose(m_handle);
    m_handle = nullptr;
    return rc == 0;
#endif
}

} // namespace ccordis
