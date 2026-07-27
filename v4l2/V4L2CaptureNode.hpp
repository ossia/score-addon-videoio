#pragma once
#include <v4l2/V4L2InputBackend.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::V4L2
{

/**
 * @brief V4L2 capture as a QRhi texture — thin wrapper over
 * DMACaptureInputNode supplying a V4L2InputBackend (host-staged, capture only).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT V4L2CaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit V4L2CaptureNode(const V4L2InputSettings& s);
  ~V4L2CaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  V4L2InputSettings settings;
};

} // namespace Gfx::V4L2
