#pragma once
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

namespace Gfx::Bluefish
{

/// Host-staged capture: the AutoCapture reception loop copies each arrived frame
/// into one of the ring slots; the render thread uploads the slot into the
/// decoder's input texture (portable QRhi path; works on every backend). The
/// universal fallback when no GPU-direct path is available.
struct BluefishCpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "Bluefish-CPU";
};
using BluefishCpuCapture
    = score::gfx::interop::CpuStagedCapture<BluefishCpuPolicy>;

} // namespace Gfx::Bluefish
