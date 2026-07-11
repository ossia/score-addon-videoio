#include "AJACaptureSession.hpp"

#include <QDebug>

#include <ajaanc/includes/ancillarydata.h>
#include <ajaanc/includes/ancillarydata_hdr_hdr10.h>
#include <ajaanc/includes/ancillarydata_hdr_hlg.h>
#include <ajaanc/includes/ancillarylist.h>
#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2formatdescriptor.h>
#include <ntv2signalrouter.h>
#include <ntv2utils.h>
#include <ntv2vpid.h>
#include <ntv2virtualregisters.h>

#include <ajabase/system/process.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstring>

namespace Gfx::AJA
{
namespace
{

NTV2FrameBufferFormat toFBF(AJAInputPixelFormat fmt) noexcept
{
  switch(fmt)
  {
    case AJAInputPixelFormat::YCbCr8:
      return NTV2_FBF_8BIT_YCBCR;
    case AJAInputPixelFormat::YCbCr10:
      return NTV2_FBF_10BIT_YCBCR;
    case AJAInputPixelFormat::ARGB:
      return NTV2_FBF_ARGB; // memory [B,G,R,A] on LE
    case AJAInputPixelFormat::RGBA:
      // NTV2_FBF_ABGR is the framestore whose memory really is [R,G,B,A] on
      // LE (see ConvertARGBYCbCrToABGR in ntv2transcode.cpp). NTV2_FBF_RGBA
      // stores [A,R,G,B] — retail name "ARGB-8" — and would swizzle every
      // channel of the published frames.
      return NTV2_FBF_ABGR;
  }
  return NTV2_FBF_8BIT_YCBCR;
}

AVPixelFormat toAVPixelFormat(AJAInputPixelFormat fmt) noexcept
{
  switch(fmt)
  {
    case AJAInputPixelFormat::YCbCr8:
      return AV_PIX_FMT_UYVY422;
    case AJAInputPixelFormat::YCbCr10:
      return AV_PIX_FMT_NONE; // GPU-direct only
    case AJAInputPixelFormat::ARGB:
      return AV_PIX_FMT_BGRA;
    case AJAInputPixelFormat::RGBA:
      return AV_PIX_FMT_RGBA;
  }
  return AV_PIX_FMT_UYVY422;
}

uint32_t rowBytes(AJAInputPixelFormat fmt, int width) noexcept
{
  switch(fmt)
  {
    case AJAInputPixelFormat::YCbCr8:
      return static_cast<uint32_t>(width) * 2u;
    case AJAInputPixelFormat::YCbCr10:
      return ((static_cast<uint32_t>(width) + 47u) / 48u) * 128u;
    case AJAInputPixelFormat::ARGB:
    case AJAInputPixelFormat::RGBA:
      return static_cast<uint32_t>(width) * 4u;
  }
  return static_cast<uint32_t>(width) * 2u;
}

NTV2VideoFormat promoteHDTo4K(NTV2VideoFormat hd)
{
  switch(hd)
  {
    case NTV2_FORMAT_1080psf_2398:
      return NTV2_FORMAT_4x1920x1080psf_2398;
    case NTV2_FORMAT_1080psf_2400:
      return NTV2_FORMAT_4x1920x1080psf_2400;
    case NTV2_FORMAT_1080p_2398:
      return NTV2_FORMAT_4x1920x1080p_2398;
    case NTV2_FORMAT_1080p_2400:
      return NTV2_FORMAT_4x1920x1080p_2400;
    case NTV2_FORMAT_1080p_2500:
      return NTV2_FORMAT_4x1920x1080p_2500;
    case NTV2_FORMAT_1080p_2997:
      return NTV2_FORMAT_4x1920x1080p_2997;
    case NTV2_FORMAT_1080p_3000:
      return NTV2_FORMAT_4x1920x1080p_3000;
    case NTV2_FORMAT_1080p_5000_A:
    case NTV2_FORMAT_1080p_5000_B:
      return NTV2_FORMAT_4x1920x1080p_5000;
    case NTV2_FORMAT_1080p_5994_A:
    case NTV2_FORMAT_1080p_5994_B:
      return NTV2_FORMAT_4x1920x1080p_5994;
    case NTV2_FORMAT_1080p_6000_A:
    case NTV2_FORMAT_1080p_6000_B:
      return NTV2_FORMAT_4x1920x1080p_6000;
    case NTV2_FORMAT_1080p_2K_2398:
      return NTV2_FORMAT_4x2048x1080p_2398;
    case NTV2_FORMAT_1080p_2K_2400:
      return NTV2_FORMAT_4x2048x1080p_2400;
    case NTV2_FORMAT_1080p_2K_2500:
      return NTV2_FORMAT_4x2048x1080p_2500;
    case NTV2_FORMAT_1080p_2K_2997:
      return NTV2_FORMAT_4x2048x1080p_2997;
    case NTV2_FORMAT_1080p_2K_3000:
      return NTV2_FORMAT_4x2048x1080p_3000;
    case NTV2_FORMAT_1080p_2K_5000_A:
      return NTV2_FORMAT_4x2048x1080p_5000;
    case NTV2_FORMAT_1080p_2K_5994_A:
      return NTV2_FORMAT_4x2048x1080p_5994;
    case NTV2_FORMAT_1080p_2K_6000_A:
      return NTV2_FORMAT_4x2048x1080p_6000;
    default:
      return NTV2_FORMAT_UNKNOWN;
  }
}

NTV2VideoFormat promote4KTo8K(NTV2VideoFormat fourK)
{
  switch(fourK)
  {
    case NTV2_FORMAT_3840x2160p_2398:
      return NTV2_FORMAT_4x3840x2160p_2398;
    case NTV2_FORMAT_3840x2160p_2400:
      return NTV2_FORMAT_4x3840x2160p_2400;
    case NTV2_FORMAT_3840x2160p_2500:
      return NTV2_FORMAT_4x3840x2160p_2500;
    case NTV2_FORMAT_3840x2160p_2997:
      return NTV2_FORMAT_4x3840x2160p_2997;
    case NTV2_FORMAT_3840x2160p_3000:
      return NTV2_FORMAT_4x3840x2160p_3000;
    case NTV2_FORMAT_3840x2160p_5000:
      return NTV2_FORMAT_4x3840x2160p_5000;
    case NTV2_FORMAT_3840x2160p_5994:
      return NTV2_FORMAT_4x3840x2160p_5994;
    case NTV2_FORMAT_3840x2160p_6000:
      return NTV2_FORMAT_4x3840x2160p_6000;
    case NTV2_FORMAT_3840x2160p_5000_B:
      return NTV2_FORMAT_4x3840x2160p_5000_B;
    case NTV2_FORMAT_3840x2160p_5994_B:
      return NTV2_FORMAT_4x3840x2160p_5994_B;
    case NTV2_FORMAT_3840x2160p_6000_B:
      return NTV2_FORMAT_4x3840x2160p_6000_B;
    case NTV2_FORMAT_4096x2160p_2398:
      return NTV2_FORMAT_4x4096x2160p_2398;
    case NTV2_FORMAT_4096x2160p_2400:
      return NTV2_FORMAT_4x4096x2160p_2400;
    case NTV2_FORMAT_4096x2160p_2500:
      return NTV2_FORMAT_4x4096x2160p_2500;
    case NTV2_FORMAT_4096x2160p_2997:
      return NTV2_FORMAT_4x4096x2160p_2997;
    case NTV2_FORMAT_4096x2160p_3000:
      return NTV2_FORMAT_4x4096x2160p_3000;
    case NTV2_FORMAT_4096x2160p_4795:
      return NTV2_FORMAT_4x4096x2160p_4795;
    case NTV2_FORMAT_4096x2160p_4800:
      return NTV2_FORMAT_4x4096x2160p_4800;
    case NTV2_FORMAT_4096x2160p_5000:
      return NTV2_FORMAT_4x4096x2160p_5000;
    case NTV2_FORMAT_4096x2160p_5994:
      return NTV2_FORMAT_4x4096x2160p_5994;
    case NTV2_FORMAT_4096x2160p_6000:
      return NTV2_FORMAT_4x4096x2160p_6000;
    case NTV2_FORMAT_4096x2160p_4795_B:
      return NTV2_FORMAT_4x4096x2160p_4795_B;
    case NTV2_FORMAT_4096x2160p_4800_B:
      return NTV2_FORMAT_4x4096x2160p_4800_B;
    case NTV2_FORMAT_4096x2160p_5000_B:
      return NTV2_FORMAT_4x4096x2160p_5000_B;
    case NTV2_FORMAT_4096x2160p_5994_B:
      return NTV2_FORMAT_4x4096x2160p_5994_B;
    case NTV2_FORMAT_4096x2160p_6000_B:
      return NTV2_FORMAT_4x4096x2160p_6000_B;
    default:
      return NTV2_FORMAT_UNKNOWN;
  }
}

/// True iff the three SDI inputs following `master` all carry the same
/// signal as `detected` — the precondition for treating four cables as one
/// quad-link group. Mirrors ntv2framegrabber's 4K check (it promotes via
/// Get4KInputFormat only when GetInputVideoFormat of inputs n+1..n+3 all
/// match input n). A single camera on one cable must NOT be promoted: with
/// nothing (or unrelated signals) on the other spigots this returns false
/// and Auto stays single-link. Bidirectional spigots still in transmit
/// mode report NTV2_FORMAT_UNKNOWN, which also (correctly) vetoes the
/// promotion — the explicit Quad4K/Quad8K modes remain the way to force a
/// quad group on such wiring.
bool quadLinkSiblingsAgree(
    CNTV2Card& card, NTV2Channel master, NTV2VideoFormat detected)
{
  for(int i = 1; i <= 3; ++i)
  {
    const auto ch = static_cast<NTV2Channel>(master + i);
    if(ch >= NTV2_MAX_NUM_CHANNELS)
      return false;
    const NTV2InputSource src = ::NTV2ChannelToInputSource(ch, NTV2_IOKINDS_SDI);
    if(!card.features().CanDoInputSource(src))
      return false;
    if(card.GetInputVideoFormat(src) != detected)
      return false;
  }
  return true;
}

/// True iff the format is a single-link 4K signal (one SDI cable
/// carries the whole frame, e.g., 12G or 6G), as opposed to a
/// quad-link 4K signal (four 3G cables carry one quadrant each).
///
/// `NTV2_IS_4K_VIDEO_FORMAT` (ntv2enums.h:783) matches both — it
/// covers the `4x...` quad-link ranges (`FIRST_4K_DEF_FORMAT` and
/// `FIRST_4K_DEF_FORMAT2`) AND the single-link UHD/DCI 4K ranges
/// (`FIRST_UHD_TSI_DEF_FORMAT`, `FIRST_4K_TSI_DEF_FORMAT`). For the
/// 12G route decision we want only the single-link halves; the
/// quad-link 4x... formats arrive on 4 cables and need SQD/TSI
/// routing instead. 8K (`NTV2_IS_QUAD_QUAD_FORMAT`) is always 4-cable
/// per the SDK — no single-cable 8K format exists — so the caller
/// must check that separately too.
bool isSingleLink4K(NTV2VideoFormat f) noexcept
{
  return (f >= NTV2_FORMAT_FIRST_UHD_TSI_DEF_FORMAT
          && f < NTV2_FORMAT_END_UHD_TSI_DEF_FORMAT)
         || (f >= NTV2_FORMAT_FIRST_4K_TSI_DEF_FORMAT
             && f < NTV2_FORMAT_END_4K_TSI_DEF_FORMATS);
}

NTV2VideoFormat parseInputVideoFormat(const QString& format)
{
  static const QHash<QString, NTV2VideoFormat> formatMap = {
      {"720p5994", NTV2_FORMAT_720p_5994},
      {"720p60", NTV2_FORMAT_720p_6000},
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
      {"2160p2398", NTV2_FORMAT_4x1920x1080p_2398},
      {"2160p25", NTV2_FORMAT_4x1920x1080p_2500},
      {"2160p2997", NTV2_FORMAT_4x1920x1080p_2997},
      {"2160p30", NTV2_FORMAT_4x1920x1080p_3000},
      {"2160p50", NTV2_FORMAT_4x1920x1080p_5000},
      {"2160p5994", NTV2_FORMAT_4x1920x1080p_5994},
      {"2160p60", NTV2_FORMAT_4x1920x1080p_6000},
  };
  auto it = formatMap.find(format);
  return it != formatMap.end() ? it.value() : NTV2_FORMAT_UNKNOWN;
}

/// Map VPID transfer characteristics to AVColorTransferCharacteristic.
AVColorTransferCharacteristic toAVColorTrc(NTV2VPIDXferChars tc) noexcept
{
  switch(tc)
  {
    case NTV2_VPID_TC_HLG:
      return AVCOL_TRC_ARIB_STD_B67;
    case NTV2_VPID_TC_PQ:
      return AVCOL_TRC_SMPTE2084;
    case NTV2_VPID_TC_SDR_TV:
      return AVCOL_TRC_BT709; // SDR_TV — ITU-R BT.1886 ≈ AVCOL_TRC_BT709
    case NTV2_VPID_TC_Unspecified:
    default:
      return AVCOL_TRC_UNSPECIFIED;
  }
}

/// Map VPID colorimetry to AVColorPrimaries (color_primaries) and
/// AVColorSpace (color_space). UHDTV maps to BT.2020; Rec.709 to
/// BT.709. We pick MatrixCoefficients=NCL for UHDTV by default.
void mapVPIDColorimetry(
    NTV2VPIDColorimetry c, AVColorPrimaries& primaries, AVColorSpace& space) noexcept
{
  switch(c)
  {
    case NTV2_VPID_Color_UHDTV:
      primaries = AVCOL_PRI_BT2020;
      space = AVCOL_SPC_BT2020_NCL;
      break;
    case NTV2_VPID_Color_Rec709:
    default:
      primaries = AVCOL_PRI_BT709;
      space = AVCOL_SPC_BT709;
      break;
  }
}

/// Pretty-format an RP188 timecode struct as "HH:MM:SS:FF".
/// Per the SMPTE 12M packing in RP188_STRUCT (`Low` and `High` words),
/// each unit (Frms 1, Frms 10, Secs 1, ..., Hrs 10) lives in a 4-bit
/// nibble. We extract the BCD digits directly.
std::string formatRP188(const NTV2_RP188& tc)
{
  // Low : | BG4 | Secs10 | BG3 | Secs1 | BG2 | Frms10 | BG1 | Frms1 |
  // High: | BG8 | Hrs 10 | BG7 | Hrs 1 | BG6 | Mins10 | BG5 | Mins1 |
  // Fields are 4-bit each in the standard SMPTE 12M packing. Frms10
  // is only 2 bits (max 39 frames) but we mask 4 to be safe.
  const uint32_t lo = tc.fLo;
  const uint32_t hi = tc.fHi;
  const unsigned f1 = (lo >> 0) & 0xF;
  const unsigned f10 = (lo >> 8) & 0x3;
  const unsigned s1 = (lo >> 16) & 0xF;
  const unsigned s10 = (lo >> 24) & 0x7;
  const unsigned m1 = (hi >> 0) & 0xF;
  const unsigned m10 = (hi >> 8) & 0x7;
  const unsigned h1 = (hi >> 16) & 0xF;
  const unsigned h10 = (hi >> 24) & 0x3;

  char buf[16];
  std::snprintf(
      buf, sizeof(buf), "%u%u:%u%u:%u%u:%u%u",
      h10, h1, m10, m1, s10, s1, f10, f1);
  return std::string{buf};
}

/// VBIs between SDI input-status reads. ~30 VBIs is ~0.5s at 60fps —
/// fast enough to surface a cable-out condition, slow enough that the
/// register read doesn't dominate the per-frame cost.
constexpr int kStatusPollPeriod = 30;

} // namespace

CaptureSession::CaptureSession(const AJAInputSettings& settings)
    : m_settings{settings}
{
}

CaptureSession::~CaptureSession()
{
  close();
}

bool CaptureSession::open()
{
  m_card = std::make_unique<CNTV2Card>();
  if(!CNTV2DeviceScanner::GetDeviceAtIndex(
         static_cast<UWord>(m_settings.deviceIndex), *m_card)
     || !m_card->IsOpen())
  {
    qWarning() << "AJA input: failed to open device" << m_settings.deviceIndex;
    m_card.reset();
    return false;
  }

  // Pass the real PID. With pid=0, the AJA Agent daemon polls the
  // ownership virtual register, sees an invalid PID, treats the
  // device as orphaned, and repossesses it on every frame-cycle
  // hook — fighting our setup. Output had the same bug; symptom on
  // input is harder to spot than the output's test-pattern
  // emission, but the fix is the same.
  const int32_t pid = static_cast<int32_t>(AJAProcess::GetPid());
  if(!m_card->AcquireStreamForApplication(
         NTV2_FOURCC('o', 'i', 'n', 'p'), pid))
  {
    qWarning() << "AJA input: another app holds the device";
    m_card.reset();
    return false;
  }

  if(!m_card->IsDeviceReady(false))
  {
    qWarning() << "AJA input: device" << m_settings.deviceIndex
               << "not ready (FPGA loading?)";
    m_card->ReleaseStreamForApplication(NTV2_FOURCC('o', 'i', 'n', 'p'), pid);
    m_card.reset();
    return false;
  }

  if(!m_card->features().CanDoCapture())
  {
    qWarning() << "AJA input: device" << m_settings.deviceIndex
               << "does not support capture";
    m_card->ReleaseStreamForApplication(NTV2_FOURCC('o', 'i', 'n', 'p'), pid);
    m_card.reset();
    return false;
  }

  // Save the retail-services task mode so we can put it back at close;
  // run in OEM mode for the duration of capture so the AJA daemon
  // doesn't fight our routing.
  m_card->GetEveryFrameServices(m_savedTaskMode);
  m_card->SetEveryFrameServices(NTV2_OEM_TASKS);
  m_taskModeSaved = true;

  m_masterChannel = static_cast<NTV2Channel>(m_settings.channelIndex);

  // Free-run reference + Windows-mandatory VBI subscriptions. Without
  // these, WaitForInputVerticalInterrupt can stall when score isn't
  // the first app to touch the card after boot.
  m_card->SetReference(NTV2_REFERENCE_FREERUN);
  m_card->EnableInputInterrupt(m_masterChannel);
  m_card->SubscribeInputVerticalEvent(m_masterChannel);
  m_card->SubscribeOutputVerticalEvent(NTV2_CHANNEL1);
  return true;
}

void CaptureSession::close()
{
  if(!m_card)
    return;
  teardownChannel();
  m_card->UnsubscribeInputVerticalEvent(m_masterChannel);
  m_card->UnsubscribeOutputVerticalEvent(NTV2_CHANNEL1);
  m_card->DMABufferUnlockAll();
  if(m_taskModeSaved)
  {
    m_card->SetEveryFrameServices(m_savedTaskMode);
    m_taskModeSaved = false;
  }
  m_card->ReleaseStreamForApplication(
      NTV2_FOURCC('o', 'i', 'n', 'p'),
      static_cast<int32_t>(AJAProcess::GetPid()));
  m_card.reset();
}

bool CaptureSession::setupChannel()
{
  if(!m_card)
    return false;

  teardownChannel();

  // First put just the master SDI port into receive so we can probe
  // the format. We'll widen the channel set once we know the topology.
  if(::NTV2DeviceHasBiDirectionalSDI(m_card->GetDeviceID()))
    m_card->SetSDITransmitEnable(m_masterChannel, false);

  // Allow the receiver to lock onto the incoming signal before
  // querying its format. Demos do ~10 VBIs.
  m_card->WaitForOutputVerticalInterrupt(NTV2_CHANNEL1, 10);

  const NTV2InputSource inputSource
      = ::NTV2ChannelToInputSource(m_masterChannel, NTV2_IOKINDS_SDI);
  if(!m_card->features().CanDoInputSource(inputSource))
  {
    qWarning() << "AJA input: channel"
               << m_settings.channelIndex
               << "is not a valid SDI input on this device";
    return false;
  }

  NTV2VideoFormat detected = m_card->GetInputVideoFormat(inputSource);
  if(detected == NTV2_FORMAT_UNKNOWN)
  {
    // Auto-detect failed: fall back to the configured format. Prefer the
    // direct enum (covers any NTV2 format) over the videoFormat-string parse.
    detected = (m_settings.videoFormatEnum != NTV2_FORMAT_UNKNOWN)
                   ? m_settings.videoFormatEnum
                   : parseInputVideoFormat(m_settings.videoFormat);
    qDebug() << "AJA input: no signal detected on channel"
             << m_settings.channelIndex << "- using configured format"
             << ::NTV2VideoFormatToString(detected).c_str();
  }
  if(detected == NTV2_FORMAT_UNKNOWN)
  {
    qWarning() << "AJA input: unknown video format";
    return false;
  }

  // Promote per resolution mode. See enum doc in AJAInput.hpp.
  // Auto keeps whatever arrives on the master cable as-is (a 12G 4K signal
  // is already complete on one link); it only widens to a quad-link group
  // when SDI n+1..n+3 demonstrably carry the same signal, like the SDK's
  // ntv2framegrabber 4K check. A lone 1080p camera stays single-link.
  NTV2VideoFormat promoted = detected;
  AJAInputResolutionMode mode = m_settings.resolutionMode;
  if(mode == AJAInputResolutionMode::Auto)
  {
    if(NTV2_IS_QUAD_QUAD_FORMAT(detected) || NTV2_IS_4K_VIDEO_FORMAT(detected))
      promoted = detected;
    else if(NTV2VideoFormat fourK = promoteHDTo4K(detected);
            fourK != NTV2_FORMAT_UNKNOWN
            && quadLinkSiblingsAgree(*m_card, m_masterChannel, detected))
      promoted = fourK;
  }
  else if(mode == AJAInputResolutionMode::Quad8K)
  {
    promoted = NTV2_IS_QUAD_QUAD_FORMAT(detected) ? detected
                                                  : promote4KTo8K(detected);
    if(promoted == NTV2_FORMAT_UNKNOWN)
    {
      qWarning() << "AJA input: 8K mode but signal isn't a 4K-promotable format";
      return false;
    }
  }
  else if(mode == AJAInputResolutionMode::Quad4K)
  {
    promoted = NTV2_IS_4K_VIDEO_FORMAT(detected) ? detected
                                                 : promoteHDTo4K(detected);
    if(promoted == NTV2_FORMAT_UNKNOWN)
    {
      qWarning() << "AJA input: 4K mode but signal isn't an HD-promotable format";
      return false;
    }
  }
  // SingleLink: keep `detected` as-is even if it's a quad format.

  m_videoFormat = promoted;
  m_bufferFormat = toFBF(m_settings.pixelFormat);

  if(!m_card->features().CanDoVideoFormat(m_videoFormat))
  {
    qWarning() << "AJA input: device cannot handle video format"
               << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat));
    return false;
  }
  if(!m_card->features().CanDoFrameBufferFormat(m_bufferFormat))
  {
    qWarning() << "AJA input: device cannot handle buffer format"
               << QString::fromStdString(
                      ::NTV2FrameBufferFormatToString(m_bufferFormat));
    return false;
  }

  // Pick frame-store and SDI-port counts based on format + topology +
  // routing. The 12G branch (one SDI carries the whole signal) takes
  // precedence over the quad-link branch when:
  //   - the card supports 12G crosspoints, AND
  //   - the user is in single-link mode OR the signal already arrived
  //     as a 4K/8K format on one cable (i.e. detected was 4K/8K and
  //     no promotion happened).
  // Otherwise the quad-link rules apply.
  const bool isQuadQuad = NTV2_IS_QUAD_QUAD_FORMAT(m_videoFormat);
  const bool isQuad4K = NTV2_IS_4K_VIDEO_FORMAT(m_videoFormat);
  const bool isQQHFR = NTV2_IS_QUAD_QUAD_HFR_VIDEO_FORMAT(m_videoFormat);

  // 12G is the right path when:
  //   - the card has 12G crosspoints in its routing matrix, AND
  //   - the detected signal is a single-link 4K format
  //     (3840x2160p_* / 4096x2160p_* — i.e. arriving on one cable).
  //
  // Quad-link 4K (4x1920x1080p_* / 4x2048x1080p_*) reports
  // `NTV2_IS_4K_VIDEO_FORMAT == true` too but arrives on 4 cables —
  // it must go through the SQD/TSI branch, not the 1-cable 12G route.
  // 8K (QUAD_QUAD) is always multi-link per the SDK; no single-cable
  // 8K enum exists.
  // 8K (QUAD_QUAD) is always multi-link per the SDK — there is no single-cable
  // 8K format. With SQD output each cable carries a single-link-4K-looking
  // quadrant, so isSingleLink4K(detected) is true and would wrongly route us to
  // the 1-cable 12G path (capturing only one quadrant). Exclude QuadQuad so 8K
  // always takes the SQD/TSI multi-store branch below.
  m_use12G = m_card->features().CanDo12gRouting()
             && isSingleLink4K(detected) && !isQuadQuad;

  // Quad-link routing strategy. 4K quad-link always uses TSI: on Kona5 the
  // Squares mode does not round-trip (it scrambles quadrants horizontally,
  // confirmed against a bare-NTV2 repro), while TSI works on every backend.
  // 8K honors the user's routingMode (TSI vs SQD) choice.
  const bool useTSI
      = !m_use12G
        && ((isQuad4K && !isQuadQuad)
            || (isQuadQuad
                && m_settings.routingMode == AJAInputRoutingMode::TSI));

  UWord fbCount = 1, sdiCount = 1;
  if(m_use12G)
  {
    // 12G single-link: one cable, one frame store. The card's
    // internal SDI receiver handles the demux from the 12G stream.
    fbCount = 1;
    sdiCount = 1;
  }
  else if(isQuadQuad)
  {
    if(useTSI)
    {
      fbCount = 2;
      sdiCount = isQQHFR ? 4 : 2;
    }
    else
    {
      fbCount = 4;
      sdiCount = 4;
    }
  }
  else if(isQuad4K)
  {
    if(useTSI)
    {
      fbCount = 2;
      sdiCount = 4;
    }
    else
    {
      fbCount = 4;
      sdiCount = 4;
    }
  }
  // 12G/6G single-link: spigot ≠ framestore. The ≥6G-capable PHY is
  // connector 3, but the raster must land in a real framestore — the
  // 12Bit personality has only 2 framestores for 4 spigots, so FS3+
  // silently don't exist (channel enables no-op, no input VBIs, and
  // AutoCirculate parks in STARTING forever). Mirror of the output side.
  if(m_use12G
     && UWord(m_masterChannel) >= m_card->features().GetNumFrameStores())
  {
    qWarning() << "AJA input: channel" << (int(m_masterChannel) + 1)
               << "is not a framestore on this firmware; using channel 1";
    m_masterChannel = NTV2_CHANNEL1;
  }
  m_activeFrameStores = ::NTV2MakeChannelSet(m_masterChannel, fbCount);
  if(m_use12G)
    m_activeSDIs = NTV2ChannelSet{NTV2_CHANNEL3};
  else
    m_activeSDIs = ::NTV2MakeChannelSet(m_masterChannel, sdiCount);
  m_isQuadQuad = isQuadQuad;
  m_is4K = isQuad4K;
  m_useTSI = useTSI;

  m_card->EnableChannels(m_activeFrameStores, /*disableOthers=*/false);
  if(::NTV2DeviceHasBiDirectionalSDI(m_card->GetDeviceID()))
  {
    m_card->SetSDITransmitEnable(m_activeSDIs, false);
    m_card->WaitForOutputVerticalInterrupt(NTV2_CHANNEL1, 10);
  }

  if(!m_card->SetVideoFormat(m_videoFormat, false, false, m_masterChannel))
  {
    qWarning() << "AJA input: SetVideoFormat failed";
    return false;
  }
  m_card->SetVANCMode(m_activeFrameStores, NTV2_VANCMODE_OFF);
  m_card->SetFrameBufferFormat(m_activeFrameStores, m_bufferFormat);

  // Defensive reset of all multi-link demux mode bits before applying the
  // format-specific ones. These bits live in the card hardware and survive a
  // process exit — and the harness exits via std::_Exit() (no teardownChannel).
  // Without this, a card left in QuadQuad/4K mode by a prior 8K/4K run would
  // stay in that mode when we next configure a plain HD/SD format, capturing
  // garbage. Clearing unconditionally here makes setup self-sufficient.
  m_card->SetQuadQuadFrameEnable(false, m_masterChannel);
  m_card->SetQuadQuadSquaresEnable(false, m_masterChannel);
  m_card->SetTsiFrameEnable(false, m_masterChannel);
  m_card->Set4kSquaresEnable(false, m_masterChannel);

  // SQD ↔ TSI mode bits. AJA's QuadQuad and 4K-routing helpers gate
  // on these to switch the card's internal demuxer. Skip for 12G.
  if(m_use12G)
  {
    // 12G single-link 4K: enable the framestore's built-in TSI demux so the
    // host raster matches the 2-sample-interleaved wire order — symmetric
    // with the output side. (ntv2capture4k enables TSI for CanDo12gRouting.)
    m_card->SetTsiFrameEnable(true, m_masterChannel);
    m_card->Set4kSquaresEnable(false, m_masterChannel);
  }
  else if(isQuadQuad)
  {
    m_card->SetQuadQuadFrameEnable(true, m_masterChannel);
    m_card->SetQuadQuadSquaresEnable(!useTSI, m_masterChannel);
  }
  else if(isQuad4K)
  {
    m_card->SetTsiFrameEnable(useTSI, m_masterChannel);
    m_card->Set4kSquaresEnable(!useTSI, m_masterChannel);
  }

  if(!routeSignal(fbCount, sdiCount, useTSI, isQuadQuad, isQQHFR))
  {
    qWarning() << "AJA input: signal route failed";
    return false;
  }

  NTV2FormatDescriptor fd(m_videoFormat, m_bufferFormat);
  m_width = static_cast<int>(fd.numPixels);
  m_height = static_cast<int>(fd.numLines);
  m_frameSize = rowBytes(m_settings.pixelFormat, m_width)
                * static_cast<uint32_t>(m_height);
  m_fps
      = ::GetFramesPerSecond(::GetNTV2FrameRateFromVideoFormat(m_videoFormat));

  // Image format defaults — VPID may overwrite below.
  {
    std::lock_guard lk{m_imageFormatMutex};
    m_imageFormat = Video::ImageFormat{};
    m_imageFormat.width = m_width;
    m_imageFormat.height = m_height;
    m_imageFormat.pixel_format = avPixelFormat();
    // SDI YCbCr is limited-range by convention; primaries default to
    // BT.709 unless VPID says otherwise.
    m_imageFormat.color_range = AVCOL_RANGE_MPEG;
    m_imageFormat.color_space = AVCOL_SPC_BT709;
    m_imageFormat.color_primaries = AVCOL_PRI_BT709;
    m_imageFormat.color_trc = AVCOL_TRC_BT709;
  }
  readVPID();

  // ANC F1/F2 buffer sizes from virtual registers. The card stores
  // ANC in a region at the tail of the frame buffer; field offsets
  // give the size of each field's region.
  ULWord ancF1Off = 0, ancF2Off = 0;
  m_card->ReadRegister(kVRegAncField1Offset, ancF1Off);
  m_card->ReadRegister(kVRegAncField2Offset, ancF2Off);
  // Demos compute "useful" sizes as the difference between the two
  // offsets — F1 region runs from F2-offset to F1-offset (offsets are
  // measured backward from the end of the frame buffer).
  m_ancF1Size = (ancF1Off > ancF2Off) ? (ancF1Off - ancF2Off) : 0;
  m_ancF2Size = ancF2Off; // F2 region runs from end-of-frame to F2-offset
  if(m_ancF1Size == 0 || m_ancF1Size > 1u * 1024u * 1024u)
    m_ancF1Size = 0x2000;
  if(m_ancF2Size > 1u * 1024u * 1024u)
    m_ancF2Size = 0x2000;
  m_ancF1Buf.assign(m_ancF1Size, 0);
  m_ancF2Buf.assign(m_ancF2Size, 0);

  qDebug() << "AJA input: opened device" << m_settings.deviceIndex
           << "channel" << m_settings.channelIndex
           << "FBs" << fbCount << "SDIs" << sdiCount
           << "12G" << m_use12G << "TSI" << useTSI
           << "format"
           << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat))
           << "buffer"
           << QString::fromStdString(
                  ::NTV2FrameBufferFormatToString(m_bufferFormat))
           << "geometry" << m_width << "x" << m_height << "@" << m_fps
           << "anc(F1/F2)" << m_ancF1Size << "/" << m_ancF2Size;
  return true;
}

