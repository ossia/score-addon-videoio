#include "AJAInputNode.hpp"

#include <AJA/AJACaptureSession.hpp>
#include <AJA/AjaFormatMap.hpp>
#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/decoders/GPUVideoDecoder.hpp>
#include <Gfx/Graph/decoders/RGBA.hpp>
#include <Gfx/Graph/decoders/V210.hpp>
#include <Gfx/Graph/decoders/YUYV422.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <ntv2card.h>
#include <ntv2publicinterface.h>

#include <QDebug>

#include <cstdlib>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#if defined(SCORE_HAS_AJA_DVP_BRIDGE)
#include <AJA/AjaDmaLockPolicy.hpp>
#if defined(_WIN32)
#include <Gfx/Graph/interop/DvpCaptureD3D11.hpp>
#define AJA_HAS_CAPTURE_DVP_D3D11 1
#endif
// GL DVP ("GPUDirect for Video") is cross-platform: Windows dvp.dll,
// Linux libdvp.so.1. sysmem DMA + dvpMemcpy into the GL texture — no
// GPUDirect RDMA / nvidia-peermem needed, so it works on consumer GPUs.
#if QT_CONFIG(opengl)
#include <Gfx/Graph/interop/DvpCaptureGl.hpp>
#define AJA_HAS_CAPTURE_DVP_GL 1
#endif
#endif

#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && defined(_WIN32)
#include <AJA/AjaRdmaCaptureD3D11.hpp>
#define AJA_HAS_RDMA_CAPTURE_D3D11 1
#endif
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && QT_CONFIG(opengl)
#include <AJA/AjaRdmaCaptureGL.hpp>
#define AJA_HAS_RDMA_CAPTURE_GL 1
#endif
#if defined(SCORE_HAS_AJA_CUDA_BRIDGE) && (QT_HAS_VULKAN || (QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)))
#include <AJA/AjaRdmaCaptureVulkan.hpp>
#define AJA_HAS_RDMA_CAPTURE_VULKAN 1
#endif

// Portable CPU-staging capture fallback (no DVP / RDMA deps): raw
// glTexSubImage2D on OpenGL, QRhi uploadTexture on Vulkan/Metal/D3D. Used
// when no GPU-direct strategy can initialize; works on every backend.
#include <AJA/AjaCpuCapture.hpp>

namespace Gfx::AJA
{
namespace
{

/**
 * @brief AJA card opener + capture thread for the GPU-direct path.
 *
 * Composes a `CaptureSession` for the AJA bookkeeping (open, channel
 * setup with quad-link / TSI / 12G routing, VPID HDR detection,
 * signal-change reroute, ANC + timecode metadata) and adds a per-VBI
 * loop that DMAs sysmem -> GPU texture via a VideoCaptureStrategy.
 *
 * Per-VBI loop:
 *   1. WaitForInputVerticalInterrupt
 *   2. detectAndApplyFormatChange every ~30 VBIs (cable swap / mode
 *      change). On change AC is reinit'd; the slot ring is reset so
 *      the renderer doesn't sample mid-transition.
 *   3. AutoCirculateGetStatus -> HasAvailableInputFrame?
 *   4. AutoCirculateTransfer DMAs SDI -> the strategy's page-locked
 *      sysmem slot (one of N pre-registered DVP buffers).
 *   5. session.readPerFrameMetadata: extracts RP188 + ANC + SDI link
 *      status into the session's atomic fields.
 *   6. strategy->ingestFrame(slot) DMAs sysmem -> GPU texture via DVP
 *      (Windows) or RDMA (future Linux). Publish frame id atomically.
 *
 * The renderer thread polls the slot ring and brackets sampling with
 * acquireForRender / releaseAfterRender (DVP API/DVP semaphore
 * handshake). No FrameQueue / AVFrame on this path.
 */
class aja_gpu_capture
{
public:
  aja_gpu_capture(
      const AJAInputSettings& s, score::gfx::interop::VideoCaptureSlotRing& ring)
      : m_session{s}
      , m_ring{ring}
  {
  }

  /// Strategy is plumbed in *after* the card has been opened, since AJA
  /// strategies hold the card pointer as a member and we want a single
  /// place that owns the card handle (aja_gpu_capture / CaptureSession).
  void setStrategy(score::gfx::interop::VideoCaptureStrategy* s) noexcept
  {
    m_strategy = s;
  }

