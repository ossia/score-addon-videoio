#include <magewell/MagewellInputBackend.hpp>

#include <magewell/MagewellCpuCapture.hpp>
#include <magewell/MagewellDevices.hpp>
#include <magewell/MagewellFormats.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <Video/VideoInterface.hpp>

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Magewell
{

MagewellInputBackend::MagewellInputBackend(
    MagewellInputSettings settings,
    score::gfx::interop::GpuDirectCaptureSlotRing& ring)
    : m_settings{settings}, m_ring{ring}
{
}

MagewellInputBackend::~MagewellInputBackend()
{
  stop();
}

bool MagewellInputBackend::open()
{
  if(!ensureMwInit())
  {
    qWarning() << "Magewell input: MWCaptureInitInstance failed";
    return false;
  }

  MWRefreshDevice();

  // Resolve the channel index to a device path, then open the channel.
  // (UTF-16 on Windows, plain char on Linux — the SDK signature differs.)
#if defined(_WIN32)
  WCHAR path[256] = {0};
#else
  char path[256] = {0};
#endif
  if(MWGetDevicePath(m_settings.deviceIndex, path) != MW_SUCCEEDED)
  {
    qWarning() << "Magewell input: MWGetDevicePath failed for channel"
               << m_settings.deviceIndex;
    return false;
  }
  m_channel = MWOpenChannelByPath(path);
  if(!m_channel)
  {
    qWarning() << "Magewell input: MWOpenChannelByPath failed for channel"
               << m_settings.deviceIndex;
    return false;
  }

  // Magewell auto-detects the incoming signal: geometry comes from the card.
  MWCAP_VIDEO_SIGNAL_STATUS status{};
  if(MWGetVideoSignalStatus(m_channel, &status) != MW_SUCCEEDED
     || status.state != MWCAP_VIDEO_SIGNAL_LOCKED)
  {
    qWarning() << "Magewell input: no locked video signal detected";
    MWCloseChannel(m_channel);
    m_channel = nullptr;
    return false;
  }

  m_width = status.cx;
  m_height = status.cy;
  const double fps = status.dwFrameDuration != 0
                         ? 10000000.0 / status.dwFrameDuration
                         : 0.0;
  m_stride = strideFromFourcc(m_settings.fourcc, m_width);
  m_frameByteSize
      = imageSizeFromFourcc(m_settings.fourcc, m_width, m_height, m_stride);

  qDebug() << "Magewell input: locked" << m_width << "x" << m_height << "@"
           << fps << "fps, frame bytes" << m_frameByteSize;
  return true;
}

Video::ImageFormat MagewellInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format
      = score::gfx::interop::toAVPixelFormat(neutralFromFourcc(m_settings.fourcc));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
MagewellInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(neutralFromFourcc(m_settings.fourcc), meta);
}

std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
MagewellInputBackend::pickStrategy(QRhi::Implementation)
{
  // Magewell has no GPU-direct capture path; always host-staged.
  return {};
}

std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
MagewellInputBackend::makeCpuStrategy()
{
  return std::make_unique<MagewellCpuCapture>();
}

void MagewellInputBackend::setStrategy(
    score::gfx::interop::GpuDirectCaptureStrategy* s) noexcept
{
  m_strategy = s;
}

void MagewellInputBackend::start()
{
  if(m_started || !m_channel)
    return;

  auto* strat = m_strategy;
  if(!strat)
    return;

  // Undo the partial bring-up on any failure so a later start() retry does
  // not overwrite live event handles or double-pin the slot buffers.
  auto failStart = [this] {
    if(m_captureStarted)
    {
      MWStopVideoCapture(m_channel);
      m_captureStarted = false;
    }
    for(auto& buf : m_pinnedBuffers)
    {
      if(buf)
      {
        MWUnpinVideoBuffer(m_channel, buf);
        buf = nullptr;
      }
    }
    if(m_notifyEvent != mwNoEvent)
    {
      mwEventClose(m_notifyEvent);
      m_notifyEvent = mwNoEvent;
    }
    if(m_captureEvent != mwNoEvent)
    {
      mwEventClose(m_captureEvent);
      m_captureEvent = mwNoEvent;
    }
  };

  // Two auto-reset events: one signalled per buffered frame (notify), one
  // signalled when a MWCaptureVideoFrameToVirtualAddress transfer completes.
  m_notifyEvent = mwEventCreate();
  m_captureEvent = mwEventCreate();
  if(m_notifyEvent == mwNoEvent || m_captureEvent == mwNoEvent)
  {
    qWarning() << "Magewell input: event creation failed";
    failStart();
    return;
  }

  // Pin each host-staged slot's virtual memory to reduce CPU load on transfer.
  const std::size_t slots = std::min<std::size_t>(strat->slotCount(), kMaxSlots);
  for(std::size_t i = 0; i < slots; ++i)
  {
    auto* buf = static_cast<LPBYTE>(strat->slotBuffer(i));
    if(buf && MWPinVideoBuffer(m_channel, buf, m_frameByteSize) == MW_SUCCEEDED)
      m_pinnedBuffers[i] = buf;
  }

  if(MWStartVideoCapture(m_channel, m_captureEvent) != MW_SUCCEEDED)
  {
    qWarning() << "Magewell input: MWStartVideoCapture failed";
    failStart();
    return;
  }
  m_captureStarted = true;

  m_notify = MWRegisterNotify(
      m_channel, m_notifyEvent, MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED);
  if(m_notify == 0)
  {
    qWarning() << "Magewell input: MWRegisterNotify failed";
    failStart();
    return;
  }

  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] { runLoop(); }};
  m_started = true;
}

