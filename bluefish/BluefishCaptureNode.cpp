#include "BluefishCaptureNode.hpp"

#include <bluefish/BluefishInputBackend.hpp>

#include <memory>

namespace Gfx::Bluefish
{

BluefishCaptureNode::BluefishCaptureNode(const BluefishInputSettings& s)
    : settings{s}
{
}

BluefishCaptureNode::~BluefishCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
BluefishCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<BluefishInputBackend>(settings, ring);
}

} // namespace Gfx::Bluefish
