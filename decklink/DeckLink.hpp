#pragma once
// Single include point for the DeckLink SDK.
//
// Windows: the COM C++ header is generated from the SDK .idl by widl at build
// time (see CMakeLists.txt); it needs the Windows COM base types, so windows.h
// comes first.
//
// Linux: the SDK ships a ready C++ header (DeckLinkAPI.h + LinuxCOM.h) with
// the same interfaces; instances come from CreateDeckLinkIteratorInstance()
// (compiled in via DeckLinkAPIDispatch.cpp, which dlopens libDeckLinkAPI.so
// from Desktop Video at runtime).
#if defined(_WIN32)
#include <windows.h>

#include "DeckLinkAPI_h.h"

// IDeckLinkProfileAttributes::GetFlag takes BOOL* (int*) on Windows but bool*
// on Linux, so call sites need a type that follows the platform.
using dlbool_t = BOOL;
// IDeckLinkVideoBuffer::GetSize takes ULONGLONG* on Windows, uint64_t* on
// Linux — same width, distinct types, so an override must follow the platform.
using dlbuffersize_t = ULONGLONG;
#else
#include <DeckLinkAPI.h>

#include <cstdint>
#include <cstring>

// The Linux SDK's LinuxCOM.h provides HRESULT/REFIID but not the Windows
// helper types/functions the shared backend code uses.
using LONGLONG = int64_t;
using dlbool_t = bool;
using dlbuffersize_t = uint64_t;

inline bool IsEqualIID(REFIID a, REFIID b) noexcept
{
  return std::memcmp(&a, &b, sizeof(REFIID)) == 0;
}
#endif
