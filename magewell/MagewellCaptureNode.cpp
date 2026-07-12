#include "MagewellCaptureNode.hpp"

#include <memory>

namespace Gfx::Magewell
{

MagewellCaptureNode::MagewellCaptureNode(const MagewellInputSettings& s)
    : settings{s}
{
}

MagewellCaptureNode::~MagewellCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
MagewellCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<MagewellInputBackend>(settings, ring);
}

} // namespace Gfx::Magewell