void CaptureSession::teardownChannel()
{
  if(!m_card)
    return;
  if(m_isQuadQuad)
  {
    m_card->SetQuadQuadFrameEnable(false, m_masterChannel);
    m_card->SetQuadQuadSquaresEnable(false, m_masterChannel);
    m_isQuadQuad = false;
  }
  if(m_is4K)
  {
    m_card->SetTsiFrameEnable(false, m_masterChannel);
    m_card->Set4kSquaresEnable(false, m_masterChannel);
    m_is4K = false;
  }
  m_useTSI = false;
  m_use12G = false;
}

bool CaptureSession::detectAndApplyFormatChange()
{
  if(!m_card)
    return false;
  const NTV2Channel statusCh = (m_use12G && !m_activeSDIs.empty())
                                   ? *m_activeSDIs.begin()
                                   : m_masterChannel;
  const NTV2InputSource src
      = ::NTV2ChannelToInputSource(statusCh, NTV2_IOKINDS_SDI);
  NTV2VideoFormat now = m_card->GetInputVideoFormat(src);
  if(now == NTV2_FORMAT_UNKNOWN)
  {
    // Cable unplugged / source dropped. Don't tear down — leave the
    // current AC running so the consumer keeps the last good frame.
    return false;
  }

  NTV2VideoFormat promoted = now;
  AJAInputResolutionMode mode = m_settings.resolutionMode;
  if(mode == AJAInputResolutionMode::Quad8K)
  {
    promoted = NTV2_IS_QUAD_QUAD_FORMAT(now) ? now : promote4KTo8K(now);
  }
  else if(mode == AJAInputResolutionMode::Quad4K)
  {
    promoted = NTV2_IS_4K_VIDEO_FORMAT(now) ? now : promoteHDTo4K(now);
  }
  else if(mode == AJAInputResolutionMode::Auto)
  {
    // Same rule as setupChannel(): only widen to quad-link when all four
    // inputs agree; a single-cable signal (incl. 12G 4K) stays as detected.
    if(NTV2_IS_QUAD_QUAD_FORMAT(now) || NTV2_IS_4K_VIDEO_FORMAT(now))
      promoted = now;
    else if(NTV2VideoFormat fourK = promoteHDTo4K(now);
            fourK != NTV2_FORMAT_UNKNOWN
            && quadLinkSiblingsAgree(*m_card, m_masterChannel, now))
      promoted = fourK;
  }

  if(promoted == NTV2_FORMAT_UNKNOWN || promoted == m_videoFormat)
    return false;

  qDebug() << "AJA input: signal format change:"
           << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat))
           << "→"
           << QString::fromStdString(::NTV2VideoFormatToString(promoted));

  if(!setupChannel())
  {
    qWarning() << "AJA input: re-setup after signal change failed";
  }
  return true;
}

