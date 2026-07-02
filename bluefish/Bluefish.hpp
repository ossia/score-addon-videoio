#pragma once
/**
 * @file Bluefish.hpp
 * @brief Umbrella include for the Bluefish444 BlueVelvetC SDK (pure C API + lib;
 *        no codegen). Windows-focused, like the DeckLink / Deltacast backends.
 */

#if !defined(__linux__) && !defined(__APPLE__)
#include <windows.h>
#endif

#include <BlueVelvetC.h>
#include <BlueVelvetCConversion.h>
#include <BlueVelvetCUtils.h>
