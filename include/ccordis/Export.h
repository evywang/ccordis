#ifndef CCORDIS_EXPORT_H
#define CCORDIS_EXPORT_H

/**
 * @brief Symbol visibility macros — the only place the kernel touches
 *        export mechanics (dependency red line §0).
 *
 * CCORDIS_EXPORT    — mark the dynamic-plugin entry symbol.
 * CCORDIS_INTERFACE — mark cross-DSO service interface types so their
 *                    type_info is unique/interposable even when a plugin
 *                    is compiled with -fvisibility=hidden (design §7.2,
 *                    review F2; defensive on libstdc++, required on
 *                    implementations without the name-string fallback).
 */

/**
 * Kernel version + plugin ABI contract version (gap G1):
 *  - CCORDIS_VERSION      — kernel release string, surfaced via ccordis::version().
 *  - CCORDIS_ABI_VERSION  — bumped whenever kernel header layout/inline
 *    ABI changes (members added/removed, inline template bodies that embed
 *    into plugins). Hosts compare it against PluginDef::abiVersion at load
 *    and REJECT stale libraries with a clear log instead of crashing later
 *    (see §7.2 lesson: stale .so silently miswrote member slots).
 * Plugins must embed it via CORDIS_PLUGIN_DEF* macros — never hand-write
 * the descriptor.
 *
 * abi 2 (kernel 2.2.0): PluginDef gained a trailing `version` field
 * (plugin semantic version, P1 §4.5). The bump is mandatory — an abi-1
 * .so embeds a smaller static PluginDef, and the host must reject it via
 * the equality check BEFORE ever touching def->version (else OOB read).
 */
#define CCORDIS_VERSION "2.2.0"
#define CCORDIS_ABI_VERSION 2

#if defined(_WIN32)
#  if defined(CCORDIS_PLUGIN_BUILD)
#    define CCORDIS_EXPORT __declspec(dllexport)
#  else
#    define CCORDIS_EXPORT __declspec(dllimport)
#  endif
#  define CCORDIS_INTERFACE /* Windows: use __declspec at the interface header */
#else
#  define CCORDIS_EXPORT __attribute__((visibility("default")))
#  define CCORDIS_INTERFACE __attribute__((visibility("default")))
#endif

#endif // CCORDIS_EXPORT_H