// Some firmware crosspoint-connect ROMs don't enumerate the SDI -> CSC -> RGB
// framestore connections even though the hardware supports them, so the
// validated ApplySignalRoute rejects the whole route. Re-apply without ROM
// validation (matches the AJA demos' warn-and-continue Connect loop).
static bool applyRouteResilient(CNTV2Card& card, const NTV2XptConnections& conns)
{
  if(card.ApplySignalRoute(conns, /*replace=*/true))
    return true;
  qWarning() << "AJA input: route validation failed; re-applying without "
                "crosspoint-ROM validation";
  card.ClearRouting();
  bool ok = true;
  for(const auto& [in, out] : conns)
    ok &= card.Connect(in, out, /*inValidate=*/false);
  return ok;
}

bool CaptureSession::routeSignal(
    UWord fbCount, UWord sdiCount, bool useTSI, bool isQuadQuad, bool isQQHFR)
{
  NTV2XptConnections connections;
  const bool isYCbCrFB = NTV2_FBF_IS_YCBCR(m_bufferFormat);

  // 12G single-link: SDI[ch] → FrameStore[ch] (YCbCr) or via CSC (RGB).
  // Same shape as single-link HD/3G; the card handles 12G demux
  // internally given the frame format and SetVideoFormat were called.
  if(m_use12G)
  {
    // Spigot ≠ framestore for single-link ≥6G (see topology setup).
    const NTV2Channel spigot
        = m_activeSDIs.empty() ? m_masterChannel : *m_activeSDIs.begin();
    const NTV2OutputCrosspointID sdiOutXpt
        = ::GetSDIInputOutputXptFromChannel(spigot, /*DS2=*/false);
    const NTV2InputCrosspointID fbInXpt
        = ::GetFrameStoreInputXptFromChannel(m_masterChannel, /*isBInput=*/false);
    if(isYCbCrFB)
    {
      connections.insert({fbInXpt, sdiOutXpt});
    }
    else
    {
      const NTV2InputCrosspointID cscVidIn
          = ::GetCSCInputXptFromChannel(m_masterChannel, /*Key=*/false);
      const NTV2OutputCrosspointID cscRGBOut = ::GetCSCOutputXptFromChannel(
          m_masterChannel, /*Key=*/false, /*RGB=*/true);
      connections.insert({cscVidIn, sdiOutXpt});
      connections.insert({fbInXpt, cscRGBOut});
    }
    return applyRouteResilient(*m_card, connections);
  }

  if(!useTSI)
  {
    // SQD (or single-link non-12G, where fbCount == sdiCount == 1).
    for(UWord i = 0; i < fbCount; ++i)
    {
      auto ch = static_cast<NTV2Channel>(m_masterChannel + i);
      const NTV2OutputCrosspointID sdiOutXpt
          = ::GetSDIInputOutputXptFromChannel(ch, /*DS2=*/false);
      const NTV2InputCrosspointID fbInXpt
          = ::GetFrameStoreInputXptFromChannel(ch, /*isBInput=*/false);
      if(isYCbCrFB)
      {
        connections.insert({fbInXpt, sdiOutXpt});
      }
      else
      {
        const NTV2InputCrosspointID cscVidIn
            = ::GetCSCInputXptFromChannel(ch, /*Key=*/false);
        const NTV2OutputCrosspointID cscRGBOut
            = ::GetCSCOutputXptFromChannel(
                ch, /*Key=*/false, /*RGB=*/true);
        connections.insert({cscVidIn, sdiOutXpt});
        connections.insert({fbInXpt, cscRGBOut});
      }
    }
    return applyRouteResilient(*m_card, connections);
  }

  // TSI from here on (4K and 8K). RGB TSI uses DLIn instead of plain
  // crosspoints; not implemented (no concrete RGB-TSI capture
  // request yet, and score's input formats are YUV-first).
  if(!isYCbCrFB)
  {
    qWarning() << "AJA input: RGB TSI routing is not implemented";
    return false;
  }

  for(UWord path = 0; path < 4; ++path)
  {
    auto fbCh = static_cast<NTV2Channel>(m_masterChannel + path / 2);
    const bool isB = (path & 1) != 0;
    if(isQuadQuad)
    {
      // 8K TSI YUV: no on-card TSI mux crosspoints — SDI inputs wire
      // directly to the frame stores' DS1/DS2 sub-streams. SDI source
      // depends on HFR (4 SDIs) vs non-HFR (2 SDIs interleaved).
      NTV2Channel sdiCh;
      bool sdiDS2;
      if(isQQHFR)
      {
        sdiCh = static_cast<NTV2Channel>(m_masterChannel + path);
        sdiDS2 = false;
      }
      else
      {
        sdiCh = static_cast<NTV2Channel>(m_masterChannel + path / 2);
        sdiDS2 = isB;
      }
      const NTV2OutputCrosspointID sdiOut
          = ::GetSDIInputOutputXptFromChannel(sdiCh, sdiDS2);
      const NTV2InputCrosspointID fbIn
          = ::GetFrameStoreInputXptFromChannel(fbCh, /*isBInput=*/isB);
      connections.insert({fbIn, sdiOut});
    }
    else
    {
      // 4K TSI YUV: 4 SDIs through 2 TSI muxers into 2 frame stores.
      auto sdiCh = static_cast<NTV2Channel>(m_masterChannel + path);
      auto muxCh = static_cast<NTV2Channel>(m_masterChannel + path / 2);
      const NTV2InputCrosspointID muxIn
          = ::GetTSIMuxInputXptFromChannel(muxCh, /*LinkB=*/isB);
      const NTV2OutputCrosspointID sdiOut
          = ::GetSDIInputOutputXptFromChannel(sdiCh, /*DS2=*/false);
      connections.insert({muxIn, sdiOut});

      const NTV2InputCrosspointID fbIn
          = ::GetFrameStoreInputXptFromChannel(fbCh, /*isBInput=*/isB);
      const NTV2OutputCrosspointID muxOut = ::GetTSIMuxOutputXptFromChannel(
          muxCh, /*LinkB=*/isB, /*RGB=*/false);
      connections.insert({fbIn, muxOut});
    }
  }
  return applyRouteResilient(*m_card, connections);
}