  ~aja_gpu_capture()
  {
    stop();
    if(m_acStarted)
    {
      if(auto* card = m_session.card())
        card->AutoCirculateStop(m_session.masterChannel());
      m_acStarted = false;
    }
    m_session.close();
  }

  bool open()
  {
    if(!m_session.open())
      return false;
    if(!m_session.setupChannel())
    {
      m_session.close();
      return false;
    }

    // v210 row stride is aligned to a multiple-of-48-pixels boundary;
    // the V210Decoder texture geometry assumes width % 6 == 0 (and in
    // practice width % 48 == 0). Refuse the strategy up front
    // otherwise — we'd need pixel padding inside the texture row
    // which the shader doesn't model. Common SDI formats (1920, 3840,
    // 7680) are all multiples of 48; DCI 4K (4096) and 2K (2048) are
    // not, and would need a v210 row-padding shader to support.
    if(m_session.settings().pixelFormat == AJAInputPixelFormat::YCbCr10
       && (m_session.width() % 48) != 0)
    {
      qWarning() << "AJA GPU input: v210 requires width % 48 == 0; got"
                 << m_session.width();
      m_session.close();
      return false;
    }

    if(!initAutoCirculate())
    {
      m_session.close();
      return false;
    }
    return true;
  }

  int width() const noexcept { return m_session.width(); }
  int height() const noexcept { return m_session.height(); }
  uint32_t frameByteSize() const noexcept { return m_session.frameSize(); }
  CNTV2Card* card() const noexcept { return m_session.card(); }
  NTV2Channel channel() const noexcept { return m_session.masterChannel(); }
  CaptureSession& session() noexcept { return m_session; }
  Video::ImageFormat imageFormat() const { return m_session.imageFormat(); }

  void start()
  {
    if(m_running.exchange(true))
      return;
    m_thread = std::thread{[this] { runLoop(); }};
  }

  void stop()
  {
    if(!m_running.exchange(false))
      return;
    if(m_thread.joinable())
      m_thread.join();
    // Ground-truth capture cadence: the input card's own frame counter
    // (acFramesProcessed = frames the card DMA-captured off the wire since
    // AutoCirculateStart), independent of the host readback rate. Matches
    // ntv2capture.cpp:503. SCORE_AJA_ACSTATS=1.
    if(std::getenv("SCORE_AJA_ACSTATS"))
    {
      if(auto* card = m_session.card())
      {
        AUTOCIRCULATE_STATUS st;
        if(card->AutoCirculateGetStatus(m_session.masterChannel(), st))
          qDebug().nospace()
              << "AJA-ACSTATS in(gpu): processed=" << st.GetProcessedFrameCount()
              << " dropped=" << st.GetDroppedFrameCount()
              << " level=" << st.GetBufferLevel();
      }
    }
  }

private:
  bool initAutoCirculate()
  {
    auto* card = m_session.card();
    if(!card)
      return false;
    auto ch = m_session.masterChannel();
    card->AutoCirculateStop(ch);
    if(!card->AutoCirculateInitForInput(
           ch, /*frames=*/7, NTV2_AUDIOSYSTEM_INVALID,
           CaptureSession::kSuggestedAcOptions))
    {
      qWarning() << "AJA GPU input: AutoCirculateInitForInput failed";
      return false;
    }
    if(!card->AutoCirculateStart(ch))
    {
      qWarning() << "AJA GPU input: AutoCirculateStart failed";
      return false;
    }
    m_acStarted = true;
    qDebug() << "AJA GPU input: opened" << m_session.width() << "x"
             << m_session.height();
    return true;
  }

