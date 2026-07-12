#pragma once
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

namespace Gfx::Magewell
{

/// Host-staged capture: the MWCapture notify+capture loop copies each arrived
/// frame into one of the ring slots; the render thread uploads the slot into the
/// decoder's input texture (portable QRhi path; works on every backend). Magewell
/// has no GPU-direct path, so this is always the strategy.
struct MagewellCpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "Magewell-CPU";
};
using MagewellCpuCapture
    = score::gfx::interop::CpuStagedCapture<MagewellCpuPolicy>;

} // namespace Gfx::Magewell
