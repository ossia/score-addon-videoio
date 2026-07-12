#pragma once
#include <decklink/DeckLinkInputBackend.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::DeckLink
{

/**
 * @brief DeckLink SDI/HDMI capture as a QRhi texture.
 *
 * Thin vendor node over score::gfx::DMACaptureInputNode: the base owns the
 * renderer machinery (decoder + capture strategy + slot ring + sampling); this
 * class only supplies the DeckLink capture backend bound to the renderer's slot
 * ring. Host-staged today (DeckLinkCpuCapture); DVP GPU-direct is a later pass.
 */
struct SCORE_ADDON_VIDEOIO_EXPORT DeckLinkCaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit DeckLinkCaptureNode(const DeckLinkInputSettings& s);
  ~DeckLinkCaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  DeckLinkInputSettings settings;
};

} // namespace Gfx::DeckLink