  void runLoop()
  {
    auto* card = m_session.card();
    auto ch = m_session.masterChannel();
    std::size_t writeIdx = 0;
    uint64_t frameId = 0;
    const std::size_t slots = m_strategy->slotCount();
    if(slots == 0)
      return;

    constexpr int kFormatPollPeriod = 30;
    int sinceLastPoll = 0;

    while(m_running.load(std::memory_order_acquire))
    {
      card->WaitForInputVerticalInterrupt(ch);

      if(++sinceLastPoll >= kFormatPollPeriod)
      {
        sinceLastPoll = 0;
        if(m_session.detectAndApplyFormatChange())
        {
          // The session applied the new wire format; its slots are sized to
          // the OLD geometry so we can no longer DMA into them. Publish the
          // new geometry for the render thread to rebuild (release()+init()
          // re-opens this backend at the current wire format) and stop
          // AutoCirculate so nothing writes mismatched-geometry buffers.
          if(m_acStarted)
          {
            card->AutoCirculateStop(ch);
            m_acStarted = false;
          }
          m_ring.publishFormat(m_session.width(), m_session.height(), -1, 0.0);
          break;
        }
      }

      // Drain the input AC ring to the FRESHEST frame. One-transfer-per-VBI
      // lets any backlog accumulated during strategy init persist forever as
      // constant end-to-end latency (one VBI in, one frame out — the level
      // never falls). Measured: 3-4 stuck frames on the DVP path (+~58 ms
      // at 60p), 0-1 on RDMA (the 88-vs-95 ms run variance). Every
      // available frame must still be TRANSFERRED to advance AC, but only
      // the newest is ingested/published; stale ones land in the same slot
      // and are overwritten.
      int drained = 0;
      for(;;)
      {
        AUTOCIRCULATE_STATUS status;
        if(!card->AutoCirculateGetStatus(ch, status))
          break;
        if(!status.IsRunning() || !status.HasAvailableInputFrame())
          break;
        if(++drained > 8) // safety: never spin unbounded inside a VBI slot
          break;

        void* sysmem = m_strategy->slotBuffer(writeIdx);
        if(!sysmem)
          break;

        AUTOCIRCULATE_TRANSFER xfer;
        xfer.SetVideoBuffer(
            reinterpret_cast<ULWord*>(sysmem), m_session.frameSize());
        m_session.attachAncToTransfer(xfer);

        if(!card->AutoCirculateTransfer(ch, xfer))
          break;

        m_session.readPerFrameMetadata(xfer);

        // Only the newest frame reaches the GPU: skip ingest when ANOTHER
        // complete frame is already waiting behind this one. Decided on a
        // fresh post-transfer status — the input ring's buffer level counts
        // the frame currently being captured into, so the pre-transfer
        // level cannot distinguish "one ready" from "one ready + backlog".
        AUTOCIRCULATE_STATUS after;
        if(card->AutoCirculateGetStatus(ch, after) && after.IsRunning()
           && after.HasAvailableInputFrame())
          continue;

        // The strategy DMAs sysmem -> GPU texture. Wraps dvpBegin/End
        // internally and CPU-blocks until the DMA completes - so when
        // ingestFrame returns, the texture's bytes are consistent and
        // the renderer can sample on its next tick.
        if(!m_strategy->ingestFrame(writeIdx))
          continue;

        // Publish: bump frame id, store slot. Renderer polls.
        ++frameId;
        m_ring.latestSlot.store(writeIdx, std::memory_order_release);
        m_ring.latestFrameId.store(frameId, std::memory_order_release);

        writeIdx = (writeIdx + 1) % slots;
      }
    }
  }

  CaptureSession m_session;
  score::gfx::interop::VideoCaptureStrategy* m_strategy{};
  score::gfx::interop::VideoCaptureSlotRing& m_ring;
  bool m_acStarted{false};

