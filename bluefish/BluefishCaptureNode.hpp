#pragma once
#include <bluefish/BluefishSettings.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::Bluefish
{

/**
 * @brief Bluefish444 capture as a QRhi texture — thin wrapper over
 * DMACaptureInputNode supplying a BluefishInputBackend (host-staged v1).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT BluefishCaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit BluefishCaptureNode(const BluefishInputSettings& s);
  ~BluefishCaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  BluefishInputSettings settings;
};

} // namespace Gfx::Bluefish
