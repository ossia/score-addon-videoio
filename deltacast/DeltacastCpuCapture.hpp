#pragma once
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

namespace Gfx::Deltacast
{

/// Host-staged capture: the VHD reception loop copies each arrived frame into
/// one of the ring slots; the render thread uploads the slot into the decoder's
/// input texture (portable QRhi path; works on every backend). The universal
/// fallback when no GPU-direct path is available.
struct DeltacastCpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "Deltacast-CPU";
};
using DeltacastCpuCapture
    = score::gfx::interop::CpuStagedCapture<DeltacastCpuPolicy>;

} // namespace Gfx::Deltacast
