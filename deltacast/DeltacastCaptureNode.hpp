#pragma once
#include <deltacast/DeltacastInputBackend.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::Deltacast
{

/**
 * @brief DELTACAST capture as a QRhi texture — thin wrapper over
 * DMACaptureInputNode supplying a DeltacastInputBackend (host-staged v1).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT DeltacastCaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit DeltacastCaptureNode(const DeltacastInputSettings& s);
  ~DeltacastCaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  DeltacastInputSettings settings;
};

} // namespace Gfx::Deltacast
