#pragma once
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

namespace Gfx::DeckLink
{

/// Host-staged capture: the DeckLink callback copies each arrived frame into one
/// of the ring slots; the render thread uploads the slot into the decoder's
/// input texture (portable QRhi path; works on every backend). The universal
/// fallback when no GPU-direct path is available.
struct DeckLinkCpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "DeckLink-CPU";
};
using DeckLinkCpuCapture
    = score::gfx::interop::CpuStagedCapture<DeckLinkCpuPolicy>;

} // namespace Gfx::DeckLink
