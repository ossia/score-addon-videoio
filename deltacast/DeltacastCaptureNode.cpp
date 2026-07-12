#include "DeltacastCaptureNode.hpp"

#include <memory>

namespace Gfx::Deltacast
{

DeltacastCaptureNode::DeltacastCaptureNode(const DeltacastInputSettings& s)
    : settings{s}
{
}

DeltacastCaptureNode::~DeltacastCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
DeltacastCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<DeltacastInputBackend>(settings, ring);
}

} // namespace Gfx::Deltacast
