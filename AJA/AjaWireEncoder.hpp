#pragma once
#include <Gfx/Graph/encoders/WireEncoderFactory.hpp>

#include <AJA/AjaFormatMap.hpp>

#include <ntv2enums.h>

#include <memory>

namespace Gfx::AJA
{

/// Compute-shader encoder factory keyed by the AJA pixel format.
/// Thin AJA wrapper over the vendor-neutral factory.
inline std::unique_ptr<score::gfx::ComputeEncoder>
ajaMakeWireEncoder(NTV2FrameBufferFormat fmt)
{
  return score::gfx::makeWireComputeEncoder(ntv2FormatTo(fmt));
}

/// Returns true when (format, width) is something the tier-3 compute
/// encoders can handle: v210 needs width % 6 == 0; UYVY needs width % 2 == 0;
/// BGRA has no constraint.
inline bool ajaWireEncoderSupports(NTV2FrameBufferFormat fmt, int width)
{
  return score::gfx::wireComputeSupports(ntv2FormatTo(fmt), width);
}

} // namespace Gfx::AJA
