#include "AjaOutputBackend.hpp"

#include <AJA/AjaFormatMap.hpp>
#include <AJA/Tier3Common.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Gfx/Graph/interop/StageProfiler.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

// GPU-direct strategy candidates (moved from AJANode::createOutput).
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && defined(_WIN32)
#include <AJA/RdmaInteropD3D11Tier3.hpp>
#include <AJA/RdmaInteropD3D12Tier3.hpp>
#endif
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && (QT_HAS_VULKAN || (QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)))
#include <AJA/RdmaInteropVulkanTier3.hpp>
#define AJA_HAS_RDMA_VULKAN 1
#endif
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && QT_CONFIG(opengl)
#include <AJA/RdmaInteropGLTier3.hpp>
#define AJA_HAS_RDMA_GL 1
#endif
#if defined(SCORE_HAS_AJA_DVP_BRIDGE)
#include <AJA/AjaDmaLockPolicy.hpp>
#include <AJA/AjaDvpEncoder.hpp>
#if defined(_WIN32)
#include <Gfx/Graph/interop/DvpOutputD3D11.hpp>
#define AJA_HAS_DVP_D3D11 1
#endif
#if QT_CONFIG(opengl)
#include <Gfx/Graph/interop/DvpOutputGl.hpp>
#define AJA_HAS_DVP_GL 1
#endif
#endif

#include <ajaanc/includes/ancillarydata_hdr_hdr10.h>
#include <ajaanc/includes/ancillarydata_hdr_hlg.h>
#include <ajaanc/includes/ancillarylist.h>

#include <ntv2devicescanner.h>
#include <ntv2publicinterface.h>
#include <ntv2signalrouter.h>
#include <ntv2transcode.h>
#include <ntv2utils.h>
#include <ntv2vpid.h>

