#pragma once
#include <AJA/AJAInput.hpp>
#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Gfx::AJA
{

/**
 * @brief AJA SDI capture as a QRhi texture — GPU-direct path.
 *
 * Thin vendor node over the shared `score::gfx::DMACaptureInputNode`: all the
 * renderer machinery (decoder + capture strategy + slot ring + sampling) lives
 * in the base; this class only supplies the AJA capture backend (card open,
 * AutoCirculate capture thread, AJA strategy selection, decoder choice).
 *
 * Created only when the device dispatch selects the GPU-direct path; the
 * CPU/AVFrame path lives on the device side.
 */
struct SCORE_ADDON_VIDEOIO_EXPORT AJAInputNode final
    : score::gfx::DMACaptureInputNode
{
  explicit AJAInputNode(const AJAInputSettings& s);
  ~AJAInputNode() override;

  std::unique_ptr<score::gfx::DMACaptureBackend> makeCaptureBackend(
      score::gfx::interop::VideoCaptureSlotRing& ring) const override;

  AJAInputSettings settings;
};

} // namespace Gfx::AJA
