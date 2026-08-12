#include "ArgusCaptureNode.hpp"

#include <memory>

namespace Gfx::Argus
{

ArgusCaptureNode::ArgusCaptureNode(const ArgusSettings& s)
    : settings{s}
{
}

ArgusCaptureNode::~ArgusCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
ArgusCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<ArgusInputBackend>(settings, ring);
}

}