#include <ajabase/system/process.h>

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::AJA
{
namespace
{
bool isYCbCr(NTV2FrameBufferFormat f) noexcept
{
  return f == NTV2_FBF_8BIT_YCBCR || f == NTV2_FBF_10BIT_YCBCR;
}

// v210 line pack for non-mod-6 widths (UYVY -> v210 on the CPU). 2 bits of
// chroma precision are lost vs a true 10-bit GPU encoder.
void packUyvyLineToV210(
    const uint8_t* uyvyLine, ULWord* v210Line, ULWord pixelsPerLine) noexcept
{
  ConvertLine_2vuy_to_v210(uyvyLine, v210Line, pixelsPerLine);
}
} // namespace

AjaOutputBackend::AjaOutputBackend(const AJAOutputSettings& settings)
    : m_settings{settings}
{
  m_videoFormat = (settings.videoFormatEnum != NTV2_FORMAT_UNKNOWN)
                      ? settings.videoFormatEnum
                      : parseAjaVideoFormat(settings.videoFormat, settings.rate);
  m_bufferFormat = parseAjaPixelFormat(settings.pixelFormat);
  // HDR (PQ / HLG) requires 10-bit chroma; force v210 otherwise.
  if(settings.hdrMode != AJAHDRMode::Off
     && m_bufferFormat != NTV2_FBF_10BIT_YCBCR)
  {
    qDebug() << "AJA: HDR mode forces 10-bit YCbCr (v210)";
    m_bufferFormat = NTV2_FBF_10BIT_YCBCR;
  }
  m_channel = static_cast<NTV2Channel>(settings.channelIndex);

  // v210 at a width not divisible by 6 can't use the GPU V210Encoder; emit UYVY
  // and pack on the CPU in customStage().
  m_v210NeedsCpuPack
      = (m_bufferFormat == NTV2_FBF_10BIT_YCBCR && (m_settings.width % 6 != 0));

  // Loop-invariant RP188 timecode constants (m_videoFormat is fixed).
  const NTV2FrameRate fr = ::GetNTV2FrameRateFromVideoFormat(m_videoFormat);
  m_framesPerSec = static_cast<uint32_t>(::GetFramesPerSecond(fr) + 0.5);
  if(m_framesPerSec == 0)
    m_framesPerSec = 1;
  m_dropFrame = (fr == NTV2_FRAMERATE_2997 || fr == NTV2_FRAMERATE_5994
                 || fr == NTV2_FRAMERATE_11988);
  // ATC LTC indices are NOT contiguous in NTV2TCIndex; list them explicitly.
  static constexpr NTV2TCIndex sdiLTC[8] = {
      NTV2_TCINDEX_SDI1_LTC, NTV2_TCINDEX_SDI2_LTC, NTV2_TCINDEX_SDI3_LTC,
      NTV2_TCINDEX_SDI4_LTC, NTV2_TCINDEX_SDI5_LTC, NTV2_TCINDEX_SDI6_LTC,
      NTV2_TCINDEX_SDI7_LTC, NTV2_TCINDEX_SDI8_LTC};
  m_tcMap.reserve(8);
  for(NTV2TCIndex idx : sdiLTC)
    m_tcMap.emplace(idx, NTV2_RP188{});
}

AjaOutputBackend::~AjaOutputBackend() = default;

// =============================================================================
// DirectVideoOutputBackend-shaped accessors
// =============================================================================

int AjaOutputBackend::visibleRows() const noexcept
{
  return static_cast<int>(m_formatDesc.GetVisibleRasterHeight());
}

score::gfx::interop::VideoPixelFormat
AjaOutputBackend::wireFormat() const noexcept
{
  return ntv2FormatTo(m_bufferFormat);
}

score::gfx::interop::VideoPixelFormat
AjaOutputBackend::encoderFormat() const noexcept
{
  if(m_v210NeedsCpuPack)
    return score::gfx::interop::VideoPixelFormat::UYVY422;
  return wireFormat();
}

bool AjaOutputBackend::prefersFloatRender() const noexcept
{
  // Render into RGBA16F for 10/12-bit wire formats or HDR so the encoder sees
  // real >8-bit precision (and PQ/HLG headroom).
  const bool tenBit = (m_bufferFormat == NTV2_FBF_10BIT_YCBCR
                       || m_bufferFormat == NTV2_FBF_10BIT_RGB
                       || m_bufferFormat == NTV2_FBF_10BIT_ARGB
                       || m_bufferFormat == NTV2_FBF_10BIT_DPX
                       || m_bufferFormat == NTV2_FBF_10BIT_DPX_LE
                       || m_bufferFormat == NTV2_FBF_48BIT_RGB
                       || m_bufferFormat == NTV2_FBF_12BIT_RGB_PACKED);
  return tenBit || m_settings.hdrMode != AJAHDRMode::Off;
}

QString AjaOutputBackend::colorConversion() const
{
  switch(m_settings.hdrMode)
  {
    case AJAHDRMode::HDR10:
      return score::gfx::colorMatrixOut(
          AVCOL_SPC_BT2020_NCL, AVCOL_TRC_SMPTE2084, AVCOL_RANGE_MPEG,
          AVCOL_PRI_BT2020);
    case AJAHDRMode::HLG:
      return score::gfx::colorMatrixOut(
          AVCOL_SPC_BT2020_NCL, AVCOL_TRC_ARIB_STD_B67, AVCOL_RANGE_MPEG,
          AVCOL_PRI_BT2020);
    case AJAHDRMode::Off:
    default:
      return score::gfx::colorMatrixOut(
          AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
  }
}

std::vector<score::gfx::interop::HostStagedPlane> AjaOutputBackend::planes() const
{
  std::vector<score::gfx::interop::HostStagedPlane> out;
  const int n = static_cast<int>(m_formatDesc.GetNumPlanes());
  for(int p = 0; p < (n > 0 ? n : 1); ++p)
    out.push_back(
        {static_cast<int>(m_formatDesc.GetBytesPerRow(UWord(p))),
         static_cast<uint32_t>(m_formatDesc.GetTotalRasterBytes(UWord(p)))});
  return out;
}

score::gfx::interop::VendorDmaRegistrar AjaOutputBackend::registrar()
{
  CNTV2Card* card = m_card.get();
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [card](void* ptr, uint32_t size) {
    return card->DMABufferLock(
        reinterpret_cast<const ULWord*>(ptr), static_cast<ULWord>(size),
        /*inMap=*/true, /*inRDMA=*/false);
  };
  reg.releaseSlot = [card](void* ptr, uint32_t size) {
    card->DMABufferUnlock(
        reinterpret_cast<const ULWord*>(ptr), static_cast<ULWord>(size));
  };
  return reg;
}

score::gfx::DirectVideoOutputBackend::CustomStage AjaOutputBackend::customStage()
{
  if(!m_v210NeedsCpuPack)
    return {};
  const ULWord ppl = static_cast<ULWord>(m_settings.width);
  return [ppl](
             const uint8_t* src, int srcRowBytes, uint8_t* dst, int dstRowBytes,
             int rows) {
    for(int y = 0; y < rows; ++y)
      packUyvyLineToV210(
          src + std::ptrdiff_t(y) * srcRowBytes,
          reinterpret_cast<ULWord*>(dst + std::ptrdiff_t(y) * dstRowBytes), ppl);
    return true;
  };
}

std::vector<std::function<std::unique_ptr<score::gfx::interop::GpuDirectStrategy>()>>
AjaOutputBackend::gpuDirectCandidates(QRhi* rhi, score::gfx::GraphicsApi api)
{
  using Strat = score::gfx::interop::GpuDirectStrategy;
  std::vector<std::function<std::unique_ptr<Strat>()>> candidates;

  // SCORE_AJA_FORCE_INTEROP = cpu | dvp | rdma (empty/unset = auto).
  const QByteArray forceIop = qgetenv("SCORE_AJA_FORCE_INTEROP");
  const bool wantGpu = m_settings.useRDMA && forceIop != "cpu";
  const bool allowDvp = forceIop.isEmpty() || forceIop == "dvp";
  const bool allowRdma = forceIop.isEmpty() || forceIop == "rdma";
  if(!wantGpu)
    return candidates;

  CNTV2Card* card = m_card.get();
  const NTV2FrameBufferFormat fbf = m_bufferFormat;

  if(allowDvp)
  {
#if defined(AJA_HAS_DVP_D3D11)
    if(api == score::gfx::GraphicsApi::D3D11)
      candidates.push_back([card, fbf] {
        return std::make_unique<score::gfx::interop::DvpOutputD3D11<AjaDmaLockPolicy>>(
            AjaDmaLockPolicy{card},
            [fbf] { return ajaMakeFragmentEncoder(fbf); },
            [fbf](score::gfx::GPUVideoEncoder* e) {
              return ajaEncoderOutputTexture(e, fbf);
            },
            "DVP-D3D11");
      });
#endif
#if defined(AJA_HAS_DVP_GL)
    if(api == score::gfx::GraphicsApi::OpenGL)
      candidates.push_back([card, fbf] {
        return std::make_unique<score::gfx::interop::DvpOutputGl<AjaDmaLockPolicy>>(
            AjaDmaLockPolicy{card},
            [fbf] { return ajaMakeFragmentEncoder(fbf); },
            [fbf](score::gfx::GPUVideoEncoder* e) {
              return ajaEncoderOutputTexture(e, fbf);
            },
            "DVP-GL");
      });
#endif
  }
  // Tier-3 RDMA (Linux; on Windows these init() to false).
  if(allowRdma && rhi && rhi->isFeatureSupported(QRhi::Compute)
     && tier3SupportsFormat(m_bufferFormat, m_settings.width))
  {
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && defined(_WIN32)
    if(api == score::gfx::GraphicsApi::D3D11)
      candidates.push_back([card, fbf] { return std::make_unique<RdmaInteropD3D11Tier3>(card, fbf); });
    else if(api == score::gfx::GraphicsApi::D3D12)
      candidates.push_back([card, fbf] { return std::make_unique<RdmaInteropD3D12Tier3>(card, fbf); });
#endif
#if defined(AJA_HAS_RDMA_VULKAN)
    if(api == score::gfx::GraphicsApi::Vulkan)
      candidates.push_back([card, fbf] { return std::make_unique<RdmaInteropVulkanTier3>(card, fbf); });
#endif
#if defined(AJA_HAS_RDMA_GL)
    if(api == score::gfx::GraphicsApi::OpenGL)
      candidates.push_back([card, fbf] { return std::make_unique<RdmaInteropGLTier3>(card, fbf); });
#endif
  }
  return candidates;
}

// =============================================================================
// PacedFramePump hooks (run on the pump's consumer thread)
// =============================================================================

score::gfx::interop::PacedFramePump::Hooks AjaOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [this] { return waitForVBI(); };
  h.canAccept = [this] { return cardCanAccept(); };
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool AjaOutputBackend::waitForVBI()
{
  // Block until the next SDI VBI on this output channel (100 ms timeout so the
  // pump re-checks shutdown). false => the pump loops and waits again.
  return m_card->WaitForOutputVerticalInterrupt(m_channel, 1);
}

bool AjaOutputBackend::cardCanAccept()
{
  AUTOCIRCULATE_STATUS status;
  if(!m_card->AutoCirculateGetStatus(m_channel, status))
    return false;
  if(m_acStarted && !status.CanAcceptMoreOutputFrames())
    return false;
  // Cap in-flight depth below the full AC ring: the pump stuffs the ring
  // whenever we accept, so accepting until ring-full trades ~4 extra
  // frames of end-to-end latency for nothing — 2 queued frames already
  // sustain rate (transfer completes well inside a frame period), and each
  // extra queued frame costs a full frame period of end-to-end latency
  // (16.7 ms at 60p, 40 ms at 25p) in exchange for that much more host-jitter
  // headroom before the card underruns.
  //
  // Precedence (highest first):
  //   1. SCORE_AJA_OUT_DEPTH env var  — debug override, floored at 2
  //   2. AJAOutputSettings::outputRingDepth (> 0) — user-facing setting,
  //      clamped to [1, kFrameCount]
  //   3. default of 2 — the auto / lowest-latency behavior (setting == 0),
  //      keeping one frame of jitter headroom so existing projects are unchanged
  // Resolved once per backend instance (settings are fixed for a session).
  if(m_maxQueuedDepth < 0)
  {
    if(const char* v = std::getenv("SCORE_AJA_OUT_DEPTH"); v && *v)
      m_maxQueuedDepth = std::max(2, std::atoi(v));
    else if(m_settings.outputRingDepth > 0)
      m_maxQueuedDepth
          = std::clamp(m_settings.outputRingDepth, 1, int(kFrameCount));
    else
      m_maxQueuedDepth = 2;
  }
  if(m_acStarted && status.GetBufferLevel() >= m_maxQueuedDepth)
    return false;
  return true;
}

bool AjaOutputBackend::submitFrame(void* framePtr)
{
  m_xfer.SetVideoBuffer(
      reinterpret_cast<ULWord*>(framePtr),
      static_cast<ULWord>(m_frameBufferSize));
  // ANC (HDR static metadata) attached every frame; empty buffer => none.
  uint32_t* ancPtr = m_hdrAncBuffer.empty() ? nullptr : m_hdrAncBuffer.data();
  const uint32_t ancSize
      = ancPtr ? static_cast<uint32_t>(m_hdrAncBuffer.size() * sizeof(uint32_t))
               : 0;
  m_xfer.SetAncBuffers(ancPtr, ancSize, nullptr, 0);

  // Free-running RP188 LTC so downstream switchers/recorders that lipsync on TC
  // have a monotonic timecode (built by hand from the frame counter).
  {
    const uint32_t f = static_cast<uint32_t>(m_outputFrame % m_framesPerSec);
    const uint32_t totalSec = static_cast<uint32_t>(m_outputFrame / m_framesPerSec);
    const uint32_t s = totalSec % 60;
    const uint32_t mn = (totalSec / 60) % 60;
    const uint32_t h = (totalSec / 3600) % 24;
    const uint32_t lo = ((f % 10) & 0xF) | (((f / 10) & 0x3) << 8)
                        | ((s % 10) << 16) | (((s / 10) & 0x7) << 24);
    const uint32_t hi = ((mn % 10) & 0xF) | (((mn / 10) & 0x7) << 8)
                        | ((h % 10) << 16) | (((h / 10) & 0x3) << 24);
    const uint32_t hiOut = m_dropFrame ? (hi | (1u << 27)) : hi;
    const NTV2_RP188 rp188(/*DBB*/ 0, /*Lo*/ lo, /*Hi*/ hiOut);
    for(auto& [idx, tc] : m_tcMap)
    {
      tc = rp188;
      m_xfer.SetOutputTimeCode(tc, idx);
    }
  }
  ++m_outputFrame;

  {
    SCORE_STAGE_PROFILE(profAcXfer, "aja-out-ac-transfer");
    if(!m_card->AutoCirculateTransfer(m_channel, m_xfer))
    {
      qWarning() << "AJA: AutoCirculateTransfer failed";
      return false;
    }
  }
  if(!m_acStarted && ++m_acGoodXfers >= 3)
  {
    m_card->AutoCirculateStart(m_channel);
    m_acStarted = true;
  }
  return true;
}

bool AjaOutputBackend::initializeAJADevice()
{
  if(m_deviceInitialized)
    return true;

  m_card = std::make_unique<CNTV2Card>(static_cast<UWord>(m_settings.deviceIndex));
  if(!m_card->IsOpen())
  {
    qWarning() << "AJA: Failed to open device" << m_settings.deviceIndex;
    m_card.reset();
    return false;
  }

  // FPGA-mid-reload guard. SetVideoFormat returns success but the card
  // is still booting if we open it during a firmware load.
  if(!m_card->IsDeviceReady(false))
  {
    qWarning() << "AJA: device" << m_settings.deviceIndex
               << "not ready (FPGA loading?)";
    m_card.reset();
    return false;
  }
  if(!m_card->features().CanDoPlayback())
  {
    qWarning() << "AJA: device" << m_settings.deviceIndex
               << "does not support playback";
    m_card.reset();
    return false;
  }

  // BISECT phase 5: re-enable AcquireStream + OEM mode, plus the
  // missing SetMultiFormatMode(false). The 8K demo at
  // ntv2player8k.cpp:142-145 sets multi-format mode explicitly:
  //   if (CanDoMultiFormat()) SetMultiFormatMode(fDoMultiFormat);
  // For single-app mode (us), fDoMultiFormat=false. Without this,
  // the card may remain in multi-format mode (each channel having
  // its own video format), in which case SetVideoFormat on the
  // master channel doesn't propagate to the sibling FBs. The
  // sibling FBs (CH2-4 for 8K SQD) then stay in their default
  // state and emit their built-in test pattern.
  //
  // Why it only mattered after AcquireStream: with the AJA Agent
  // daemon actively managing the device (no Acquire), the daemon
  // forces single-format-mode on its own. Once we Acquire, the
  // daemon stops, and our missing single-format setting becomes
  // visible.
  // Pass the real process ID, NOT 0. The AJA Agent daemon polls the
  // ownership virtual register; if the recorded pid is invalid (0),
  // the daemon treats the device as orphaned and attempts to
  // repossess it on every frame-cycle hook, fighting our routing
  // setup. The user-visible symptom: card emits test pattern even
  // though our setup logs all succeed. Demos pass
  // int32_t(AJAProcess::GetPid()) — match that.
  const int32_t pid = static_cast<int32_t>(AJAProcess::GetPid());
  if(!m_card->AcquireStreamForApplication(
         NTV2_FOURCC('s', 'c', 'o', 'r'), pid))
  {
    qWarning() << "AJA: another app holds the device";
    m_card.reset();
    return false;
  }
  m_card->GetEveryFrameServices(m_savedTaskMode);
  m_card->SetEveryFrameServices(NTV2_OEM_TASKS);
  m_taskModeSaved = true;
  if(m_card->features().CanDoMultiFormat())
    m_card->SetMultiFormatMode(false);

  qDebug() << "AJA: Opened device" << QString::fromStdString(m_card->GetDisplayName())
           << "format" << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat))
           << "buffer" << QString::fromStdString(::NTV2FrameBufferFormatToString(m_bufferFormat))
           << "channel" << int(m_channel) << "mode8K" << int(m_settings.mode8K);

  // Determine topology from the chosen video format and 8K mode bit.
  // The mode8K enum is misnamed historically: it really means "quad
  // routing strategy" and is consulted for any 4K+ format. For sub-4K
  // formats it's ignored.
  m_isQuadQuad = NTV2_IS_QUAD_QUAD_FORMAT(m_videoFormat);
  m_is4K = NTV2_IS_4K_VIDEO_FORMAT(m_videoFormat);
  // Single-link 4K = the format itself is 3840x2160p_* / 4096x2160p_*
  // (NOT 4x...), and the card has 12G crosspoints so we can route the
  // whole signal on one cable.
  const bool isSingleLinkFmt
      = (m_videoFormat >= NTV2_FORMAT_FIRST_UHD_TSI_DEF_FORMAT
         && m_videoFormat < NTV2_FORMAT_END_UHD_TSI_DEF_FORMAT)
        || (m_videoFormat >= NTV2_FORMAT_FIRST_4K_TSI_DEF_FORMAT
            && m_videoFormat < NTV2_FORMAT_END_4K_TSI_DEF_FORMATS);
  m_use12G = m_is4K && !m_isQuadQuad && isSingleLinkFmt
             && m_card->features().CanDo12gRouting();

  // Quad-link routing strategy (12G single-link doesn't use TSI muxers).
  // 4K quad-link always uses TSI: on Kona5 the Squares mode does not
  // round-trip — it scrambles the quadrants horizontally (TL=TR, BL=BR),
  // confirmed against a bare-NTV2 repro (aja_rgb_repro --4k) — whereas TSI
  // round-trips cleanly on every backend (GL/Vulkan/D3D11/D3D12). 8K honors
  // the user's mode8K (TSI vs SQD) choice.
  m_useTSI = !m_use12G
             && ((m_is4K && !m_isQuadQuad)
                 || (m_isQuadQuad && m_settings.mode8K == AJA8KMode::TSI));
  const bool isQQHFR = NTV2_IS_QUAD_QUAD_HFR_VIDEO_FORMAT(m_videoFormat);

  if(m_isQuadQuad && m_channel != NTV2_CHANNEL1 && m_channel != NTV2_CHANNEL5)
  {
    qWarning() << "AJA: 8K mode requires Channel 1 or Channel 5 as base channel";
    m_channel = NTV2_CHANNEL1;
  }
  if(m_isQuadQuad && !m_card->features().CanDo8KVideo())
  {
    qWarning() << "AJA: Device does not support 8K video";
    shutdownAJADevice();
    return false;
  }
  if(!m_card->features().CanDoVideoFormat(m_videoFormat))
  {
    qWarning() << "AJA: device cannot handle video format"
               << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat));
    shutdownAJADevice();
    return false;
  }
  if(!m_card->features().CanDoFrameBufferFormat(m_bufferFormat))
  {
    qWarning() << "AJA: device cannot handle buffer format"
               << QString::fromStdString(
                      ::NTV2FrameBufferFormatToString(m_bufferFormat));
    shutdownAJADevice();
    return false;
  }
  if(!ajaFbfPlayoutCapable(m_bufferFormat))
  {
    // The framestore stores these layouts fine but the playout path
    // rasters BLACK (capture/codec-side formats) — fail loudly instead.
    qWarning() << "AJA: buffer format"
               << QString::fromStdString(
                      ::NTV2FrameBufferFormatToString(m_bufferFormat))
               << "is capture-only on this hardware (plays out black)";
    shutdownAJADevice();
    return false;
  }

  // Pick frame-store and SDI counts from topology.
  UWord fbCount = 1, sdiCount = 1;
  if(m_use12G)
  {
    fbCount = 1;
    sdiCount = 1;
  }
  else if(m_isQuadQuad)
  {
    if(m_useTSI)
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
  else if(m_is4K)
  {
    if(m_useTSI)
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
  // 12G/6G single-link: framestore and SDI spigot are DIFFERENT channels.
  // The single-link ≥6G PHY lives on connector 3 (AJA's own demos hardcode
  // NTV2_CHANNEL3 for the link-grouped path; retail FW puts the 2081-10
  // VPID there), while the raster must live in a real framestore — the
  // Kona5-12Bit personality exposes only 2 framestores for 4 spigots, so
  // FS3 silently doesn't exist (enables no-op, no input VBIs, AC parks in
  // STARTING forever). Decouple: FS[m_channel] + SDIOut3.
  if(m_use12G
     && UWord(m_channel) >= m_card->features().GetNumFrameStores())
  {
    qWarning() << "AJA: channel" << (int(m_channel) + 1)
               << "is not a framestore on this firmware; using channel 1";
    m_channel = NTV2_CHANNEL1;
  }
  m_activeFrameStores = ::NTV2MakeChannelSet(m_channel, fbCount);
  if(m_use12G)
    m_activeSDIs = NTV2ChannelSet{NTV2_CHANNEL3};
  else
    m_activeSDIs = ::NTV2MakeChannelSet(m_channel, sdiCount);
  qDebug() << "AJA: topology fbCount=" << fbCount << "sdiCount=" << sdiCount
           << "is4K=" << m_is4K << "isQuadQuad=" << m_isQuadQuad
           << "use12G=" << m_use12G << "useTSI=" << m_useTSI
           << "isQQHFR=" << isQQHFR;

  // From here on, the order mirrors ntv2player8k.cpp's SetUpVideo +
  // RouteOutputSignal almost exactly. Deviating from the demo order
  // produced an SMPTE-test-pattern emission instead of our content
  // on 8K SQD — the card needs the QuadQuad mode bits set BEFORE
  // SetFrameBufferFormat or the FB ends up in the wrong geometry.
  //
  // Demo sequence (ntv2player8k.cpp:224-246):
  //   EnableChannels -> SetVANCMode -> SetVideoFormat ->
  //   SetQuadQuadFrameEnable + SetQuadQuadSquaresEnable ->
  //   SetFrameBufferFormat -> SubscribeOutputVerticalEvent ->
  //   SetReference. Then RouteOutputSignal builds connections,
  //   SetSDITransmitEnable + level conversions per SDI, then
  //   ApplySignalRoute(replace=true).

  m_card->EnableChannels(m_activeFrameStores, /*disableOthers=*/true);
  m_card->SetVANCMode(m_activeFrameStores, NTV2_VANCMODE_OFF);

  if(!m_card->SetVideoFormat(m_videoFormat, false, false, m_channel))
  {
    qWarning() << "AJA: SetVideoFormat failed for"
               << QString::fromStdString(::NTV2VideoFormatToString(m_videoFormat));
    shutdownAJADevice();
    return false;
  }

  // Mode bits MUST come right after SetVideoFormat, before
  // SetFrameBufferFormat. SetFrameBufferFormat looks at the QuadQuad
  // / 4K-Squares / TSI mode bits to size + lay out the FB; setting
  // FBF first (in non-QuadQuad mode) leaves the FB in the wrong
  // geometry and the routing emits test pattern.
  if(m_isQuadQuad)
  {
    m_card->SetQuadQuadFrameEnable(true, m_channel);
    m_card->SetQuadQuadSquaresEnable(!m_useTSI, m_channel);
  }
  else
  {
    m_card->SetQuadQuadFrameEnable(false, m_channel);
    if(m_use12G)
    {
      // 12G single-link 4K: activate the framestore's built-in TSI mux. The
      // always-on 12G serializer 2-sample-interleaves the wire; without the
      // framestore mux the host raster and wire order disagree and the image
      // is spatially scrambled. (ntv2player4k enables TSI for CanDo12gRouting.)
      m_card->SetTsiFrameEnable(true, m_channel);
      m_card->Set4kSquaresEnable(false, m_channel);
    }
    else if(m_is4K)
    {
      m_card->SetTsiFrameEnable(m_useTSI, m_channel);
      m_card->Set4kSquaresEnable(!m_useTSI, m_channel);
    }
    else
    {
      // single-link sub-4K: clear leftover 4K mode bits from a prior route.
      m_card->SetTsiFrameEnable(false, m_channel);
      m_card->Set4kSquaresEnable(false, m_channel);
    }
  }

  m_card->SetFrameBufferFormat(m_activeFrameStores, m_bufferFormat);

  for(NTV2Channel ch : m_activeFrameStores)
  {
    m_card->SetMode(ch, NTV2_MODE_DISPLAY);
    // Top-down memory layout matches our QRhi readback (encoder
    // writes row 0 at byte 0). Without setting this, the card may
    // default to bottom-up and display the image flipped on SDI.
    m_card->SetFrameBufferOrientation(ch, NTV2_FRAMEBUFFER_ORIENTATION_TOPDOWN);
  }

  // Per-SDI standard register: only meaningful for sub-4K formats.
  // The 4K and 8K demos don't call SetSDIOutputStandard — for SQD/TSI
  // each cable carries one sub-frame quadrant and the per-cable
  // standard is derived from SetVideoFormat + mode bits.
  if(!m_is4K && !m_isQuadQuad && !m_use12G)
  {
    const NTV2Standard standard
        = ::GetNTV2StandardFromVideoFormat(m_videoFormat);
    for(NTV2Channel ch : m_activeSDIs)
    {
      m_card->SetSDIOutputStandard(ch, standard);
    }
  }

  // Windows-mandatory output VBI subscriptions. Without these,
  // WaitForOutputVerticalInterrupt in the consumer thread can stall
  // when score isn't the first app to touch the card after boot.
  // Must happen BEFORE AutoCirculateInit per the demos.
  for(NTV2Channel ch : m_activeFrameStores)
  {
    m_card->EnableOutputInterrupt(ch);
    m_card->SubscribeOutputVerticalEvent(ch);
  }

  // Make register changes (SetOutputFrame in particular) latch on the
  // next VBI rather than mid-frame — eliminates a class of tearing
  // artifacts.
  m_card->SetRegisterWriteMode(NTV2_REGWRITE_SYNCTOFRAME);

  // Free-running clock for output. (Could be a setting later if
  // anyone wants genlock to an SDI input.)
  m_card->SetReference(NTV2_REFERENCE_FREERUN);

  // Build the connections set — describes which crosspoints the
  // route will wire up but doesn't write registers yet.
  NTV2XptConnections connections;
  buildSignalRoute(connections);

  // Bidirectional SDI transmit + per-SDI level conversions, mirroring
  // ntv2player8k.cpp:444-455. Force broadcast Level-A 3G (the
  // default for receivers without Level-B parsing).
  if(::NTV2DeviceHasBiDirectionalSDI(m_card->GetDeviceID()))
  {
    m_card->SetSDITransmitEnable(m_activeSDIs, true);
  }
  for(NTV2Channel ch : m_activeSDIs)
  {
    m_card->SetSDIOutLevelAtoLevelBConversion(ch, false);
    m_card->SetSDIOutRGBLevelAConversion(ch, false);
  }
  // Per-cable SDI bit rate. Enable 12G for single-link 4K and 8K; explicitly
  // disable 12G/6G for everything else. The disable matters: a spigot left in
  // 12G mode by a prior 4K/8K run (the harness exits via std::_Exit(), so
  // teardown never runs) would otherwise transmit an HD/SD raster as a
  // malformed 12G signal — the receiver then misdetects it (e.g. 1080p60a sent
  // as "1080p60b @ 30") and captures garbage. Setting both bits every time
  // makes output setup self-sufficient against stale card state.
  // NOTE (review 2026-07, kept as-is deliberately): the demos program 12G
  // differently — ntv2player4k touches only CH3 and only with link grouping;
  // plain quad-link 3G leaves both bits off. This exact code (12G bit on all
  // active spigots, incl. quad-link 4x1920x1080p) passed the full hardware
  // round-trip on the Kona 5 rig, so it is NOT changed on review alone. If a
  // third-party quad-link 3G receiver ever misparses the per-cable rate,
  // restrict to `m_use12G || m_isQuadQuad` and re-run AJARoundtrip.
  const bool want12G = m_use12G || m_isQuadQuad || m_is4K;
  for(NTV2Channel ch : m_activeSDIs)
  {
    m_card->SetSDIOut6GEnable(ch, false);
    m_card->SetSDIOut12GEnable(ch, want12G);
  }

  if(!m_card->ApplySignalRoute(connections, /*replace=*/true))
  {
    // Some firmware crosspoint-connect ROMs don't enumerate the
    // RGB-framestore -> CSC -> SDI connections even though the hardware
    // fully supports them, so the validated ApplySignalRoute rejects the
    // whole route and (previously) aborted device init. Re-apply without
    // ROM validation, matching the AJA demos' warn-and-continue Connect
    // loop (ntv2player::RouteOutputSignal).
    qWarning() << "AJA: ApplySignalRoute validation failed; re-applying route "
                  "without crosspoint-ROM validation";
    m_card->ClearRouting();
    bool anyFail = false;
    for(const auto& [in, out] : connections)
      if(!m_card->Connect(in, out, /*inValidate=*/false))
        anyFail = true;
    if(anyFail)
      qWarning() << "AJA: some crosspoint connections were rejected";
  }

  // An RGB framebuffer reaches the SDI spigot through a CSC (RGB -> YCbCr).
  // The CSC method register can power up as "Unimplemented" — a no-op that
  // emits black — so explicitly select a valid method and tell the CSC our
  // GPU-rendered RGB is full-range (0-255), not SMPTE-narrow.
  if(!isYCbCr(m_bufferFormat))
  {
    // Match the AJA demos (ntv2player / ntv2outputtestpattern): leave the CSC
    // in its power-on default (Original) mode, which has correct built-in
    // RGB->YUV coefficients. Switching to Enhanced selects an unprogrammed
    // enhanced matrix and emits black. Just declare the host RGB full-range
    // (0-255) and disable alpha-from-key.
    for(NTV2Channel ch : m_activeFrameStores)
    {
      m_card->SetColorSpaceRGBBlackRange(NTV2_CSC_RGB_RANGE_FULL, ch);
      m_card->SetColorSpaceMakeAlphaFromKey(false, ch);
    }
  }

  // VPID payload for HDR signaling (and SDR transfer-char correctness).
  // Written AFTER routing — the SDI transmitter latches the VPID on
  // each frame.
  writeOutputVPIDs();

  // RP188 timecode output config. Each active SDI emits ATC LTC on
  // every frame from the per-frame xfer.SetOutputTimeCodes payload
  // populated by AJAConsumerThread.
  for(NTV2Channel ch : m_activeSDIs)
  {
    m_card->SetRP188Mode(ch, NTV2_RP188_OUTPUT);
    m_card->DisableRP188Bypass(ch);
  }

  m_formatDesc = NTV2FormatDescriptor(m_videoFormat, m_bufferFormat);
  m_frameBufferSize = m_formatDesc.GetTotalBytes();

  // HDR metadata, two channels:
  //   1. HDMI: program the AJA HDR registers + EnableHDMIHDR(true).
  //   2. SDI:  build an HDR Static Metadata Descriptor ANC packet once,
  //            and ask AutoCirculate to insert it on every output frame.
  // RP188 timecode is always on — the consumer thread populates a
  // free-running counter into xfer.SetOutputTimeCodes per VBI.
  ULWord acOptions = AUTOCIRCULATE_WITH_RP188;
  if(m_settings.hdrMode != AJAHDRMode::Off)
  {
    HDRRegValues hdr;
    hdr.setBT2020();
    if(m_settings.hdrMode == AJAHDRMode::HLG)
      hdr.electroOpticalTransferFunction = 3; // ARIB STD-B67 / HLG
    else
      hdr.electroOpticalTransferFunction = 2; // SMPTE ST 2084 / PQ

    if(m_card->features().CanDoHDMIHDROut())
    {
      m_card->SetHDRData(hdr);
      m_card->SetHDMIHDRElectroOpticalTransferFunction(
          hdr.electroOpticalTransferFunction);
      m_card->SetHDMIHDRConstantLuminance(false);
      m_card->EnableHDMIHDRDolbyVision(false);
      m_card->EnableHDMIHDR(true);
    }

    // SDI: build a Static Metadata Descriptor ANC packet (DID=0xC0, SID=0x00)
    // for HDR10 (PQ) or HLG. AJA's anc helpers ship the standard payload.
    if(m_card->features().CanDoCustomAnc())
    {
      // Build the *device* ANC buffer (GUMP) the AutoCirculate inserter
      // expects, via AJAAncillaryList::GetTransmitData — the ntv2ccplayer
      // pattern. A single packet's GenerateTransmitData is NOT that format
      // and is silently not transmitted (the receiver sees no DID=0xC0).
      AJAAncillaryList packetList;
      AJAStatus st = AJA_STATUS_FAIL;
      // Don't override the data location: the HDR packet's own Init() already
      // sets AJA's canonical VANC location (link A, luma, line 16, AnyVanc
      // horizontal offset). An earlier override to line 9 used the 4-arg
      // AJAAncDataLoc ctor, which leaves the horizontal offset at a *fixed* 0
      // instead of AnyVanc — placing the packet outside the inserter's VANC
      // slot. Keep the default so the inserter emits it.
      if(m_settings.hdrMode == AJAHDRMode::HLG)
      {
        AJAAncillaryData_HDR_HLG pkt;
        st = packetList.AddAncillaryData(pkt);
      }
      else
      {
        AJAAncillaryData_HDR_HDR10 pkt;
        st = packetList.AddAncillaryData(pkt);
      }
      if(AJA_SUCCESS(st))
      {
        constexpr uint32_t kAncSize = 0x2000; // standard F1 ANC region
        m_hdrAncBuffer.assign(kAncSize / 4, 0);
        NTV2Buffer f1Buf(m_hdrAncBuffer.data(), kAncSize);
        NTV2Buffer f2Buf; // progressive: no field 2
        const bool isProgressive = NTV2_IS_PROGRESSIVE_STANDARD(
            ::GetNTV2StandardFromVideoFormat(m_videoFormat));
        if(AJA_SUCCESS(packetList.GetTransmitData(f1Buf, f2Buf, isProgressive, 0)))
        {
          m_card->DMABufferLock(
              m_hdrAncBuffer.data(), kAncSize, /*inMap=*/true, /*inRDMA=*/false);
          acOptions |= AUTOCIRCULATE_WITH_ANC;
        }
      }
    }
  }
  else if(m_card->features().CanDoHDMIHDROut())
  {
    m_card->EnableHDMIHDR(false);
  }

  // Set up AutoCirculate output ring. The card schedules SDI output from
  // these frames at the SDI clock; the VBI-paced AJAConsumerThread pushes
  // into the ring with AutoCirculateTransfer.
  m_card->AutoCirculateStop(m_channel);
  // ntv2player waits a few VBIs after Stop before re-initialising ("Let it
  // stop") so the Init doesn't land on a not-yet-idle channel.
  m_card->WaitForOutputVerticalInterrupt(m_channel, 4);
  if(!m_card->AutoCirculateInitForOutput(
         m_channel,
         /*inFrameCount=*/kFrameCount,
         /*inAudioSystem=*/NTV2_AUDIOSYSTEM_INVALID,
         /*inOptionFlags=*/acOptions,
         /*inNumChannels=*/1))
  {
    qWarning() << "AJA: AutoCirculateInitForOutput failed";
    // Full teardown, like every other post-acquire failure path: dropping the
    // card object alone would leave the device acquired by our (now dead)
    // 'scor' claim and stuck in OEM task mode until manual recovery.
    shutdownAJADevice();
    return false;
  }

  m_deviceInitialized = true;
  qDebug() << "AJA: Device initialized, frame buffer size:"
           << m_frameBufferSize
           << "topology:"
           << (m_use12G    ? "12G single-link"
               : m_isQuadQuad ? (m_useTSI ? "8K TSI" : "8K SQD")
               : m_is4K       ? (m_useTSI ? "4K TSI" : "4K SQD/single")
                              : "single-link HD/SD");
  return true;
}

void AjaOutputBackend::shutdownAJADevice()
{
  if(!m_card)
  {
    m_deviceInitialized = false;
    return;
  }

  // The VBI consumer is owned by AJANode; it is stopped in destroyOutput()
  // before this runs, so the card is idle here.
  // Ground-truth wire cadence: the card's own frame counters (identical to
  // ntv2capture.cpp:503's GetProcessedFrameCount). acFramesProcessed = frames
  // the card actually clocked to the SDI wire since AutoCirculateStart,
  // independent of any host-side render/readback rate. SCORE_AJA_ACSTATS=1.
  if(std::getenv("SCORE_AJA_ACSTATS"))
  {
    AUTOCIRCULATE_STATUS st;
    if(m_card->AutoCirculateGetStatus(m_channel, st))
      qDebug().nospace()
          << "AJA-ACSTATS out: processed=" << st.GetProcessedFrameCount()
          << " dropped=" << st.GetDroppedFrameCount()
          << " level=" << st.GetBufferLevel();
  }
  m_card->AutoCirculateStop(m_channel);

  // Drop HDR registers + ANC injection.
  if(m_card->features().CanDoHDMIHDROut())
    m_card->EnableHDMIHDR(false);

  // Drop output VBI subscriptions on all active framestores so
  // repeated reconnect cycles don't leak kernel-side event records.
  for(NTV2Channel ch : m_activeFrameStores)
    m_card->UnsubscribeOutputVerticalEvent(ch);

  // Bidirectional SDI: flip output spigots back to receive.
  if(::NTV2DeviceHasBiDirectionalSDI(m_card->GetDeviceID())
     && m_activeSDIs.size() > 0)
  {
    m_card->SetSDITransmitEnable(m_activeSDIs, false);
  }

  // Clear quad/quad-quad/TSI mode bits so a follow-on app starts
  // from a clean state.
  if(m_isQuadQuad)
  {
    m_card->SetQuadQuadFrameEnable(false, m_channel);
    m_card->SetQuadQuadSquaresEnable(false, m_channel);
  }
  if(m_is4K)
  {
    m_card->SetTsiFrameEnable(false, m_channel);
    m_card->Set4kSquaresEnable(false, m_channel);
  }

  // Release every page-locked DMA buffer the kernel is holding for
  // us (encoder readback staging, ANC packet, RDMA imports).
  m_card->DMABufferUnlockAll();

  // Hand the retail-services daemon back its frame-cycle hooks so
  // a downstream non-OEM app sees the card as it expects.
  if(m_taskModeSaved)
  {
    m_card->SetEveryFrameServices(m_savedTaskMode);
    m_taskModeSaved = false;
  }

  m_card->ReleaseStreamForApplication(
      NTV2_FOURCC('s', 'c', 'o', 'r'),
      static_cast<int32_t>(AJAProcess::GetPid()));
  m_card->Close();
  m_card.reset();

  m_activeFrameStores.clear();
  m_activeSDIs.clear();
  m_use12G = false;
  m_useTSI = false;
  m_is4K = false;
  m_isQuadQuad = false;
  m_deviceInitialized = false;
  // Per-session AutoCirculate state: each AutoCirculateInitForOutput needs
  // its own AutoCirculateStart (the demos' start gate is a per-run local).
  // Stale values here made a reopened backend never start circulation: the
  // ring filled, CanAcceptMoreOutputFrames() went false and the pump blocked
  // forever on a black output.
  m_acStarted = false;
  m_acGoodXfers = 0;
  m_outputFrame = 0;
}

void AjaOutputBackend::buildSignalRoute(NTV2XptConnections& conns)
{
  if(!m_card)
    return;

  const bool isRGB = !isYCbCr(m_bufferFormat);
  const bool isQQHFR = NTV2_IS_QUAD_QUAD_HFR_VIDEO_FORMAT(m_videoFormat);

  // ──────────────────────────────────────────────────────────────
  // 12G single-link (one cable carrying 4K UHD or DCI-4K).
  // YCbCr: SDI[ch] ← FBOut[ch]
  // RGB:   CSCIn[ch] ← FBOut[ch] (RGB), SDI[ch] ← CSCOut[ch] (YUV-on-wire)
  // The card's transmitter handles 12G framing internally given
  // SetSDIOut12GEnable + SetVideoFormat were set.
  // ──────────────────────────────────────────────────────────────
  if(m_use12G)
  {
    // Spigot ≠ framestore for single-link ≥6G (see topology setup).
    const NTV2Channel spigot
        = m_activeSDIs.empty() ? m_channel : *m_activeSDIs.begin();
    const NTV2OutputCrosspointID fbOut
        = ::GetFrameBufferOutputXptFromChannel(m_channel, isRGB, false);
    const NTV2InputCrosspointID sdiIn = ::GetSDIOutputInputXpt(spigot, false);
    if(isRGB)
    {
      const NTV2InputCrosspointID cscIn = ::GetCSCInputXptFromChannel(m_channel);
      const NTV2OutputCrosspointID cscOut
          = ::GetCSCOutputXptFromChannel(m_channel, /*Key=*/false, /*RGB=*/false);
      conns.insert({cscIn, fbOut});
      conns.insert({sdiIn, cscOut});
    }
    else
    {
      conns.insert({sdiIn, fbOut});
    }
    return;
  }

  // ──────────────────────────────────────────────────────────────
  // SQD (single-link HD/SD/3G or quad-link 4K/8K Squares).
  // For each frame store i in [0..fbCount): SDI[ch+i] ← FBOut[ch+i]
  // (or via per-channel CSC for RGB).
  // ──────────────────────────────────────────────────────────────
  if(!m_useTSI)
  {
    UWord i = 0;
    for(NTV2Channel ch : m_activeFrameStores)
    {
      const NTV2OutputCrosspointID fbOut
          = ::GetFrameBufferOutputXptFromChannel(ch, isRGB, false);
      const NTV2InputCrosspointID sdiIn = ::GetSDIOutputInputXpt(ch, false);
      if(isRGB)
      {
        const NTV2InputCrosspointID cscIn = ::GetCSCInputXptFromChannel(ch);
        const NTV2OutputCrosspointID cscOut = ::GetCSCOutputXptFromChannel(
            ch, /*Key=*/false, /*RGB=*/false);
        conns.insert({cscIn, fbOut});
        conns.insert({sdiIn, cscOut});
      }
      else
      {
        conns.insert({sdiIn, fbOut});
      }
      ++i;
    }
    return;
  }

  // ──────────────────────────────────────────────────────────────
  // TSI 4K YUV (4 SDIs through 2 muxers, 2 frame stores).
  //   for path = 0..3:
  //     muxIn(mux=path/2, LinkB=path&1) ← FBOut(fb=path/2, B=path&1)
  //     SDI[path]                       ← muxOut(mux=path/2, LinkB=path&1)
  // ──────────────────────────────────────────────────────────────
  if(m_is4K && !m_isQuadQuad)
  {
    if(isRGB)
    {
      // RGB TSI 4K uses DLOut muxers — not implemented (score's output
      // formats are YUV-first). Receivers requesting RGB-on-wire 4K
      // need a CSC chain we haven't wired yet.
      qWarning() << "AJA: RGB TSI 4K output routing is not implemented";
      return;
    }
    for(UWord path = 0; path < 4; ++path)
    {
      const auto fbCh = static_cast<NTV2Channel>(m_channel + path / 2);
      const auto sdiCh = static_cast<NTV2Channel>(m_channel + path);
      const bool isB = (path & 1) != 0;
      const NTV2InputCrosspointID muxIn
          = ::GetTSIMuxInputXptFromChannel(fbCh, /*LinkB=*/isB);
      const NTV2OutputCrosspointID fbOut
          = ::GetFrameBufferOutputXptFromChannel(
              fbCh, /*RGB=*/false, /*Quarter=*/isB);
      conns.insert({muxIn, fbOut});

      const NTV2InputCrosspointID sdiIn
          = ::GetSDIOutputInputXpt(sdiCh, /*DS2=*/false);
      const NTV2OutputCrosspointID muxOut = ::GetTSIMuxOutputXptFromChannel(
          fbCh, /*LinkB=*/isB, /*RGB=*/false);
      conns.insert({sdiIn, muxOut});
    }
    return;
  }

  // ──────────────────────────────────────────────────────────────
  // TSI 8K (QuadQuad). HFR uses 4 SDIs (4 paths × 1 cable each); non-HFR
  // uses 2 SDIs interleaved across DS1/DS2 (4 paths × 2 cables). On 8K
  // the card bypasses the TSI muxer crosspoints — each FB DS-output
  // wires straight to an SDI spigot DS1/DS2.
  //   for path = 0..3:
  //     fbCh = master + path/2
  //     isB  = path & 1
  //     HFR:    SDI[master+path, DS2=false] ← FBOut[fbCh, RGB, B=isB]
  //     ! HFR:  SDI[master+path/2, DS2=isB] ← FBOut[fbCh, RGB, B=isB]
  // ──────────────────────────────────────────────────────────────
  if(m_isQuadQuad)
  {
    if(isRGB)
    {
      qWarning() << "AJA: RGB TSI 8K output routing is not implemented";
      return;
    }
    for(UWord path = 0; path < 4; ++path)
    {
      const auto fbCh = static_cast<NTV2Channel>(m_channel + path / 2);
      const bool isB = (path & 1) != 0;
      const NTV2OutputCrosspointID fbOut
          = ::GetFrameBufferOutputXptFromChannel(
              fbCh, /*RGB=*/false, /*Quarter=*/isB);
      NTV2Channel sdiCh;
      bool sdiDS2;
      if(isQQHFR)
      {
        sdiCh = static_cast<NTV2Channel>(m_channel + path);
        sdiDS2 = false;
      }
      else
      {
        sdiCh = static_cast<NTV2Channel>(m_channel + path / 2);
        sdiDS2 = isB;
      }
      const NTV2InputCrosspointID sdiIn = ::GetSDIOutputInputXpt(sdiCh, sdiDS2);
      conns.insert({sdiIn, fbOut});
    }
    return;
  }
}

void AjaOutputBackend::writeOutputVPIDs()
{
  if(!m_card)
    return;

  // Build VPID payload bytes A and B from the active video format,
  // buffer format, and HDR mode. The card derives a default VPID
  // from SetVideoFormat for standard SDR cases — we override for HDR
  // (transfer characteristic + colorimetry) and for the cases where
  // the driver-default is wrong (notably DCI 4K and Level-B 3G).
  ULWord vpidA = 0, vpidB = 0;
  if(!m_card->GetSDIOutVPID(vpidA, vpidB, static_cast<UWord>(m_channel)))
    return;

  CNTV2VPID vpid(vpidA);

  // Transfer characteristic.
  switch(m_settings.hdrMode)
  {
    case AJAHDRMode::HDR10:
      vpid.SetTransferCharacteristics(NTV2_VPID_TC_PQ);
      vpid.SetColorimetry(NTV2_VPID_Color_UHDTV);
      break;
    case AJAHDRMode::HLG:
      vpid.SetTransferCharacteristics(NTV2_VPID_TC_HLG);
      vpid.SetColorimetry(NTV2_VPID_Color_UHDTV);
      break;
    case AJAHDRMode::Off:
    default:
      vpid.SetTransferCharacteristics(NTV2_VPID_TC_SDR_TV);
      // Colorimetry left as-is (driver-default depends on resolution).
      break;
  }

  // NB: an RGB framebuffer goes through a CSC before the SDI spigot, so the
  // wire carries YCbCr 4:2:2 — the framebuffer being RGB must NOT set an
  // RGB-range/sampling VPID flag here (that mislabels the YUV stream).

  vpidA = vpid.GetVPID();

  // Write to every active SDI spigot. For multi-link formats each
  // cable carries its own VPID; for SQD the per-link VPID encodes
  // the link's quadrant / DS pairing — we let the driver handle the
  // link-specific bytes by writing the same payload to each (the
  // card's transmitter overlays the link-channel field).
  for(NTV2Channel ch : m_activeSDIs)
  {
    m_card->SetSDIOutVPID(vpidA, vpidB, static_cast<UWord>(ch));
  }
}

// =============================================================================
// Rendering
// =============================================================================


} // namespace Gfx::AJA
