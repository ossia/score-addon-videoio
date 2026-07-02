#pragma once
#include <bluefish/Bluefish.hpp>

#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#include <QString>

#include <cstdint>

namespace Gfx::Bluefish
{

/// Neutral video-format token ("1080p5994", "2160p60", ...) -> EVideoModeExt.
/// Fractional (…97 / …94) and integer rates are distinct Bluefish enums.
/// Defaults to 1080p5994 for unknown tokens. (SDK-internal; the SDK-free
/// wrapper videoModeExtFromToken() in BluefishSettings.hpp forwards to this.)
inline BLUE_U32 videoModeExtFromTokenBlue(const QString& f) noexcept
{
  if(f == "720p50")                      return VID_FMT_EXT_720P_5000;
  if(f == "720p5994")                    return VID_FMT_EXT_720P_5994;
  if(f == "720p60")                      return VID_FMT_EXT_720P_6000;

  if(f == "1080i50")                     return VID_FMT_EXT_1080I_5000;
  if(f == "1080i5994")                   return VID_FMT_EXT_1080I_5994;
  if(f == "1080i60")                     return VID_FMT_EXT_1080I_6000;

  if(f == "1080p2398")                   return VID_FMT_EXT_1080P_2398;
  if(f == "1080p24")                     return VID_FMT_EXT_1080P_2400;
  if(f == "1080p25")                     return VID_FMT_EXT_1080P_2500;
  if(f == "1080p2997")                   return VID_FMT_EXT_1080P_2997;
  if(f == "1080p30")                     return VID_FMT_EXT_1080P_3000;
  if(f == "1080p50")                     return VID_FMT_EXT_1080P_5000;
  if(f == "1080p5994" || f == "1080p")   return VID_FMT_EXT_1080P_5994;
  if(f == "1080p60")                     return VID_FMT_EXT_1080P_6000;

  if(f == "2160p2398")                   return VID_FMT_EXT_2160P_2398;
  if(f == "2160p24")                     return VID_FMT_EXT_2160P_2400;
  if(f == "2160p25")                     return VID_FMT_EXT_2160P_2500;
  if(f == "2160p2997")                   return VID_FMT_EXT_2160P_2997;
  if(f == "2160p30")                     return VID_FMT_EXT_2160P_3000;
  if(f == "2160p50")                     return VID_FMT_EXT_2160P_5000;
  if(f == "2160p5994")                   return VID_FMT_EXT_2160P_5994;
  if(f == "2160p60")                     return VID_FMT_EXT_2160P_6000;

  return VID_FMT_EXT_1080P_5994;
}

/// Pixel-format token -> EMemoryFormat. Defaults to 8-bit YUV 4:2:2 (YUVS/BV8).
///   YCbCr8  -> MEM_FMT_YUVS (== MEM_FMT_BV8), memory Y0 Cb Y1 Cr = YUY2/YUYV
///              (UYVY is the *separate* MEM_FMT_2VUY — see the SDK's
///               InitBufferYUVS vs InitBuffer2VUY in SampleCodeUtils.h)
///   YCbCr10 -> MEM_FMT_V210,                  on-wire V210
///   RGBA    -> MEM_FMT_RGBA,                  on-wire RGBA8
///   RGB8    -> MEM_FMT_ARGB_PC (== MEM_FMT_BGRA), on-wire BGRA8 (UI "BGRA 8-bit")
inline BLUE_U32 memFmtFromTokenBlue(const QString& p) noexcept
{
  if(p == "YCbCr10")              return MEM_FMT_V210;
  if(p == "RGBA")                 return MEM_FMT_RGBA;
  if(p == "RGB8" || p == "BGRA8") return MEM_FMT_ARGB_PC;
  return MEM_FMT_YUVS; // "YCbCr8" + default (also unknown, e.g. RGB10)
}

/// EMemoryFormat -> neutral wire format (for makeWireEncoder / makeWireDecoder).
inline score::gfx::interop::VideoPixelFormat
neutralFromMemFmt(BLUE_U32 memFmt) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;
  switch(memFmt)
  {
    case MEM_FMT_V210:    return F::V210;
    case MEM_FMT_YUVS:    return F::YUYV422; // YUVS/BV8 is Y0 Cb Y1 Cr (YUY2), NOT UYVY
    case MEM_FMT_2VUY:    return F::UYVY422;
    case MEM_FMT_RGBA:    return F::RGBA8;
    case MEM_FMT_ARGB_PC: return F::BGRA8;   // MEM_FMT_BGRA aliases MEM_FMT_ARGB_PC
    default:              return F::YUYV422;
  }
}

} // namespace Gfx::Bluefish