void CaptureSession::readVPID()
{
  // ReadSDIInVPID reads VPID payload bytes A and B (per SMPTE ST 352)
  // for the master channel. Two ULWords: A = bytes 0..3 (link A or
  // single-link), B = bytes 4..7 (link B for dual-link 12G).
  ULWord vpidA = 0, vpidB = 0;
  if(!m_card->ReadSDIInVPID(m_masterChannel, vpidA, vpidB))
    return;
  if(vpidA == 0)
    return; // no VPID detected

  CNTV2VPID vpid(vpidA);
  if(!vpid.IsValid())
    return;

  const NTV2VPIDXferChars trc = vpid.GetTransferCharacteristics();
  const NTV2VPIDColorimetry colo = vpid.GetColorimetry();
  const NTV2VPIDRGBRange range = vpid.GetRGBRange();

  AVColorPrimaries primaries = AVCOL_PRI_BT709;
  AVColorSpace space = AVCOL_SPC_BT709;
  mapVPIDColorimetry(colo, primaries, space);
  AVColorTransferCharacteristic newTrc = toAVColorTrc(trc);

  std::lock_guard lk{m_imageFormatMutex};
  m_imageFormat.color_primaries = primaries;
  m_imageFormat.color_space = space;
  if(newTrc != AVCOL_TRC_UNSPECIFIED)
    m_imageFormat.color_trc = newTrc;
  // VPID range field only applies to RGB; YCbCr is always limited.
  if(NTV2_IS_FBF_RGB(m_bufferFormat))
  {
    m_imageFormat.color_range
        = (range == NTV2_VPID_Range_Full) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  }
  else
  {
    m_imageFormat.color_range = AVCOL_RANGE_MPEG;
  }

  qDebug() << "AJA input: VPID transfer="
           << (trc == NTV2_VPID_TC_PQ      ? "PQ"
               : trc == NTV2_VPID_TC_HLG   ? "HLG"
               : trc == NTV2_VPID_TC_SDR_TV ? "SDR"
                                            : "?")
           << "colorimetry="
           << (colo == NTV2_VPID_Color_UHDTV   ? "BT.2020"
               : colo == NTV2_VPID_Color_Rec709 ? "BT.709"
                                                : "?");
}

