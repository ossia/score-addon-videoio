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
#else
#include <DeckLinkAPI.h>
#endif
