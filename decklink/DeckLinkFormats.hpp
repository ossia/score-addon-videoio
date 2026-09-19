#pragma once
#include <decklink/DeckLink.hpp>

#include <Video/VideoPixelFormat.hpp>

namespace Gfx::DeckLink
{

/// BMDPixelFormat -> neutral wire format. Every format DeckLink transmits maps
/// 1:1 to an existing score encoder/decoder (verified in MULTIVENDOR_IO_MAP.md);
/// returns Unknown for the encoded (H.265/DNxHR) and rarely-used (Ay10/R10b)
/// formats we don't drive yet.
inline Video::VideoPixelFormat
toNeutralFormat(BMDPixelFormat fmt) noexcept
{
  using F = Video::VideoPixelFormat;
  switch(fmt)
  {
    case bmdFormat8BitYUV:    return F::UYVY422;  // '2vuy'
    case bmdFormat10BitYUV:   return F::V210;     // 'v210'
    case bmdFormat8BitBGRA:   return F::BGRA8;
    case bmdFormat8BitARGB:   return F::ARGB8;
    case bmdFormat10BitRGB:   return F::R210;     // 'r210'
    case bmdFormat12BitRGB:   return F::R12B;     // 'R12B'
    case bmdFormat12BitRGBLE: return F::R12L;     // 'R12L'
    default:                  return F::Unknown;
  }
}

/// Neutral wire format -> BMDPixelFormat. 0 (not a valid BMDPixelFormat) when
/// DeckLink has no matching on-wire format.
inline BMDPixelFormat fromNeutralFormat(
    Video::VideoPixelFormat fmt) noexcept
{
  using F = Video::VideoPixelFormat;
  switch(fmt)
  {
    case F::UYVY422: return bmdFormat8BitYUV;
    case F::V210:    return bmdFormat10BitYUV;
    case F::BGRA8:   return bmdFormat8BitBGRA;
    case F::ARGB8:   return bmdFormat8BitARGB;
    case F::R210:    return bmdFormat10BitRGB;
    case F::R12B:    return bmdFormat12BitRGB;
    case F::R12L:    return bmdFormat12BitRGBLE;
    default:         return BMDPixelFormat(0);
  }
}

} // namespace Gfx::DeckLink