void CaptureSession::readSDIStatus()
{
  if(!m_card->features().CanDoSDIErrorChecks())
    return;
  NTV2SDIInStatistics stats;
  if(!m_card->ReadSDIStatistics(stats))
    return;
  NTV2SDIInputStatus status;
  const NTV2Channel statusCh = (m_use12G && !m_activeSDIs.empty())
                                   ? *m_activeSDIs.begin()
                                   : m_masterChannel;
  const UWord spigot = static_cast<UWord>(::GetIndexForNTV2InputSource(
      ::NTV2ChannelToInputSource(statusCh, NTV2_IOKINDS_SDI)));
  stats.GetSDIInputStatus(status, spigot);
  signalLocked.store(status.mLocked, std::memory_order_relaxed);
  // CRC tallies are per-line counts, separate for the "A" and "B"
  // streams of the SDI link. Sum them — for our purposes a single
  // monotonic "any error?" readout is enough; consumers can compute
  // per-second rates by sampling.
  crcErrorCount.store(
      static_cast<uint32_t>(status.mCRCTallyA)
          + static_cast<uint32_t>(status.mCRCTallyB),
      std::memory_order_relaxed);
}

void CaptureSession::readTimecodes(const AUTOCIRCULATE_TRANSFER& xfer)
{
  // Preference order: ATC LTC (ancillary RP188 LTC, the most reliable
  // SDI-embedded one) > LTC1 (analog) > VITC on SDI1 (NTV2_TCINDEX_SDI1
  // is the embedded-VITC slot per ntv2enums.h:3954). Read each index
  // directly via the per-index getter instead of building an
  // NTV2TimeCodes (std::map) every frame; IsValid() filters out indices
  // the card didn't actually see on the wire this frame.
  static constexpr NTV2TCIndex preferred[]
      = {NTV2_TCINDEX_SDI1_LTC, NTV2_TCINDEX_LTC1, NTV2_TCINDEX_SDI1};
  NTV2_RP188 tc;
  for(auto idx : preferred)
  {
    if(xfer.acTransferStatus.acFrameStamp.GetInputTimeCode(tc, idx)
       && tc.IsValid())
    {
      const std::string formatted = formatRP188(tc);
      std::lock_guard lk{timecodeMutex};
      lastTimecode = formatted;
      return;
    }
  }
}

