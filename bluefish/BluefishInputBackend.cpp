#include <bluefish/BluefishInputBackend.hpp>

#include <bluefish/BluefishCpuCapture.hpp>
#include <bluefish/BluefishFormats.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <Video/VideoInterface.hpp>

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace Gfx::Bluefish
{

BluefishInputBackend::BluefishInputBackend(
    BluefishInputSettings settings,
    score::gfx::interop::VideoCaptureSlotRing& ring)
    : m_settings{settings}, m_ring{ring}
{
}

BluefishInputBackend::~BluefishInputBackend()
{
  stop();
}

bool BluefishInputBackend::open()
{
  m_bvc = bfcFactory();
  if(!m_bvc)
    return false;

  const BLUE_S32 deviceId = std::max(1, m_settings.deviceIndex);
  if(BLUE_FAIL(bfcAttach(m_bvc, deviceId)))
  {
    qWarning() << "Bluefish input: bfcAttach failed for device" << deviceId;
    stop();
    return false;
  }

  // Recommended input setup auto-detects the incoming standard on channel 1.
  blue_setup_info setup = bfcUtilsGetDefaultSetupInfoInput(BLUE_VIDEO_INPUT_CHANNEL_1);
  setup.DeviceId = deviceId;
  if(BLUE_FAIL(bfcUtilsGetRecommendedSetupInfoInput(
         m_bvc, &setup, UHD_PREFERENCE_DEFAULT)))
  {
    qWarning() << "Bluefish input: no incoming signal detected";
    stop();
    return false;
  }

  setup.MemoryFormat = static_cast<EMemoryFormat>(m_settings.memoryFormat);
  setup.UpdateMethod = UPD_FMT_FRAME;
  setup.VideoEngine
      = (setup.VideoModeExt > VID_FMT_EXT_2K_1556I_1500)
            ? VIDEO_ENGINE_AUTO_CAPTURE_UHD
            : VIDEO_ENGINE_AUTO_CAPTURE;

  if(BLUE_FAIL(bfcUtilsValidateSetupInfo(&setup)))
  {
    qWarning() << "Bluefish input: bfcUtilsValidateSetupInfo (RX) failed";
    stop();
    return false;
  }
  if(BLUE_FAIL(bfcUtilsSetupInput(m_bvc, &setup)))
  {
    qWarning() << "Bluefish input: bfcUtilsSetupInput failed";
    stop();
    return false;
  }
  m_videoModeExt = setup.VideoModeExt;

  BLUE_U32 w = 0, h = 0, bpl = 0, bpf = 0, golden = 0;
  bfcGetVideoInfo(setup.VideoModeExt, setup.UpdateMethod, setup.MemoryFormat,
                  &w, &h, &bpl, &bpf, &golden);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  m_frameByteSize = bpf;

  // SDK-owned pinned buffers: bfcAutoCaptureGetFilledBuffer hands us a filled one.
  if(BLUE_FAIL(bfcAutoCaptureCreateInternalBuffers(m_bvc, BUFFER_COMPONENT_VIDEO)))
  {
    qWarning() << "Bluefish input: bfcAutoCaptureCreateInternalBuffers failed";
    stop();
    return false;
  }
  m_buffersCreated = true;
  return true;
}

Video::ImageFormat BluefishInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format = score::gfx::interop::toAVPixelFormat(
      neutralFromMemFmt(m_settings.memoryFormat));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
BluefishInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(
      neutralFromMemFmt(m_settings.memoryFormat), meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
BluefishInputBackend::pickStrategy(QRhi::Implementation)
{
  // BlueGpuDirect (GPU-direct capture) is out of scope; always host-staged.
  return {};
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
BluefishInputBackend::makeCpuStrategy()
{
  return std::make_unique<BluefishCpuCapture>();
}

void BluefishInputBackend::setStrategy(
    score::gfx::interop::VideoCaptureStrategy* s) noexcept
{
  m_strategy = s;
}

void BluefishInputBackend::start()
{
  if(m_started || !m_bvc)
    return;
  if(BLUE_FAIL(bfcVideoCaptureStart(m_bvc)))
  {
    qWarning() << "Bluefish input: bfcVideoCaptureStart failed";
    return;
  }
  m_captureStarted = true;
  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] { runLoop(); }};
  m_started = true;
}

void BluefishInputBackend::stop()
{
  m_running.store(false, std::memory_order_release);
  // Stop the AutoCapture FIFO BEFORE joining: the reception thread may be
  // parked inside bfcAutoCaptureGetFilledBuffer(RETURN_MODE_BLOCKING) with no
  // signal on the wire, and only the capture stop unblocks it (the old
  // stop-after-join order could hang the render thread forever).
  if(m_bvc && m_captureStarted)
    bfcVideoCaptureStop(m_bvc);
  if(m_thread.joinable())
    m_thread.join();

  if(m_bvc)
  {
    if(m_buffersCreated)
      bfcAutoCaptureDestroyInternalBuffers(m_bvc);
    bfcDetach(m_bvc);
    bfcDestroy(m_bvc);
    m_bvc = nullptr;
  }
  if(m_conv)
  {
    bfcConversionDestroy(m_conv);
    m_conv = nullptr;
  }
  m_captureStarted = false;
  m_buffersCreated = false;
  m_started = false;
}

void BluefishInputBackend::runLoop()
{
  std::size_t writeIdx = 0;
  while(m_running.load(std::memory_order_acquire))
  {
    auto* strat = m_strategy;
    const std::size_t slots = strat ? strat->slotCount() : 0;
    if(slots == 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    blue_auto_buffer_info info{};
    info.CardBufferId = -1;
    const BErr r = bfcAutoCaptureGetFilledBuffer(m_bvc, &info, RETURN_MODE_BLOCKING);
    if(BLUE_FAIL(r) || info.CardBufferId < 0)
    {
      // Per the SDK Capture sample: the blocking call CAN return without a
      // buffer, and re-calling in a tight loop pins a core. FIFO stopped =>
      // shutdown (or signal teardown); no-buffer => wait for the next input
      // interrupt; underrun / invalid mode => brief sleep.
      if(r == BERR_FIFO_NOT_RUNNING)
        break;
      if(r == BERR_NO_ERROR)
      {
        unsigned long fieldCount = 0;
        bfcWaitVideoInputSync(m_bvc, UPD_FMT_FRAME, &fieldCount);
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      continue;
    }

    // Only publish complete frames whose mode matches the negotiated one; the
    // SDK owns info.pBufferVideo, so copy it into the strategy slot then return.
    if(!info.BufferIncomplete && info.pBufferVideo
       && info.VideoModeExt == m_videoModeExt)
    {
      if(void* dst = strat->slotBuffer(writeIdx))
      {
        if(!ingestBuffer(info, dst))
        {
          bfcAutoCaptureReturnBuffer(m_bvc, &info);
          continue;
        }
        strat->ingestFrame(writeIdx);
        m_ring.latestSlot.store(writeIdx, std::memory_order_release);
        m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
        writeIdx = (writeIdx + 1) % slots;
      }
    }
    else if(info.pBufferVideo && info.VideoModeExt != m_videoModeExt)
    {
      // Live input mode change: publish the new geometry so the render thread
      // rebuilds; open() re-derives the raster from the wire on re-open.
      BLUE_U32 nw = 0, nh = 0, nbpl = 0, nbpf = 0, ngold = 0;
      bfcGetVideoInfo(
          info.VideoModeExt, UPD_FMT_FRAME,
          static_cast<EMemoryFormat>(m_settings.memoryFormat), &nw, &nh, &nbpl,
          &nbpf, &ngold);
      if(nw > 0 && nh > 0)
        m_ring.publishFormat(int(nw), int(nh), -1, 0.0);
    }

    bfcAutoCaptureReturnBuffer(m_bvc, &info);
  }
}

bool BluefishInputBackend::ingestBuffer(blue_auto_buffer_info& info, void* dst)
{
  // UHD 2SI inputs deliver a sample-interleaved buffer; the SDK flags it via
  // RequiredConversionType and the CaptureAndReplay sample runs the matching
  // bfcConvert_TsiToSquareDivision_* to rebuild the plain raster. Publishing
  // the buffer verbatim would show interleave-scrambled quadrants.
  if(info.RequiredConversionType == BLUE_CONVERSION_TYPE_2SI_TO_SQD)
  {
    if(!m_conv)
      m_conv = bfcConversionFactory();
    if(!m_conv)
      return false;
    const auto w = static_cast<BLUE_U32>(m_width);
    const auto h = static_cast<BLUE_U32>(m_height);
    switch(m_settings.memoryFormat)
    {
      case MEM_FMT_YUVS: // == MEM_FMT_BV8
      case MEM_FMT_2VUY:
        return !BLUE_FAIL(bfcConvert_TsiToSquareDivision_2VUY(
            m_conv, w, h, info.pBufferVideo, dst));
      case MEM_FMT_V210:
        return !BLUE_FAIL(bfcConvert_TsiToSquareDivision_V210(
            m_conv, w, h, info.pBufferVideo, dst));
      case MEM_FMT_RGBA:
      case MEM_FMT_ARGB_PC: // == MEM_FMT_BGRA
        return !BLUE_FAIL(bfcConvert_TsiToSquareDivision_ARGB32(
            m_conv, w, h, info.pBufferVideo, dst));
      default:
        qWarning() << "Bluefish input: no 2SI->SQD conversion for memory format"
                   << m_settings.memoryFormat << "- dropping frame";
        return false;
    }
  }

  std::memcpy(
      dst, info.pBufferVideo, std::min<BLUE_U32>(info.SizeVideo, m_frameByteSize));
  return true;
}

} // namespace Gfx::Bluefish
