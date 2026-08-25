#include "ArgusCaptureNode.hpp"

#include <memory>

namespace Gfx::Argus
{

ArgusCaptureNode::ArgusCaptureNode(const ArgusSettings& s)
    : settings{s}
{
  // Seed the shared block so a control read before anything opened reports the
  // configured value rather than a default.
  live->apply(s);
}

ArgusCaptureNode::~ArgusCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
ArgusCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<ArgusInputBackend>(settings, ring, live);
}

}