  std::thread m_thread;
  std::atomic<bool> m_running{false};
};

/**
 * @brief Strategy chain for picking a CaptureInterop based on the
 *        QRhi backend + bridge availability. Mirrors the output
 *        side's strategy chain in AJAOutputNode::createOutput.
 */
std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
pickAjaCaptureStrategy(
    QRhi::Implementation backend, CNTV2Card* card, AJAInputPixelFormat pixfmt)
{
  // Optional override to validate each fallback in isolation:
  //   SCORE_AJA_FORCE_INTEROP = cpu | dvp | rdma   (empty/unset = auto).
  // "cpu" returns null so the GPU-direct node produces nothing (the CPU
  // AVFrame path lives on the device side).
  const QByteArray force = qgetenv("SCORE_AJA_FORCE_INTEROP");
  if(force == "cpu")
    return nullptr;
  const bool allowDvp = force.isEmpty() || force == "dvp";
  const bool allowRdma = force.isEmpty() || force == "rdma";

  // DVP first: "GPUDirect for Video" needs no nvidia-peermem, so it works
  // on consumer GPUs where RDMA (DMABufferLock inRDMA=true) can't.
#if defined(AJA_HAS_CAPTURE_DVP_D3D11)
  if(allowDvp && backend == QRhi::D3D11)
    return std::make_unique<score::gfx::interop::DvpCaptureD3D11<AjaDmaLockPolicy>>(
        AjaDmaLockPolicy{card},
        (pixfmt == AJAInputPixelFormat::ARGB) ? NV_DVP_FORMAT_BGRA8
                                              : NV_DVP_FORMAT_RGBA8,
        "DVP-D3D11");
#endif
#if defined(AJA_HAS_CAPTURE_DVP_GL)
  if(allowDvp && backend == QRhi::OpenGLES2)
    return std::make_unique<score::gfx::interop::DvpCaptureGl<AjaDmaLockPolicy>>(
        AjaDmaLockPolicy{card},
        (pixfmt == AJAInputPixelFormat::ARGB) ? NV_DVP_FORMAT_BGRA8
                                              : NV_DVP_FORMAT_RGBA8,
        "DVP-GL");
#endif
#if defined(AJA_HAS_RDMA_CAPTURE_GL)
  // Linux GL: real RDMA via CUDA-imported GL storage buffer +
  // DMABufferLock(inRDMA=true). init() returns false if the platform
  // doesn't have GPUDirect RDMA available, in which case the caller
  // falls back to AVFrame staging.
  if(allowRdma && backend == QRhi::OpenGLES2)
    return std::make_unique<AjaRdmaCaptureGL>(card, pixfmt);
#endif
#if defined(AJA_HAS_RDMA_CAPTURE_VULKAN)
  // Linux Vulkan: RDMA-Vulkan/T3, a full implementation (the strategy-owned
  // exportable outputTexture contract it was once waiting on is in place).
  // Verified engaging on the Quadro box: AJA capture never reaches the
  // CPU-staging rung there because this one succeeds.
  if(allowRdma && backend == QRhi::Vulkan)
    return std::make_unique<AjaRdmaCaptureVulkan>(card, pixfmt);
#endif
  (void)backend; (void)card; (void)pixfmt;
  return nullptr;
}

} // namespace

namespace
{

/**
 * @brief AJA implementation of the shared DMA capture backend.
 *
 * Wraps the AutoCirculate capture thread (aja_gpu_capture) and supplies the
 * AJA-specific strategy + decoder selection. All the renderer-side machinery
 * lives in score::gfx::DMACaptureInputNode::Renderer.
 */
class AJACaptureBackend final : public score::gfx::DMACaptureBackend
{
public:
  AJACaptureBackend(
      const AJAInputSettings& s,
      score::gfx::interop::VideoCaptureSlotRing& ring)
      : m_capture{s, ring}
      , m_pixfmt{s.pixelFormat}
  {
  }

  bool open() override { return m_capture.open(); }
  int width() const noexcept override { return m_capture.width(); }
  int height() const noexcept override { return m_capture.height(); }
  uint32_t frameByteSize() const noexcept override
  {
    return m_capture.frameByteSize();
  }
  Video::ImageFormat imageFormat() const override
  {
    return m_capture.imageFormat();
  }

  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override
  {
    // Centralised wire-format -> decoder selection (meta carries the VPID-
    // derived colour metadata; the decoder sizes its input texture to the AJA
    // byte layout, the strategy DMAs into it, the shader unpacks at sample time).
    return score::gfx::makeWireDecoder(ajaInputFormatTo(m_pixfmt), meta);
  }

  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  pickStrategy(QRhi::Implementation backend) override
  {
    return pickAjaCaptureStrategy(backend, m_capture.card(), m_pixfmt);
  }

  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  makeCpuStrategy() override
  {
    return std::make_unique<AjaCpuCapture>(m_capture.card(), m_pixfmt);
  }

  void
  setStrategy(score::gfx::interop::VideoCaptureStrategy* s) noexcept override
  {
    m_capture.setStrategy(s);
  }

  void start() override { m_capture.start(); }
  void stop() override { m_capture.stop(); }

private:
  aja_gpu_capture m_capture;
  AJAInputPixelFormat m_pixfmt;
};

} // namespace

AJAInputNode::AJAInputNode(const AJAInputSettings& s)
    : settings{s}
{
  // The Image output port is created by the DMACaptureInputNode base ctor.
}

AJAInputNode::~AJAInputNode() = default;

std::unique_ptr<score::gfx::DMACaptureBackend>
AJAInputNode::makeCaptureBackend(
    score::gfx::interop::VideoCaptureSlotRing& ring) const
{
  return std::make_unique<AJACaptureBackend>(settings, ring);
}

} // namespace Gfx::AJA
