#pragma once
#include <score_addon_videoio_export.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::Magewell
{

/// A Magewell capture channel as seen by the unified Direct Video I/O
/// enumerator. Magewell PCIe cards are capture-only (no playout), so canOutput
/// is always false.
struct DeviceInfo
{
  int index{};            ///< channel index passed to MWGetDevicePath
  std::string displayName;
  bool canInput{true};    ///< every enumerated capture channel is an input
  bool canOutput{false};  ///< Magewell has no playout
};

/// Initialize the MWCapture runtime exactly once (MWCaptureInitInstance).
/// Process-lifetime init; safe to call repeatedly. Returns false if the SDK
/// runtime/driver is missing.
SCORE_ADDON_VIDEOIO_EXPORT
bool ensureMwInit() noexcept;

/// Enumerate installed Magewell capture channels (PCIe only; USB skipped).
SCORE_ADDON_VIDEOIO_EXPORT
std::vector<DeviceInfo> enumerateDevices();

/// True iff channel `index` currently has a LOCKED incoming video signal.
/// Cheap open→query→close probe; used by harnesses to distinguish
/// "no cable / no source" (skip) from "signal present but capture broken"
/// (fail) before running the full graph.
SCORE_ADDON_VIDEOIO_EXPORT
bool signalLocked(int index) noexcept;

} // namespace Gfx::Magewell
