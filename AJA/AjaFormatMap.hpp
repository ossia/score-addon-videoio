#pragma once

/**
 * @file AjaFormatMap.hpp
 * @brief AJA NTV2FrameBufferFormat <-> vendor-neutral VideoPixelFormat.
 *
 * The neutral enum + the generic encoder factory
 * (Gfx/Graph/encoders/WireEncoderFactory.hpp) own the encoder selection; this
 * table is the only AJA-specific piece. Every value reproduces exactly the
 * encoder the hand-written switch used, so output stays byte-identical.
 *
 * Note: AJA's NTV2_FBF_* names denote the 32-bit word layout, not the in-memory
 * byte order the card's CSC reads. The neutral enum is by *memory* byte order,
 * which is why NTV2_FBF_ARGB maps to BGRA8 etc. (verified vs ntv2transcode.cpp).
 */

#include <AJA/AJAInput.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#include <ntv2enums.h>

#include <QHash>
#include <QString>

namespace Gfx::AJA
{

/// Settings video-format string (e.g. "1080p60", "UHD60", "8K_60") -> NTV2
/// video format. Falls back to 1080p59.94 on an unknown string.
inline NTV2VideoFormat
parseAjaVideoFormat(const QString& format, double /*rate*/) noexcept
{
  static const QHash<QString, NTV2VideoFormat> formatMap = {
      {"1080p2398", NTV2_FORMAT_1080p_2398},
      {"1080p24", NTV2_FORMAT_1080p_2400},
      {"1080p25", NTV2_FORMAT_1080p_2500},
      {"1080p2997", NTV2_FORMAT_1080p_2997},
      {"1080p30", NTV2_FORMAT_1080p_3000},
      {"1080p50", NTV2_FORMAT_1080p_5000_A},
      {"1080p5994", NTV2_FORMAT_1080p_5994_A},
      {"1080p60", NTV2_FORMAT_1080p_6000_A},
      {"1080i50", NTV2_FORMAT_1080i_5000},
      {"1080i5994", NTV2_FORMAT_1080i_5994},
      {"1080i60", NTV2_FORMAT_1080i_6000},
      {"720p50", NTV2_FORMAT_720p_5000},
      {"720p5994", NTV2_FORMAT_720p_5994},
      {"720p60", NTV2_FORMAT_720p_6000},
      // Quad-link 4K (4x 3G/HD-SDI cables, SQD or TSI).
      {"UHD2398", NTV2_FORMAT_4x1920x1080p_2398},
      {"UHD24", NTV2_FORMAT_4x1920x1080p_2400},
      {"UHD25", NTV2_FORMAT_4x1920x1080p_2500},
      {"UHD2997", NTV2_FORMAT_4x1920x1080p_2997},
      {"UHD50", NTV2_FORMAT_4x1920x1080p_5000},
      {"UHD5994", NTV2_FORMAT_4x1920x1080p_5994},
      {"UHD60", NTV2_FORMAT_4x1920x1080p_6000},
      // Neutral "2160pXX" tokens emitted by the unified Direct Video I/O widget
      // (vendor-agnostic spelling; DeckLink reads them as single-link 4K). For
      // AJA they alias the quad-link UHD formats — the topology the old AJA
      // widget used and that retail Kona firmware supports without a 12G profile.
      // The capability filter hides any a given card can't do.
      {"2160p2398", NTV2_FORMAT_4x1920x1080p_2398},
      {"2160p24", NTV2_FORMAT_4x1920x1080p_2400},
      {"2160p25", NTV2_FORMAT_4x1920x1080p_2500},
      {"2160p2997", NTV2_FORMAT_4x1920x1080p_2997},
      {"2160p30", NTV2_FORMAT_4x1920x1080p_3000},
      {"2160p50", NTV2_FORMAT_4x1920x1080p_5000},
      {"2160p5994", NTV2_FORMAT_4x1920x1080p_5994},
      {"2160p60", NTV2_FORMAT_4x1920x1080p_6000},
      // Single-link 4K UHD (one 12G cable; requires CanDo12gRouting).
      {"UHD_SL_2398", NTV2_FORMAT_3840x2160p_2398},
      {"UHD_SL_24", NTV2_FORMAT_3840x2160p_2400},
      {"UHD_SL_25", NTV2_FORMAT_3840x2160p_2500},
      {"UHD_SL_2997", NTV2_FORMAT_3840x2160p_2997},
      {"UHD_SL_30", NTV2_FORMAT_3840x2160p_3000},
      {"UHD_SL_50", NTV2_FORMAT_3840x2160p_5000},
      {"UHD_SL_5994", NTV2_FORMAT_3840x2160p_5994},
      {"UHD_SL_60", NTV2_FORMAT_3840x2160p_6000},
      // Single-link DCI 4K (one 12G cable).
      {"DCI_SL_2398", NTV2_FORMAT_4096x2160p_2398},
      {"DCI_SL_24", NTV2_FORMAT_4096x2160p_2400},
      {"DCI_SL_25", NTV2_FORMAT_4096x2160p_2500},
      {"DCI_SL_2997", NTV2_FORMAT_4096x2160p_2997},
      {"DCI_SL_30", NTV2_FORMAT_4096x2160p_3000},
      {"DCI_SL_4795", NTV2_FORMAT_4096x2160p_4795},
      {"DCI_SL_48", NTV2_FORMAT_4096x2160p_4800},
      {"DCI_SL_50", NTV2_FORMAT_4096x2160p_5000},
      {"DCI_SL_5994", NTV2_FORMAT_4096x2160p_5994},
      {"DCI_SL_60", NTV2_FORMAT_4096x2160p_6000},
      {"UHD2_2398", NTV2_FORMAT_4x3840x2160p_2398},
      {"UHD2_24", NTV2_FORMAT_4x3840x2160p_2400},
      {"UHD2_25", NTV2_FORMAT_4x3840x2160p_2500},
      {"UHD2_2997", NTV2_FORMAT_4x3840x2160p_2997},
      {"UHD2_30", NTV2_FORMAT_4x3840x2160p_3000},
      {"UHD2_50", NTV2_FORMAT_4x3840x2160p_5000},
      {"UHD2_5994", NTV2_FORMAT_4x3840x2160p_5994},
      {"UHD2_60", NTV2_FORMAT_4x3840x2160p_6000},
      {"8K_2398", NTV2_FORMAT_4x4096x2160p_2398},
      {"8K_24", NTV2_FORMAT_4x4096x2160p_2400},
      {"8K_25", NTV2_FORMAT_4x4096x2160p_2500},
      {"8K_2997", NTV2_FORMAT_4x4096x2160p_2997},
      {"8K_30", NTV2_FORMAT_4x4096x2160p_3000},
      {"8K_4795", NTV2_FORMAT_4x4096x2160p_4795},
      {"8K_48", NTV2_FORMAT_4x4096x2160p_4800},
      {"8K_50", NTV2_FORMAT_4x4096x2160p_5000},
      {"8K_5994", NTV2_FORMAT_4x4096x2160p_5994},
      {"8K_60", NTV2_FORMAT_4x4096x2160p_6000},
  };
  auto it = formatMap.find(format);
  return it != formatMap.end() ? it.value() : NTV2_FORMAT_1080p_5994_A;
}

/// Settings pixel-format string (e.g. "YCbCr10", "RGB8", "RGB12P") -> NTV2
/// framebuffer format. Falls back to v210 on an unknown string.
inline NTV2FrameBufferFormat parseAjaPixelFormat(const QString& format) noexcept
{
  if(format == "YCbCr10")      return NTV2_FBF_10BIT_YCBCR;
  if(format == "YCbCr8")       return NTV2_FBF_8BIT_YCBCR;
  if(format == "YCbCr10_422P") return NTV2_FBF_10BIT_YCBCR_422PL3_LE;
  if(format == "YCbCr8_422P")  return NTV2_FBF_8BIT_YCBCR_422PL3;
  if(format == "YCbCr8_420P")  return NTV2_FBF_8BIT_YCBCR_420PL3;
  if(format == "YCbCr10_420P") return NTV2_FBF_10BIT_YCBCR_420PL3_LE;
  if(format == "RGB10")        return NTV2_FBF_10BIT_RGB;
  if(format == "ARGB10")       return NTV2_FBF_10BIT_ARGB;
  if(format == "RGB10DPX")     return NTV2_FBF_10BIT_DPX;
  if(format == "RGB10DPXLE")   return NTV2_FBF_10BIT_DPX_LE;
  if(format == "RGB12")        return NTV2_FBF_48BIT_RGB;
  if(format == "RGB12P")       return NTV2_FBF_12BIT_RGB_PACKED;
  if(format == "RGB24")        return NTV2_FBF_24BIT_RGB;
  if(format == "BGR24")        return NTV2_FBF_24BIT_BGR;
  if(format == "RGB8")         return NTV2_FBF_ARGB;
  if(format == "RGBA8")        return NTV2_FBF_RGBA;
  if(format == "ABGR8")        return NTV2_FBF_ABGR;
  if(format == "YUY2")         return NTV2_FBF_8BIT_YCBCR_YUY2;
  return NTV2_FBF_10BIT_YCBCR;
}

/// AJA capture pixel format -> neutral wire format (for makeWireDecoder).
inline score::gfx::interop::VideoPixelFormat
ajaInputFormatTo(AJAInputPixelFormat fmt) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;
  switch(fmt)
  {
    case AJAInputPixelFormat::YCbCr8:  return F::UYVY422;
    case AJAInputPixelFormat::YCbCr10: return F::V210;
    case AJAInputPixelFormat::ARGB:    return F::BGRA8; // NTV2_FBF_ARGB: [B,G,R,A]
    case AJAInputPixelFormat::RGBA:    return F::RGBA8; // NTV2_FBF_ABGR: [R,G,B,A]
  }
  return F::Unknown;
}