void MagewellInputBackend::stop()
{
  m_running.store(false, std::memory_order_release);
  if(m_thread.joinable())
    m_thread.join();

  if(m_channel)
  {
    if(m_notify != 0)
    {
      MWUnregisterNotify(m_channel, m_notify);
      m_notify = 0;
    }
    if(m_captureStarted)
    {
      MWStopVideoCapture(m_channel);
      m_captureStarted = false;
    }
    for(auto& buf : m_pinnedBuffers)
    {
      if(buf)
      {
        MWUnpinVideoBuffer(m_channel, buf);
        buf = nullptr;
      }
    }
    MWCloseChannel(m_channel);
    m_channel = nullptr;
  }

  if(m_notifyEvent != mwNoEvent)
  {
    mwEventClose(m_notifyEvent);
    m_notifyEvent = mwNoEvent;
  }
  if(m_captureEvent != mwNoEvent)
  {
    mwEventClose(m_captureEvent);
    m_captureEvent = mwNoEvent;
  }
  m_started = false;
}

void MagewellInputBackend::runLoop()
{
  constexpr DWORD kTimeoutMs = 1000;
  std::size_t writeIdx = 0;
  while(m_running.load(std::memory_order_acquire))
  {
    auto* strat = m_strategy;
    if(!strat)
      continue;
    const std::size_t slots = std::min<std::size_t>(strat->slotCount(), kMaxSlots);
    if(slots == 0)
      continue;

    // Wait for the next buffered frame (auto-reset notify event).
    if(!mwEventWait(m_notifyEvent, kTimeoutMs))
      continue; // no signal yet; keep polling

    ULONGLONG ullStatusBits = 0;
    if(MWGetNotifyStatus(m_channel, m_notify, &ullStatusBits) != MW_SUCCEEDED)
      continue;
    if(!(ullStatusBits & MWCAP_NOTIFY_VIDEO_FRAME_BUFFERED))
      continue;

    MWCAP_VIDEO_BUFFER_INFO binfo{};
    if(MWGetVideoBufferInfo(m_channel, &binfo) != MW_SUCCEEDED)
      continue;

    auto* dst = static_cast<LPBYTE>(strat->slotBuffer(writeIdx));
    if(!dst)
      continue;

    // Capture the newest fully-buffered frame straight into the slot in the
    // selected FOURCC, driving the card's on-card CSC + deinterlace so no CPU
    // swizzle is needed: RGB targets get RGB output, YUV targets BT.709.
    const MWCAP_VIDEO_COLOR_FORMAT csc
        = (m_settings.fourcc == MWFOURCC_BGRA || m_settings.fourcc == MWFOURCC_RGBA)
              ? MWCAP_VIDEO_COLOR_FORMAT_RGB
              : MWCAP_VIDEO_COLOR_FORMAT_YUV709;
    if(MWCaptureVideoFrameToVirtualAddressEx(
           m_channel, binfo.iNewestBufferedFullFrame, dst, m_frameByteSize,
           m_stride, FALSE, (MWCAP_PTR64)0, m_settings.fourcc, m_width, m_height,
           /*dwProcessSwitchs*/ 0, /*cyParitalNotify*/ 0,
           /*hOSDImage*/ (HOSD)0, /*pOSDRects*/ nullptr, /*cOSDRects*/ 0,
           // Neutral processing amplitudes per the SDK ranges: contrast
           // [50,200] and saturation [0,200] are scaled around 100 = identity
           // (0 would clamp contrast and fully desaturate every frame).
           /*sContrast*/ 100, /*sBrightness*/ 0, /*sSaturation*/ 100, /*sHue*/ 0,
           MWCAP_VIDEO_DEINTERLACE_WEAVE, MWCAP_VIDEO_ASPECT_RATIO_IGNORE,
           /*pRectSrc*/ nullptr, /*pRectDest*/ nullptr,
           /*nAspectX*/ 0, /*nAspectY*/ 0, csc,
           MWCAP_VIDEO_QUANTIZATION_UNKNOWN, MWCAP_VIDEO_SATURATION_UNKNOWN)
       != MW_SUCCEEDED)
      continue;

    // Wait for the transfer to complete (auto-reset capture event).
    if(!mwEventWait(m_captureEvent, kTimeoutMs))
      continue;

    strat->ingestFrame(writeIdx);
    m_ring.latestSlot.store(writeIdx, std::memory_order_release);
    m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
    writeIdx = (writeIdx + 1) % slots;
  }
}

} // namespace Gfx::Magewell
