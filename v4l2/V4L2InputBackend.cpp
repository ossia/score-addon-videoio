#include "V4L2InputBackend.hpp"

#include <v4l2/V4L2CpuCapture.hpp>
#include <v4l2/V4L2SyncRig.hpp>

#include <Gfx/Graph/decoders/BayerExternalOES.hpp>
#include <Gfx/Graph/decoders/NV12ExternalOES.hpp>
#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/BorrowedHostImportCapture.hpp>
#include <Gfx/Graph/interop/DmaBufImportCapture.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/V4L2PixelFormat.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <linux/videodev2.h>

#include <QDebug>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace Gfx::V4L2
{
using score::gfx::interop::VideoPixelFormat;

// The fourcc table lives in score-plugin-gfx so the camera enumeration and this
// capture path cannot drift apart; compressed fourccs deliberately resolve to
// Unknown here because this path has no decode stage.
VideoPixelFormat
neutralFromV4L2Fourcc(std::uint32_t f, std::string_view driver) noexcept
{
  const auto n = score::gfx::interop::fromV4L2PixelFormat(f);

  // V4L2 puts the ten significant bits of SRGGB10 & co. in the low bits of a
  // 16-bit word and zeroes the rest. Tegra's VI instead replicates the high
  // bits down into the low six, so a sample spans the whole 16-bit range and
  // 1023 reads as 65535 rather than 1023. Measured on both IMX676 nodes of the
  // Orin NX rig: (s & 63) == (s >> 10) holds for every sample of a frame.
  //
  // That is precisely what the 16-bit orders already describe, so they name
  // these bytes exactly, and the right-aligned 10-bit ones would come out 64x
  // too bright -- a uniformly white frame. Only the two orders with a 16-bit
  // row in the vocabulary can be remapped; no GRBG/GBRG sensor exists on this
  // hardware.
  if(driver == "tegra-video")
  {
    switch(n)
    {
      case VideoPixelFormat::BayerRGGB10:
        return VideoPixelFormat::BayerRGGB16;
      case VideoPixelFormat::BayerBGGR10:
        return VideoPixelFormat::BayerBGGR16;
      default:
        break;
    }
  }
  return n;
}

V4L2InputBackend::V4L2InputBackend(
    V4L2InputSettings settings, score::gfx::interop::VideoCaptureSlotRing& ring)
    : m_settings{std::move(settings)}
    , m_ring{ring}
{
}

V4L2InputBackend::~V4L2InputBackend()
{
  stop();
}

bool V4L2InputBackend::open()
{
  if(!m_session.open(m_settings.device))
  {
    qWarning() << "V4L2: cannot open" << m_settings.device.c_str() << ":"
               << m_session.lastError().c_str();
    return false;
  }

  if(m_settings.width || m_settings.height || m_settings.fourcc)
  {
    if(!m_session.configure(
           m_settings.width, m_settings.height, m_settings.fourcc))
    {
      qWarning() << "V4L2: S_FMT failed:" << m_session.lastError().c_str();
      return false;
    }
  }

  if(neutralFromV4L2Fourcc(m_session.format().fourcc, m_session.driver())
     == VideoPixelFormat::Unknown)
  {
    // S_FMT state outlives the process that set it, so a device left in MJPG
    // stays there and every later open inherits it -- one bad choice would
    // black the camera out permanently, long after the setting that caused it
    // was changed back. Renegotiate to something this path can actually
    // unpack, unless the undecodable format is what was explicitly asked for.
    const bool asked = m_settings.fourcc != 0;
    bool recovered = false;

    if(!asked)
    {
      for(const auto& m : enumerateModes(m_settings.device))
      {
        if(neutralFromV4L2Fourcc(m.fourcc, m_session.driver())
           == VideoPixelFormat::Unknown)
          continue;
        // Keep the geometry that was asked for, if any; only the layout is
        // being corrected.
        const auto w = m_settings.width ? m_settings.width : m.width;
        const auto h = m_settings.height ? m_settings.height : m.height;
        if(m_session.configure(w, h, m.fourcc)
           && neutralFromV4L2Fourcc(m_session.format().fourcc, m_session.driver())
                  != VideoPixelFormat::Unknown)
        {
          qWarning() << "V4L2:" << m_settings.device.c_str()
                     << "was left in a format this path cannot unpack;"
                        " renegotiated to"
                     << QByteArray(reinterpret_cast<const char*>(&m.fourcc), 4);
          recovered = true;
          break;
        }
      }
    }

    if(!recovered)
    {
      // Refusing here is better than opening and rendering garbage: a webcam
      // whose only 4K mode is MJPG has to be reported, not silently accepted.
      qWarning() << "V4L2: pixel format not decodable by the wire unpackers"
                 << m_settings.device.c_str()
                 << "-- this path has no decode stage, so pick an uncompressed"
                    " format (or use the Camera Input device, which decodes)";
      return false;
    }
  }

  const auto& fmt = m_session.format();

  m_width = int(fmt.width);
  m_height = int(fmt.height);
  m_fourcc = fmt.fourcc;
  m_frameByteSize = fmt.sizeImage;

  m_rig = acquireSyncRig(m_settings.syncRig, m_settings.syncMembers);
  if(m_rig && m_settings.syncMember >= m_rig->memberCount())
  {
    // A member index the rig cannot address would silently never publish, and
    // the rig would hold forever waiting for a device that is already running.
    qWarning() << "V4L2: sync member" << m_settings.syncMember << "is outside rig"
               << m_settings.syncRig.c_str() << "of" << m_rig->memberCount()
               << "-- capturing unsynchronised";
    m_rig.reset();
  }

  // Seed the live-format channel with the geometry we just negotiated. The
  // renderer baselines the channel at the end of its init(); without this the
  // capture thread's first publish races that baseline and, when it wins, is
  // read as a mid-stream format change -- tearing down and rebuilding the whole
  // renderer once per start.
  m_ring.publishFormat(
      m_width, m_height,
      int(score::gfx::interop::toAVPixelFormat(neutralFromV4L2Fourcc(m_fourcc, m_session.driver()))),
      0.0);
  return true;
}

Video::ImageFormat V4L2InputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format
      = score::gfx::interop::toAVPixelFormat(neutralFromV4L2Fourcc(m_fourcc, m_session.driver()));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
V4L2InputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  const auto neutral = neutralFromV4L2Fourcc(m_fourcc, m_session.driver());

  // pickStrategies() has already run, so by now we know whether the rung took
  // the whole-frame external image. The two must be chosen together: an
  // external sampler is a different GLSL type and puts the sample in .r, so
  // pairing it with the ordinary decoder samples a texture that was never
  // uploaded, and pairing the ordinary rungs with the external decoder binds a
  // target the staged texture does not have.
  if(m_wantExternal)
  {
    using P = score::gfx::BayerDecoder::Phase;
    const auto phase = [neutral]() -> std::optional<P> {
      switch(neutral)
      {
        case score::gfx::interop::VideoPixelFormat::BayerRGGB8:
        case score::gfx::interop::VideoPixelFormat::BayerRG8:
        case score::gfx::interop::VideoPixelFormat::BayerRGGB10:
        case score::gfx::interop::VideoPixelFormat::BayerRGGB16:
        case score::gfx::interop::VideoPixelFormat::BayerRG12:
          return P::RGGB;
        case score::gfx::interop::VideoPixelFormat::BayerBGGR8:
        case score::gfx::interop::VideoPixelFormat::BayerBGGR10:
        case score::gfx::interop::VideoPixelFormat::BayerBGGR16:
          return P::BGGR;
        case score::gfx::interop::VideoPixelFormat::BayerGRBG8:
        case score::gfx::interop::VideoPixelFormat::BayerGRBG10:
          return P::GRBG;
        case score::gfx::interop::VideoPixelFormat::BayerGBRG8:
        case score::gfx::interop::VideoPixelFormat::BayerGBRG10:
          return P::GBRG;
        default:
          return std::nullopt;
      }
    }();

    if(phase)
    {
      // The ten- and twelve-bit orders ride right-aligned in a 16-bit lane and
      // carry the same rescale the 2D decoder applies.
      const double scale
          = (neutral == score::gfx::interop::VideoPixelFormat::BayerRGGB10
             || neutral == score::gfx::interop::VideoPixelFormat::BayerBGGR10
             || neutral == score::gfx::interop::VideoPixelFormat::BayerGRBG10
             || neutral == score::gfx::interop::VideoPixelFormat::BayerGBRG10)
                ? 64.0625
                : (neutral == score::gfx::interop::VideoPixelFormat::BayerRG12
                       ? 16.0039
                       : 1.0);
      qDebug() << "V4L2: external-image Bayer demosaic engaged";
      return std::make_unique<score::gfx::BayerExternalOESDecoder>(
          meta, *phase, scale);
    }
  }

  return score::gfx::makeWireDecoder(neutral, meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
V4L2InputBackend::pickStrategy(
    QRhi::Implementation impl, const score::gfx::interop::GpuCapabilities&)
{
  m_gpu = nullptr;
  m_gpuActive = false;

  m_borrowed = nullptr;

  // Vulkan and EGL-backed GL are the only backends with a DMA-BUF import path.
  if(impl != QRhi::Vulkan && impl != QRhi::OpenGLES2)
    return {};
  if(!m_session.isOpen() || m_session.isStreaming())
    return {};

  const auto caps = m_session.bufferCaps();
  if(!caps.probed || !caps.mmap)
    return {};

  // The strategy exposes one sampled texture, so a multi-plane wire layout
  // cannot be imported through it.
  const auto& fmt = m_session.format();
  if(fmt.planeCount != 1)
    return {};

  // Deep enough for the driver's own queue plus the frames the renderer holds
  // while the GPU is still reading them.
  const std::size_t slots = std::max<std::size_t>(m_settings.slotCount, 8u);
  if(!m_session.start(BufferMode::MmapExport, slots))
  {
    qDebug() << "V4L2: no dma-buf rung, VIDIOC_EXPBUF path failed:"
             << m_session.lastError().c_str();
    // No EXPBUF is the common case (ProCapture, many UVC drivers); the node
    // moves on to the host-import candidate.
    return {};
  }

  std::vector<score::gfx::interop::DmaBufSlotDesc> descs;
  descs.reserve(m_session.slotCount());
  for(std::size_t i = 0; i < m_session.slotCount(); ++i)
  {
    const auto& s = m_session.slot(i);
    descs.push_back(
        {s.dmabufFd[0], s.modifier, 0u, m_session.format().bytesPerLine});
  }

  auto strat = std::make_unique<score::gfx::interop::DmaBufImportCapture>(
      "V4L2", std::move(descs),
      score::gfx::interop::DmaBufOrigin::ForeignDevice);

  // Ask for the whole frame as one external image. Measured on the Orin NX:
  // eglCreateImage accepts R16/R8/ABGR8888 from a V4L2 export, but binding any
  // of them to GL_TEXTURE_2D fails and only GL_TEXTURE_EXTERNAL_OES succeeds --
  // which is exactly what the per-plane 2D branch reports as "driver cannot
  // sample fourcc ... as a 2D texture" before declining the whole rung. Without
  // this the zero-copy path is unreachable on Tegra and capture falls back to
  // staging 25 MB per frame out of uncached pages.
  //
  // Single-plane only: the external form is one image for the frame, and a
  // planar layout has no single fourcc to name it by.
  if(m_wantExternal)
  {
    const auto neutral = neutralFromV4L2Fourcc(fmt.fourcc, m_session.driver());
    if(const auto drm = score::gfx::interop::toDrmFourcc(neutral); drm != 0)
      strat->requestExternalImage(drm, int(fmt.height));
  }

  m_gpu = strat.get();
  return strat;
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
V4L2InputBackend::pickBorrowedHostImport(QRhi::Implementation impl)
{
  // Zero-copy without VIDIOC_EXPBUF: V4L2_MEMORY_USERPTR lets the card DMA into
  // pages we allocate ourselves. Unlike an MMAP mapping -- which is the
  // device's own DMA memory and which vkGetMemoryHostPointerPropertiesEXT
  // refuses on both NVIDIA and RADV (measured on vivid and ProCapture) --
  // those are ordinary anonymous pages, so the same buffer serves the card and
  // VK_EXT_external_memory_host.
  if(impl != QRhi::Vulkan)
    return {};
  if(!m_session.isOpen())
    return {};
  // The dma-buf candidate may have left the queue streaming in MmapExport;
  // the modes are mutually exclusive.
  if(m_session.isStreaming())
    m_session.stop();
  if(m_session.format().planeCount != 1)
    return {};

  const std::size_t slots = std::max<std::size_t>(m_settings.slotCount, 8u);
  if(!m_session.start(BufferMode::UserPtr, slots))
  {
    qDebug() << "V4L2: no USERPTR rung:" << m_session.lastError().c_str();
    return {};
  }

  std::vector<score::gfx::interop::BorrowedHostBuffer> bufs;
  bufs.reserve(m_session.slotCount());
  for(std::size_t i = 0; i < m_session.slotCount(); ++i)
    bufs.push_back({m_session.userBuffer(i), m_session.userBufferSize()});

  auto strat = std::make_unique<score::gfx::interop::BorrowedHostImportCapture>(
      "V4L2", std::move(bufs));
  m_borrowed = strat.get();
  return strat;
}

std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>()>>
V4L2InputBackend::pickStrategies(
    QRhi::Implementation impl, const score::gfx::interop::GpuCapabilities& caps)
{
  // Best first. The dma-buf rung is preferred where the driver exports (it is
  // the only one that also works on GL), but NVIDIA accepts the fd and then
  // refuses the import, so the host-import rung has to be reachable after it.
  // The external-image intent has to be settled HERE, not inside the thunk.
  // DMACaptureInputNode::init calls this to build the ladder, then makeDecoder,
  // and only then runs the thunks -- so anything pickStrategy() decides is
  // still unknown when the decoder is chosen, and the decoder has to be the
  // one that matches. Deciding it eagerly is what the Argus backend does for
  // the same reason.
  //
  // GL only: the external image is an EGL/GLES construct. A planar layout has
  // no single fourcc to name the whole frame by.
  //
  // It must also be a decision the dma-buf rung can honour, because the
  // external decoder cannot consume a staged upload: if the ladder falls back
  // to CPU with this set, that rung renders black. So the same preconditions
  // pickStrategy() checks before it will build the rung at all are checked
  // here, and anything less certain leaves the ordinary decoder in place --
  // an unnecessary copy is recoverable, a silently black fallback is not.
  m_wantExternal = false;
  if(impl == QRhi::OpenGLES2 && m_session.format().planeCount == 1)
  {
    const auto caps = m_session.bufferCaps();
    const auto neutral = neutralFromV4L2Fourcc(m_fourcc, m_session.driver());
    m_wantExternal = caps.probed && caps.mmap
                     && score::gfx::interop::toDrmFourcc(neutral) != 0
                     && score::gfx::nv12ExternalOesUsable(impl);
  }
  if(!m_wantExternal)
    qDebug() << "V4L2: no whole-frame external image; the ordinary decoder and "
                "the staged rungs stay in play";

  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>()>>
      v;
  v.emplace_back([this, impl, &caps] { return pickStrategy(impl, caps); });
  v.emplace_back([this, impl] { return pickBorrowedHostImport(impl); });
  return v;
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
V4L2InputBackend::makeCpuStrategy()
{
  return std::make_unique<V4L2CpuCapture>();
}

void V4L2InputBackend::setStrategy(
    score::gfx::interop::VideoCaptureStrategy* s) noexcept
{
  m_strategy = s;
  m_gpuActive
      = s != nullptr
        && s == static_cast<score::gfx::interop::VideoCaptureStrategy*>(m_gpu);
  // Settling on anything else means the renderer has already destroyed the
  // GPU strategy (it owns it), so the typed pointer must not outlive it.
  if(!m_gpuActive)
    m_gpu = nullptr;
  // Same for the borrowed-buffer rung: if it is not the one the renderer kept,
  // the capture loop must go back to copy-and-requeue rather than hold buffers
  // for a strategy that no longer exists.
  if(s == nullptr
     || s != static_cast<score::gfx::interop::VideoCaptureStrategy*>(m_borrowed))
    m_borrowed = nullptr;
}

void V4L2InputBackend::start()
{
  if(m_started || !m_session.isOpen() || !m_strategy)
    return;

  if(m_gpuActive || m_borrowed)
  {
    // pickStrategy already brought the queue up in the mode the strategy
    // imported from; restarting it would invalidate those imports. For the
    // USERPTR rung that is not merely wasteful: REQBUFS(0) frees the very pages
    // the strategy handed to VK_EXT_external_memory_host, so the renderer would
    // sample freed memory (observed as black frames, PSNR 4.61).
    if(!m_session.isStreaming())
      return;
  }
  else
  {
    // The GPU rung was refused, rejected by the pin, or never tried: the CPU
    // strategy reads host-visible memory, so the queue has to be reallocated
    // as MmapRead.
    if(m_session.isStreaming())
      m_session.stop();
    if(!m_session.start(BufferMode::MmapRead, m_settings.slotCount))
    {
      qWarning() << "V4L2: STREAMON failed:" << m_session.lastError().c_str();
      return;
    }
  }

  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] {
    if(m_gpuActive)
      runLoopDmaBuf();
    else
      runLoopStaged();
  }};
  m_started = true;
}

void V4L2InputBackend::stop()
{
  m_running.store(false, std::memory_order_release);
  if(m_thread.joinable())
    m_thread.join();

  // The capture thread is gone, so whatever this device left pending in the
  // rig can never complete a row. Give it up before stopping the queue, or the
  // partners wait on a member that no longer exists.
  if(m_rig)
  {
    int held[score::gfx::interop::CaptureFrameSet::kMaxMembers];
    m_rig->drain(held);
    const auto mine = held[m_settings.syncMember];
    if(mine >= 0 && m_gpuActive)
      m_session.requeue(std::size_t(mine));
  }

  m_session.stop();
  m_started = false;
}

score::gfx::DMACaptureBackend::SyncMembership
V4L2InputBackend::syncGroup() noexcept
{
  if(!m_rig)
    return {};
  return {&m_rig->group(), m_settings.syncMember};
}

void V4L2InputBackend::offerToRig(int slot, std::uint64_t stampNs)
{
  if(!m_rig)
    return;

  const int displaced = m_rig->offer(m_settings.syncMember, slot, stampNs);
  if(displaced < 0)
    return;

  // Only the zero-copy rungs hand out the driver's own buffers; the staged rung
  // offers an index into the strategy's ring, which recycles on its own and
  // must never be handed to VIDIOC_QBUF.
  if(m_gpuActive || m_borrowed)
    m_session.requeue(std::size_t(displaced));
}

void V4L2InputBackend::requeueReleasedSlots()
{
  if(!m_gpu)
    return;
  std::uint32_t mask = m_gpu->takeReturnedSlots();
  for(std::size_t i = 0; mask != 0u; ++i, mask >>= 1)
    if(mask & 1u)
      m_session.requeue(i);
}

void V4L2InputBackend::runLoopDmaBuf()
{
  while(m_running.load(std::memory_order_acquire))
  {
    // Before asking for another buffer: the driver only owns the slots we have
    // given back, and the renderer's releases arrive asynchronously.
    requeueReleasedSlots();

    const int idx = m_session.dequeue(200);
    if(idx == -1)
      continue;
    if(idx < 0)
      break;

    const auto& fmt = m_session.format();
    m_ring.publishFormat(
        int(fmt.width), int(fmt.height),
        int(score::gfx::interop::toAVPixelFormat(
            neutralFromV4L2Fourcc(fmt.fourcc, m_session.driver()))),
        0.0);

    // From here the slot belongs to the renderer: it samples the buffer in
    // place, so requeueing it now would let the driver overwrite the frame
    // being drawn. It comes back through requeueReleasedSlots().
    if(!m_gpu->ingestFrame(std::size_t(idx)))
    {
      m_session.requeue(std::size_t(idx));
      continue;
    }
    offerToRig(idx, m_session.slot(std::size_t(idx)).timestampNs);
    m_ring.latestSlot.store(std::size_t(idx), std::memory_order_release);
    m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
  }
  requeueReleasedSlots();
}

void V4L2InputBackend::runLoopStaged()
{
  auto* strat = m_strategy;
  if(!strat)
    return;
  const std::size_t slots = strat->slotCount();
  if(slots == 0)
    return;

  std::size_t writeIdx = 0;
  while(m_running.load(std::memory_order_acquire))
  {
    const int idx = m_session.dequeue(200);
    if(idx == -1)
      continue; // timeout: no frame yet, keep polling so stop() is responsive
    if(idx < 0)
      break;    // fatal stream error

    const auto& slot = m_session.slot(std::size_t(idx));
    const auto& fmt = m_session.format();

    // A source that renegotiates mid-stream (webcam, HDMI bridge relocking)
    // is published the same way the SDI cards publish a detected wire format;
    // the render thread reallocates its size-dependent resources.
    m_ring.publishFormat(
        int(fmt.width), int(fmt.height),
        int(score::gfx::interop::toAVPixelFormat(
            neutralFromV4L2Fourcc(fmt.fourcc, m_session.driver()))),
        0.0);

    if(m_borrowed)
    {
      // The renderer samples this very mapping, so the buffer must NOT go back
      // to the driver yet; it is requeued only once takeReturnedSlots() says
      // the GPU is done with it. The slot index is the driver's own, not a
      // rolling write index.
      if(!m_borrowed->ingestFrame(std::size_t(idx)))
      {
        m_session.requeue(std::size_t(idx));
        continue;
      }
      offerToRig(idx, slot.timestampNs);
      m_ring.latestSlot.store(std::size_t(idx), std::memory_order_release);
      m_ring.latestFrameId.fetch_add(1, std::memory_order_release);

      for(std::uint32_t freed = m_borrowed->takeReturnedSlots(); freed;)
      {
        const unsigned bit = static_cast<unsigned>(__builtin_ctz(freed));
        freed &= freed - 1u;
        m_session.requeue(std::size_t(bit));
      }
      continue;
    }

    if(void* dst = strat->slotBuffer(writeIdx))
    {
      const std::size_t n
          = std::min<std::size_t>(slot.bytesUsed ? slot.bytesUsed : fmt.sizeImage,
                                  m_frameByteSize);
      if(slot.mapped[0])
        std::memcpy(dst, slot.mapped[0], n);
    }

    // Give the buffer back before publishing: the driver only has
    // slotCount buffers, and holding one across the renderer's consumption
    // would starve the queue.
    m_session.requeue(std::size_t(idx));

    if(!strat->ingestFrame(writeIdx))
      continue;
    offerToRig(int(writeIdx), slot.timestampNs);
    m_ring.latestSlot.store(writeIdx, std::memory_order_release);
    m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
    writeIdx = (writeIdx + 1) % slots;
  }
}

} // namespace Gfx::V4L2