void CaptureSession::readAncPayloads(const AUTOCIRCULATE_TRANSFER& xfer)
{
  if(m_ancF1Size == 0 && m_ancF2Size == 0)
    return;
  const ULWord f1Bytes = xfer.GetCapturedAncByteCount(false);
  const ULWord f2Bytes = xfer.GetCapturedAncByteCount(true);
  if(f1Bytes == 0 && f2Bytes == 0)
    return;

  NTV2Buffer f1Buf{m_ancF1Buf.data(), std::min<size_t>(f1Bytes, m_ancF1Buf.size())};
  NTV2Buffer f2Buf{m_ancF2Buf.data(), std::min<size_t>(f2Bytes, m_ancF2Buf.size())};
  AJAAncillaryList ancList;
  if(AJAAncillaryList::SetFromDeviceAncBuffers(f1Buf, f2Buf, ancList) != AJA_STATUS_SUCCESS)
    return;

  // HDR static metadata (ST 2108-1). The HDR10 / HLG-detection here
  // updates the transfer characteristic in case VPID didn't flag it
  // (some sources omit VPID byte 2 colorimetry but still ship HDR
  // metadata in ANC).
  //
  // NB: AJA's AJAAncillaryDataFactory::GuessAncillaryDataType does NOT include
  // the HDR recognizers — AJAAncillaryData_HDR_HDR10/HLG::RecognizeThisAncillaryData
  // are absent from the cascade in ancillarydatafactory.cpp — so
  // SetFromDeviceAncBuffers types the DID=0xC0 packet as Unknown and
  // CountAncillaryDataWithType(AJAAncDataType_HDR_*) ALWAYS returns 0 even when
  // the packet is present on the wire. Detect by raw DID/SID instead. HDR10 and
  // HLG share DID=0xC0/SID=0x00, so disambiguate via the EOTF byte (ST 2108 SDP
  // payload[1]: 0x02 = PQ/HDR10, 0x03 = HLG).
  // TODO: parse the rest of the payload (24 bytes ST 2086 + 4 bytes ST 2094-10)
  //       into m_imageFormat.mastering_display / content_light. Skipped here
  //       because the VPID transfer flag alone drives the right convert_to_rgb.
  bool hasHdr10 = false, hasHlg = false;
  if(AJAAncillaryData* pkt = ancList.GetAncillaryDataWithID(
         AJAAncillaryData_HDR_HDR10_DID, AJAAncillaryData_HDR_HDR10_SID))
  {
    const uint8_t eotf = pkt->GetDC() >= 2 ? pkt->GetPayloadByteAtIndex(1) : 0xFF;
    if(eotf == 0x03)
      hasHlg = true;
    else
      hasHdr10 = true; // 0x02 (PQ) or unspecified -> treat as HDR10
  }
  if(hasHdr10 || hasHlg)
  {
    std::lock_guard lk{m_imageFormatMutex};
    if(m_imageFormat.color_trc != AVCOL_TRC_SMPTE2084
       && m_imageFormat.color_trc != AVCOL_TRC_ARIB_STD_B67)
    {
      m_imageFormat.color_trc
          = hasHdr10 ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_ARIB_STD_B67;
    }
  }
}

