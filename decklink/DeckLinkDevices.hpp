#pragma once
#include <decklink/DeckLink.hpp>
#include <decklink/DeckLinkComPtr.hpp>

#include <score_addon_videoio_export.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::DeckLink
{

/// A DeckLink sub-device as seen by the unified Direct Video I/O enumerator.
struct DeviceInfo
{
  int index{};               ///< position in the iterator (stable per session)
  std::int64_t persistentId{}; ///< BMDDeckLinkPersistentID (stable across reboots)
  std::string displayName;
  bool canOutput{};
  bool canInput{};
};

/// CoInitializeEx for the calling thread. Safe to call repeatedly; tolerates an
/// already-initialised apartment. Returns false only on a hard failure.
SCORE_ADDON_VIDEOIO_EXPORT
bool ensureComInit() noexcept;

/// Enumerate all installed DeckLink sub-devices.
SCORE_ADDON_VIDEOIO_EXPORT
std::vector<DeviceInfo> enumerateDevices();

/// Open the device at iterator position `index` (or null if out of range).
SCORE_ADDON_VIDEOIO_EXPORT
ComPtr<IDeckLink> openDevice(int index);

} // namespace Gfx::DeckLink