inline score::gfx::interop::VideoPixelFormat
ntv2FormatTo(NTV2FrameBufferFormat fmt) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;
  switch(fmt)
  {
    case NTV2_FBF_8BIT_YCBCR:            return F::UYVY422;
    case NTV2_FBF_8BIT_YCBCR_YUY2:       return F::YUYV422;
    case NTV2_FBF_10BIT_YCBCR:           return F::V210;
    case NTV2_FBF_10BIT_YCBCR_422PL3_LE: return F::YUV422P10;
    case NTV2_FBF_8BIT_YCBCR_422PL3:     return F::YUV422P;
    case NTV2_FBF_8BIT_YCBCR_420PL3:     return F::YUV420P;
    case NTV2_FBF_10BIT_YCBCR_420PL3_LE: return F::YUV420P10;
    case NTV2_FBF_ARGB:                  return F::BGRA8; // memory [B,G,R,A]
    case NTV2_FBF_RGBA:                  return F::ARGB8; // memory [A,R,G,B]
    case NTV2_FBF_ABGR:                  return F::RGBA8; // memory [R,G,B,A]
    case NTV2_FBF_10BIT_RGB:             return F::R210;
    case NTV2_FBF_10BIT_ARGB:            return F::ARGB10;
    case NTV2_FBF_10BIT_DPX:             return F::DPX10;
    case NTV2_FBF_10BIT_DPX_LE:          return F::DPX10LE;
    case NTV2_FBF_48BIT_RGB:             return F::RGB48;
    case NTV2_FBF_12BIT_RGB_PACKED:      return F::RGB12P;
    case NTV2_FBF_24BIT_RGB:             return F::RGB24;
    case NTV2_FBF_24BIT_BGR:             return F::BGR24;
    default:                             return F::Unknown;
  }
}

} // namespace Gfx::AJA