void CaptureSession::attachAncToTransfer(AUTOCIRCULATE_TRANSFER& xfer) noexcept
{
  if(m_ancF1Size == 0 && m_ancF2Size == 0)
    return;
  xfer.SetAncBuffers(
      reinterpret_cast<ULWord*>(m_ancF1Buf.data()),
      static_cast<ULWord>(m_ancF1Size),
      reinterpret_cast<ULWord*>(m_ancF2Buf.data()),
      static_cast<ULWord>(m_ancF2Size));
}

void CaptureSession::readPerFrameMetadata(const AUTOCIRCULATE_TRANSFER& xfer)
{
  // Timecode is genuinely per-frame. The ANC HDR parse, however, builds and
  // walks a whole AJAAncillaryList over the F1/F2 buffers just to flip a
  // transfer-characteristic flag that is static for the stream — so run it
  // (and the SDI-status register read) only on the poll cadence, not every
  // VBI. HDR detection lags stream start by < kStatusPollPeriod VBIs, which
  // is harmless: the VPID-derived TRC from setup is the value until then.
  readTimecodes(xfer);
  if(++m_statusPollCounter >= kStatusPollPeriod)
  {
    m_statusPollCounter = 0;
    readSDIStatus();
    // Re-read the input VPID so transfer/colorimetry signaled by the source is
    // picked up once it stabilizes (the setup-time read can miss a late-latching
    // VPID). Run BEFORE the ANC HDR parse so a HDR Static-Metadata ANC packet
    // takes precedence over a VPID that still reads SDR.
    readVPID();
    readAncPayloads(xfer);
  }
}

Video::ImageFormat CaptureSession::imageFormat() const
{
  std::lock_guard lk{m_imageFormatMutex};
  return m_imageFormat;
}

AVPixelFormat CaptureSession::avPixelFormat() const noexcept
{
  return toAVPixelFormat(m_settings.pixelFormat);
}

} // namespace Gfx::AJA
