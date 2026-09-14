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
