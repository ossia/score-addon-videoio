#include "V4L2CaptureNode.hpp"

#include <memory>

namespace Gfx::V4L2
{

V4L2CaptureNode::V4L2CaptureNode(const V4L2InputSettings& s)
    : settings{s}
{
}

V4L2CaptureNode::~V4L2CaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
V4L2CaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<V4L2InputBackend>(settings, ring);
}

} // namespace Gfx::V4L2
