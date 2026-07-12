#include "DeckLinkCaptureNode.hpp"

#include <memory>

namespace Gfx::DeckLink
{

DeckLinkCaptureNode::DeckLinkCaptureNode(const DeckLinkInputSettings& s)
    : settings{s}
{
}

DeckLinkCaptureNode::~DeckLinkCaptureNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
DeckLinkCaptureNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<DeckLinkInputBackend>(settings, ring);
}

} // namespace Gfx::DeckLink
