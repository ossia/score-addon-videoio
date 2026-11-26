#pragma once

#include <linux/videodev2.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cstdint>

namespace Gfx::V4L2
{

struct Colorimetry
{
  AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
  AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
};

/// Resolves the four fields of a granted v4l2_pix_format the way
/// videodev2.h's V4L2_MAP_*_DEFAULT macros define them.
inline Colorimetry colorimetryFromV4L2(
    std::uint32_t colorspace, std::uint32_t ycbcrEnc, std::uint32_t quantization,
    std::uint32_t xferFunc, bool isRgb) noexcept
{
  Colorimetry c;
  if(colorspace == V4L2_COLORSPACE_DEFAULT)
    return c;

  if(ycbcrEnc == V4L2_YCBCR_ENC_DEFAULT)
    ycbcrEnc = V4L2_MAP_YCBCR_ENC_DEFAULT(colorspace);
  if(xferFunc == V4L2_XFER_FUNC_DEFAULT)
    xferFunc = V4L2_MAP_XFER_FUNC_DEFAULT(colorspace);
  if(quantization == V4L2_QUANTIZATION_DEFAULT)
    quantization = V4L2_MAP_QUANTIZATION_DEFAULT(isRgb, colorspace, ycbcrEnc);

  switch(colorspace)
  {
    case V4L2_COLORSPACE_SMPTE170M:
      c.primaries = AVCOL_PRI_SMPTE170M;
      break;
    case V4L2_COLORSPACE_470_SYSTEM_BG:
      c.primaries = AVCOL_PRI_BT470BG;
      break;
    case V4L2_COLORSPACE_470_SYSTEM_M:
      c.primaries = AVCOL_PRI_BT470M;
      break;
    case V4L2_COLORSPACE_SMPTE240M:
      c.primaries = AVCOL_PRI_SMPTE240M;
      break;
    case V4L2_COLORSPACE_REC709:
    case V4L2_COLORSPACE_SRGB:
    case V4L2_COLORSPACE_JPEG:
      c.primaries = AVCOL_PRI_BT709;
      break;
    case V4L2_COLORSPACE_BT2020:
      c.primaries = AVCOL_PRI_BT2020;
      break;
    case V4L2_COLORSPACE_DCI_P3:
      c.primaries = AVCOL_PRI_SMPTE431;
      break;
    default:
      break;
  }

  switch(ycbcrEnc)
  {
    case V4L2_YCBCR_ENC_601:
    case V4L2_YCBCR_ENC_XV601:
      c.space = AVCOL_SPC_SMPTE170M;
      break;
    case V4L2_YCBCR_ENC_709:
    case V4L2_YCBCR_ENC_XV709:
      c.space = AVCOL_SPC_BT709;
      break;
    case V4L2_YCBCR_ENC_BT2020:
      c.space = AVCOL_SPC_BT2020_NCL;
      break;
    case V4L2_YCBCR_ENC_BT2020_CONST_LUM:
      c.space = AVCOL_SPC_BT2020_CL;
      break;
    case V4L2_YCBCR_ENC_SMPTE240M:
      c.space = AVCOL_SPC_SMPTE240M;
      break;
    default:
      break;
  }

  switch(xferFunc)
  {
    case V4L2_XFER_FUNC_709:
      c.trc = AVCOL_TRC_BT709;
      break;
    case V4L2_XFER_FUNC_SRGB:
      c.trc = AVCOL_TRC_IEC61966_2_1;
      break;
    case V4L2_XFER_FUNC_OPRGB:
      c.trc = AVCOL_TRC_GAMMA22;
      break;
    case V4L2_XFER_FUNC_SMPTE240M:
      c.trc = AVCOL_TRC_SMPTE240M;
      break;
    case V4L2_XFER_FUNC_NONE:
      c.trc = AVCOL_TRC_LINEAR;
      break;
    case V4L2_XFER_FUNC_SMPTE2084:
      c.trc = AVCOL_TRC_SMPTE2084;
      break;
    default:
      break;
  }

  c.range = quantization == V4L2_QUANTIZATION_FULL_RANGE ? AVCOL_RANGE_JPEG
                                                          : AVCOL_RANGE_MPEG;
  return c;
}

}
