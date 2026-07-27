#include "V4L2InputBackend.hpp"

#include <v4l2/V4L2CpuCapture.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <linux/videodev2.h>

#include <QDebug>

#include <cstring>

namespace Gfx::V4L2
{
using score::gfx::interop::VideoPixelFormat;

VideoPixelFormat neutralFromV4L2Fourcc(std::uint32_t f) noexcept
{
  switch(f)
  {
    case V4L2_PIX_FMT_UYVY:
      return VideoPixelFormat::UYVY422;
    case V4L2_PIX_FMT_YUYV:
      return VideoPixelFormat::YUYV422;
    case V4L2_PIX_FMT_YVYU:
      return VideoPixelFormat::YVYU422;
    case V4L2_PIX_FMT_VYUY:
      return VideoPixelFormat::VYUY422;
    case V4L2_PIX_FMT_NV12:
      return VideoPixelFormat::NV12;
    case V4L2_PIX_FMT_ABGR32: // V4L2 "AR24": B,G,R,A in memory
      return VideoPixelFormat::BGRA8;
    case V4L2_PIX_FMT_RGBA32: // V4L2 "AB24": R,G,B,A in memory
      return VideoPixelFormat::RGBA8;
    case V4L2_PIX_FMT_BGR24:
      return VideoPixelFormat::BGR24;
    case V4L2_PIX_FMT_RGB24:
      return VideoPixelFormat::RGB24;
    default:
      // Compressed sources (MJPG/H264) land here on purpose: they need a
      // decode stage this path does not have, and silently treating them as
      // raw would render noise.
      return VideoPixelFormat::Unknown;
  }
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

  const auto& fmt = m_session.format();
  if(neutralFromV4L2Fourcc(fmt.fourcc) == VideoPixelFormat::Unknown)
  {
    // Refusing here is better than opening and rendering garbage: a webcam
    // whose only 4K mode is MJPG has to be reported, not silently accepted.
    qWarning() << "V4L2: pixel format not decodable by the wire unpackers"
               << m_settings.device.c_str();
    return false;
  }

  m_width = int(fmt.width);
  m_height = int(fmt.height);
  m_fourcc = fmt.fourcc;
  m_frameByteSize = fmt.sizeImage;
  return true;
}

Video::ImageFormat V4L2InputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format
      = score::gfx::interop::toAVPixelFormat(neutralFromV4L2Fourcc(m_fourcc));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
V4L2InputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(neutralFromV4L2Fourcc(m_fourcc), meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
V4L2InputBackend::pickStrategy(QRhi::Implementation)
{
  // No GPU-direct strategy yet: importing the V4L2 DMA-BUF into the
  // renderer's texture needs its own VideoCaptureStrategy. The session
  // supports both zero-copy ingress modes already, so that is additive.
  return {};
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
}

void V4L2InputBackend::start()
{
  if(m_started || !m_session.isOpen() || !m_strategy)
    return;

  // MmapRead: the loop copies into the strategy's slots, which is the
  // host-staged contract the renderer expects from a CPU strategy.
  if(!m_session.start(BufferMode::MmapRead, m_settings.slotCount))
  {
    qWarning() << "V4L2: STREAMON failed:" << m_session.lastError().c_str();
    return;
  }

  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] { runLoop(); }};
  m_started = true;
}

void V4L2InputBackend::stop()
{
  if(!m_started)
    return;
  m_running.store(false, std::memory_order_release);
  if(m_thread.joinable())
    m_thread.join();
  m_session.stop();
  m_started = false;
}

void V4L2InputBackend::runLoop()
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
            neutralFromV4L2Fourcc(fmt.fourcc))),
        0.0);

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
    m_ring.latestSlot.store(writeIdx, std::memory_order_release);
    m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
    writeIdx = (writeIdx + 1) % slots;
  }
}

} // namespace Gfx::V4L2
