#include "V4L2SyncRig.hpp"

#include <map>
#include <mutex>

namespace Gfx::V4L2
{

std::shared_ptr<score::gfx::interop::CaptureCorrelator>
acquireSyncRig(const std::string& name, std::size_t memberCount)
{
  if(name.empty())
    return {};

  static std::mutex mutex;
  static std::map<
      std::string, std::weak_ptr<score::gfx::interop::CaptureCorrelator>>
      rigs;

  std::lock_guard lock{mutex};

  // Weak, so the correlator's lifetime is the union of its members' and a rig
  // that stops leaves nothing for the next one under the same name to inherit.
  auto& entry = rigs[name];
  if(auto existing = entry.lock())
    return existing;

  auto fresh
      = std::make_shared<score::gfx::interop::CaptureCorrelator>(memberCount);
  entry = fresh;
  return fresh;
}

} // namespace Gfx::V4L2
