#pragma once
/**
 * @file Magewell.hpp
 * @brief Umbrella include for the Magewell Pro Capture SDK (LibMWCapture; pure C
 *        API, no codegen). Capture (input) only — Magewell PCIe cards have no
 *        playout, so there is no output backend.
 *
 * Windows: MWCapture.h already pulls in <Windows.h>, <ks.h>/<ksmedia.h> and
 * MWFOURCC.h; we include <windows.h> first anyway (CreateEvent /
 * WaitForSingleObject live there) and MWFOURCC.h explicitly for the FOURCC
 * constants + stride/size helpers (FOURCC_CalcMinStride / FOURCC_CalcImageSize).
 *
 * Linux: the SDK's own event primitives (MWCreateEvent / MWWaitEvent /
 * MWCloseEvent over /dev/mw-event) replace the Win32 auto-reset events, and
 * event handles are MWCAP_PTR rather than HANDLE. The `MwEvent` alias + the
 * mwEvent*() inline shims below are the single portability seam the input
 * backend uses; everything else in LibMWCapture is the same C API.
 */

#if defined(_WIN32)
#include <windows.h>

#include <LibMWCapture/MWCapture.h>

#include <MWFOURCC.h>

namespace Gfx::Magewell
{
using MwEvent = HANDLE;
inline constexpr HANDLE mwNoEvent = nullptr;

/// Auto-reset, initially unsignalled — matches the SDK notify/capture contract.
inline MwEvent mwEventCreate() noexcept
{
  return CreateEvent(nullptr, FALSE, FALSE, nullptr);
}
inline void mwEventClose(MwEvent h) noexcept
{
  CloseHandle(h);
}
/// True iff the event fired within `ms` milliseconds.
inline bool mwEventWait(MwEvent h, unsigned long ms) noexcept
{
  return WaitForSingleObject(h, ms) == WAIT_OBJECT_0;
}
/// MWPinVideoBuffer/MWUnpinVideoBuffer buffer argument (LPBYTE on Windows,
/// MWCAP_PTR address on Linux).
inline unsigned char* mwBufferAddress(unsigned char* p) noexcept
{
  return p;
}
/// Device-path character type: MWGetDevicePath / MWOpenChannelByPath take
/// WCHAR* on Windows and char* on Linux.
using MwPathChar = WCHAR;
} // namespace Gfx::Magewell

#else

// The Linux SDK's WinTypes.h re-typedefs Win32 look-alikes (HANDLE as void*,
// struct RECT, ...) that collide with the AJA SDK's own Linux compat typedefs
// when a TU sees both (the unified VideoInput/VideoOutput enumerators do).
// Shadow-rename the conflicting identifiers for the duration of the include —
// the MWCapture prototypes then use the renamed types consistently.
#define HANDLE MW_WINTYPES_HANDLE
#define RECT MW_WINTYPES_RECT
#define PRECT MW_WINTYPES_PRECT
#define LPRECT MW_WINTYPES_LPRECT

#include <LibMWCapture/MWCapture.h>

#include <MWFOURCC.h>

// WinTypes.h defines the Win32 scalar look-alikes as MACROS (BOOL, CHAR,
// TRUE, ...). Left defined, they poison every identifier of the same name in
// downstream headers (ossia's val_type enum has BOOL/CHAR/FLOAT members).
// The SDK prototypes above are already parsed, so undefine the poisonous
// ones; our own code uses plain C++ types instead. DWORD/BYTE/WORD must STAY
// defined — the MWFOURCC_* constants expand through them at every use site.
#undef HANDLE
#undef RECT
#undef PRECT
#undef LPRECT
#undef CHAR
#undef SHORT
#undef INT
#undef FLOAT
#undef LONG
#undef LONGLONG
#undef ULONGLONG
#undef BOOL
#undef TRUE
#undef FALSE
#undef VOID
#undef UINT
#undef ULONG
#undef USHORT
#undef LPDWORD

namespace Gfx::Magewell
{
using MwEvent = MWCAP_PTR;
inline constexpr MWCAP_PTR mwNoEvent = 0;

inline MwEvent mwEventCreate() noexcept
{
  return MWCreateEvent();
}
inline void mwEventClose(MwEvent h) noexcept
{
  MWCloseEvent(h);
}
inline bool mwEventWait(MwEvent h, unsigned long ms) noexcept
{
  return MWWaitEvent(h, int(ms)) > 0;
}
/// MWPinVideoBuffer/MWUnpinVideoBuffer buffer argument (LPBYTE on Windows,
/// MWCAP_PTR address on Linux).
inline MWCAP_PTR mwBufferAddress(unsigned char* p) noexcept
{
  return MWCAP_PTR(reinterpret_cast<uintptr_t>(p));
}
/// Device-path character type: MWGetDevicePath / MWOpenChannelByPath take
/// WCHAR* on Windows and char* on Linux.
using MwPathChar = char;
} // namespace Gfx::Magewell

#endif
