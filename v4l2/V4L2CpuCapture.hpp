#pragma once
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

namespace Gfx::V4L2
{

/// Host-staged capture: the V4L2 loop copies each dequeued frame into a ring
/// slot and the render thread uploads it. On Vulkan the shared implementation
/// engages the host-import rung by itself, so the slot pages are imported once
/// and the per-frame staging copy disappears.
struct V4L2CpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "V4L2-CPU";
};
using V4L2CpuCapture = score::gfx::interop::CpuStagedCapture<V4L2CpuPolicy>;

} // namespace Gfx::V4L2
