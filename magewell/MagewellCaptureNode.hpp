#pragma once
#include <magewell/MagewellInputBackend.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::Magewell
{

/**
 * @brief Magewell capture as a QRhi texture — thin wrapper over
 * DMACaptureInputNode supplying a MagewellInputBackend (host-staged, capture
 * only; Magewell has no playout).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT MagewellCaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit MagewellCaptureNode(const MagewellInputSettings& s);
  ~MagewellCaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  MagewellInputSettings settings;
};

} // namespace Gfx::Magewell
