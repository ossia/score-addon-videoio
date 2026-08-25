#pragma once
#include <argus/ArgusInputBackend.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::Argus
{

/**
 * @brief Argus camera capture as a QRhi texture — thin wrapper over
 * DMACaptureInputNode supplying an ArgusInputBackend.
 */
struct SCORE_ADDON_VIDEOIO_EXPORT ArgusCaptureNode final
    : score::gfx::DMACaptureInputNode
{
  explicit ArgusCaptureNode(const ArgusSettings& s);
  ~ArgusCaptureNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  /// Shared with the device's `controls/` group. Created here because the node
  /// outlives every backend it makes, and the tree has to keep writing across
  /// a stop/start.
  std::shared_ptr<ArgusLiveControls> live{std::make_shared<ArgusLiveControls>()};

  ArgusSettings settings;
};

}
