#include <magewell/MagewellDevices.hpp>

#include <magewell/Magewell.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace Gfx::Magewell
{

bool ensureMwInit() noexcept
{
  // Process-lifetime init. MWCaptureInitInstance returns BOOL (TRUE on success);
  // a matching MWCaptureExitInstance is optional (the SDK cleans up at process
  // teardown), so we init once and never explicitly exit.
  static std::once_flag once;
  static std::atomic<bool> ok{false};
  std::call_once(once, [] { ok.store(MWCaptureInitInstance() != 0); });
  return ok.load();
}

std::vector<DeviceInfo> enumerateDevices()
{
  std::vector<DeviceInfo> out;
  if(!ensureMwInit())
    return out;

  // Refresh the device list, then walk every capture channel.
  MWRefreshDevice();
  const int count = MWGetChannelCount();
  for(int i = 0; i < count; ++i)
  {
    MWCAP_CHANNEL_INFO info{};
    if(MWGetChannelInfoByIndex(i, &info) != MW_SUCCEEDED)
      continue;

    // Host-staged Pro Capture (pin/notify) is a PCIe-only path; skip USB.
    if(info.wFamilyID == MW_FAMILY_ID_USB_CAPTURE)
      continue;

    DeviceInfo dev;
    dev.index = i;
    dev.canInput = true;
    dev.canOutput = false;

    std::string name(info.szProductName);
    // Multi-channel cards (Quad HDMI, ...) enumerate as N identical products;
    // distinguish them by the card's own channel index (1-based, matching
    // Magewell's utilities and the bracket labels).
    name += " — input " + std::to_string(int(info.byChannelIndex) + 1);
    std::string serial(info.szBoardSerialNo);
    while(!serial.empty() && (serial.back() == ' ' || serial.back() == '\0'))
      serial.pop_back();
    if(!serial.empty())
      name += " (" + serial + ")";
    dev.displayName = name.empty() ? ("Magewell channel " + std::to_string(i))
                                   : std::move(name);

    out.push_back(std::move(dev));
  }
  return out;
}

bool signalLocked(int index) noexcept
{
  if(!ensureMwInit())
    return false;

  char path[256]{};
  if(MWGetDevicePath(index, path) != MW_SUCCEEDED)
    return false;
  HCHANNEL h = MWOpenChannelByPath(path);
  if(!h)
    return false;

  MWCAP_VIDEO_SIGNAL_STATUS status{};
  const bool locked = MWGetVideoSignalStatus(h, &status) == MW_SUCCEEDED
                      && status.state == MWCAP_VIDEO_SIGNAL_LOCKED;
  MWCloseChannel(h);
  return locked;
}

} // namespace Gfx::Magewell
