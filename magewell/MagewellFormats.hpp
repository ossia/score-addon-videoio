#pragma once
#include <magewell/Magewell.hpp>

#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#include <QString>

#include <cstdint>

namespace Gfx::Magewell
{

/// Pixel-format token (from the settings widget) -> MWCapture FOURCC.
/// Defaults to 8-bit packed UYVY (the universal SDI/HDMI capture format).
inline std::uint32_t fourccFromToken(const QString& p) noexcept
{
  if(p == "V210") return MWFOURCC_V210; // 10-bit YUV 4:2:2
  if(p == "BGRA") return MWFOURCC_BGRA; // 32-bit packed BGRA
  if(p == "RGBA") return MWFOURCC_RGBA; // 32-bit packed RGBA
  return MWFOURCC_UYVY;                 // "UYVY" + default (8-bit YUV 4:2:2)
}

/// MWCapture FOURCC -> neutral wire format (for makeWireDecoder).
inline score::gfx::interop::VideoPixelFormat
neutralFromFourcc(std::uint32_t fourcc) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;
  switch(fourcc)
  {
    case MWFOURCC_UYVY: return F::UYVY422;
    case MWFOURCC_V210: return F::V210;
    case MWFOURCC_BGRA: return F::BGRA8;
    case MWFOURCC_RGBA: return F::RGBA8;
    default:            return F::UYVY422;
  }
}

/// Minimum row stride in bytes for @p fourcc at @p width. Uses the SDK's own
/// raster rules (V210's 48-pixel packing, etc). 2-byte alignment matches the
/// MWCapture examples.
inline std::uint32_t strideFromFourcc(std::uint32_t fourcc, int width) noexcept
{
  return FOURCC_CalcMinStride(fourcc, width, 2);
}

/// Total byte size of one @p fourcc frame at @p width x @p height for a given
/// @p stride (from strideFromFourcc).
inline std::uint32_t imageSizeFromFourcc(
    std::uint32_t fourcc, int width, int height, std::uint32_t stride) noexcept
{
  return FOURCC_CalcImageSize(fourcc, width, height, stride);
}

} // namespace Gfx::Magewell
