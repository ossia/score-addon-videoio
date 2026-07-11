#include <decklink/DeckLinkInputBackend.hpp>

#include <decklink/DeckLinkCpuCapture.hpp>
#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkFormats.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

// NVIDIA-DVP GPU-direct capture via the shared shim, gated on the bridge being
// linked (same source-side gate AJA uses). GL needs Qt OpenGL; D3D11 is WIN32.
#if defined(SCORE_HAS_AJA_DVP_BRIDGE)
#include <Gfx/Graph/interop/DmaLockPolicy.hpp>
#if QT_CONFIG(opengl)
#include <Gfx/Graph/interop/DvpCaptureGl.hpp>
#define VIDEOIO_DECKLINK_DVP_GL 1
#endif
#if defined(_WIN32)
#include <Gfx/Graph/interop/DvpCaptureD3D11.hpp>
#define VIDEOIO_DECKLINK_DVP_D3D11 1
#endif
#endif

#include <Video/VideoInterface.hpp>

#include <QDebug>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace Gfx::DeckLink
{
namespace
{

/// Wire row stride (bytes) for a DeckLink packed pixel format at `width`,
/// matching the SDK's RowBytesForPixelFormat rules.
int rowBytesFor(BMDPixelFormat f, int w) noexcept
{
  switch(f)
  {
    case bmdFormat8BitYUV:    return w * 2;
    case bmdFormat10BitYUV:   return ((w + 47) / 48) * 128;
    case bmdFormat8BitBGRA:
    case bmdFormat8BitARGB:    return w * 4;
    case bmdFormat10BitRGB:   return ((w + 63) / 64) * 256;
    case bmdFormat12BitRGB:
    case bmdFormat12BitRGBLE:  return (w * 36) / 8;
    default:                   return w * 4;
  }
}

#if defined(VIDEOIO_DECKLINK_DVP_GL) || defined(VIDEOIO_DECKLINK_DVP_D3D11)
/// DVP texel format of the decode-input texture for a DeckLink wire format.
/// BGRA is the only byte-swapped 4-byte layout; everything else (UYVY/v210/...)
/// decodes through an RGBA8 texture.
NvDvpFormat deckLinkDvpFormat(BMDPixelFormat f) noexcept
{
  return (f == bmdFormat8BitBGRA) ? NV_DVP_FORMAT_BGRA8 : NV_DVP_FORMAT_RGBA8;
}
#endif

/// Copies each arrived frame into the capture strategy's next slot and publishes
/// it in the node's slot ring (the SDK owns this callback thread).
class InputCallback final : public IDeckLinkInputCallback
{
public:
  InputCallback(
      IDeckLinkInput* input, DeckLinkInputSettings settings,
      score::gfx::interop::GpuDirectCaptureStrategy** strategy,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring,
      std::uint32_t frameByteSize, int rowBytes, int height)
      : m_input{input}
      , m_inputSettings{settings}
      , m_strategy{strategy}
      , m_ring{ring}
      , m_frameByteSize{frameByteSize}
      , m_rowBytes{rowBytes}
      , m_height{height}
  {
  }

  /// stop() sets this before StopStreams so a re-arm can't race teardown.
  std::atomic<bool> stopping{false};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
  {
    if(!ppv)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IUnknown)
       || IsEqualIID(iid, IID_IDeckLinkInputCallback))
    {
      *ppv = static_cast<IDeckLinkInputCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG r = --m_ref;
    if(r == 0)
      delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
      BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* newMode,
      BMDDetectedVideoInputFormatFlags detected) override
  {
    // We run a FIXED mode, so this event is only actionable when the detected
    // signal actually differs from what the input is already enabled with.
    // Re-arming unconditionally self-feeds: every re-enable re-triggers
    // detection, and during an upstream colorspace transition (YUV idle
    // raster -> 4:4:4 RGB payload) the storm burns the whole re-arm budget
    // before the wire settles, leaving the capture black for the session.
    const bool wantRGB = m_inputSettings.pixelFormat == bmdFormat8BitBGRA
                         || m_inputSettings.pixelFormat == bmdFormat8BitARGB
                         || m_inputSettings.pixelFormat == bmdFormat10BitRGB
                         || m_inputSettings.pixelFormat == bmdFormat10BitRGBX
                         || m_inputSettings.pixelFormat == bmdFormat10BitRGBXLE
                         || m_inputSettings.pixelFormat == bmdFormat12BitRGB
                         || m_inputSettings.pixelFormat == bmdFormat12BitRGBLE;
    const bool wireRGB = detected & bmdDetectedVideoInputRGB444;
    const bool wireYUV = detected & bmdDetectedVideoInputYCbCr422;
    const bool modeDiffers = (events & bmdVideoInputDisplayModeChanged)
                             && newMode
                             && newMode->GetDisplayMode()
                                    != m_inputSettings.displayMode;
    const bool colorDiffers
        = (wantRGB && wireYUV && !wireRGB) || (!wantRGB && wireRGB && !wireYUV);
    if(modeDiffers || colorDiffers)
      rearm("format-change");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
      IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*) override
  {
    auto* strat = *m_strategy;
    if(!strat || !frame)
      return S_OK;
    // Signal loss produces placeholder frames flagged bmdFrameHasNoInputSource;
    // the samples never forward those (the consumer keeps the last good frame).
    // A capture that starts before the far end locks can stay in this state
    // forever ("black latch"): the placeholder stream never flips back to real
    // frames on its own. Count the streak and periodically re-arm the input so
    // the hardware re-acquires the (by-now valid) signal.
    if(frame->GetFlags() & bmdFrameHasNoInputSource)
    {
      if(++m_noSourceStreak >= kRearmAfterNoSource)
        rearm("no-input-source streak");
      return S_OK;
    }
    m_noSourceStreak = 0;
    // SDK 16.0: frame bytes are accessed via IDeckLinkVideoBuffer, not the
    // frame interface.
    ComPtr<IDeckLinkVideoBuffer> buf;
    if(frame->QueryInterface(IID_IDeckLinkVideoBuffer, buf.putVoid()) != S_OK
       || !buf)
      return S_OK;
    if(buf->StartAccess(bmdBufferAccessRead) != S_OK)
      return S_OK;
    void* src = nullptr;
    const HRESULT hr = buf->GetBytes(&src);

    const std::size_t slot = m_slot;
    if(SUCCEEDED(hr) && src)
    {
      // Use the frame's own geometry, per sample practice: devices may pad
      // row bytes, and a drifted mode must neither overread the SDK buffer
      // nor skew rows in the tightly-packed slot.
      const long srcRowBytes = frame->GetRowBytes();
      const long srcRows = frame->GetHeight();
      void* dst = strat->slotBuffer(slot);
      if(dst && srcRowBytes >= m_rowBytes && srcRows >= m_height)
      {
        if(srcRowBytes == m_rowBytes)
        {
          std::memcpy(dst, src, m_frameByteSize);
        }
        else
        {
          auto* s = static_cast<const std::uint8_t*>(src);
          auto* d = static_cast<std::uint8_t*>(dst);
          for(int y = 0; y < m_height; ++y)
            std::memcpy(
                d + std::size_t(y) * m_rowBytes,
                s + std::size_t(y) * srcRowBytes, m_rowBytes);
        }
        strat->ingestFrame(slot);
        m_ring.latestSlot.store(slot, std::memory_order_release);
        m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
        const std::size_t n = strat->slotCount();
        m_slot = n ? (slot + 1) % n : 0;
      }
    }
    buf->EndAccess(bmdBufferAccessRead);
    return S_OK;
  }

private:
  // ~half a second at 60p; placeholder frames keep arriving at mode rate
  // while unlocked, so the streak advances even with no signal.
  static constexpr int kRearmAfterNoSource = 25;
  static constexpr int kMaxRearms = 32;

  /// Re-acquire the signal: pause -> re-enable (same fixed mode) -> flush ->
  /// start, per the SDK Capture sample. Runs on the SDK callback thread, which
  /// the API allows (the sample re-enables from VideoInputFormatChanged).
  void rearm(const char* why)
  {
    m_noSourceStreak = 0;
    if(stopping.load(std::memory_order_acquire) || !m_input)
      return;
    if(m_rearms >= kMaxRearms)
      return;
    ++m_rearms;
    qDebug() << "DeckLink input: re-arming capture (" << why << "), attempt"
             << m_rearms;
    m_input->PauseStreams();
    m_input->EnableVideoInput(
        m_inputSettings.displayMode, m_inputSettings.pixelFormat,
        m_enabledFlags);
    m_input->FlushStreams();
    m_input->StartStreams();
  }

public:
  /// Flags EnableVideoInput was opened with (so re-arm keeps format detection).
  BMDVideoInputFlags m_enabledFlags{bmdVideoInputFlagDefault};

private:
  IDeckLinkInput* m_input{};
  DeckLinkInputSettings m_inputSettings;
  score::gfx::interop::GpuDirectCaptureStrategy** m_strategy{};
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;
  std::uint32_t m_frameByteSize{};
  int m_rowBytes{};
  int m_height{};
  std::size_t m_slot{0};
  int m_noSourceStreak{0};
  int m_rearms{0};
  std::atomic<ULONG> m_ref{1};
};

} // namespace

DeckLinkInputBackend::DeckLinkInputBackend(
    DeckLinkInputSettings settings,
    score::gfx::interop::GpuDirectCaptureSlotRing& ring)
    : m_settings{settings}, m_ring{ring}
{
}

DeckLinkInputBackend::~DeckLinkInputBackend()
{
  stop();
}

bool DeckLinkInputBackend::open()
{
  ensureComInit();
  m_device = openDevice(m_settings.deviceIndex);
  if(!m_device)
    return false;
  if(m_device->QueryInterface(IID_IDeckLinkInput, m_input.putVoid()) != S_OK
     || !m_input)
    return false;

  // Route the requested physical connector before mode validation — signal
  // detection and DoesSupportVideoMode depend on the active input.
  if(m_settings.connection != BMDVideoConnection(0))
  {
    ComPtr<IDeckLinkConfiguration> cfg;
    if(m_device->QueryInterface(IID_IDeckLinkConfiguration, cfg.putVoid())
           == S_OK
       && cfg)
    {
      if(cfg->SetInt(
             bmdDeckLinkConfigVideoInputConnection,
             int64_t(m_settings.connection))
         != S_OK)
        qWarning() << "DeckLink input: could not select input connection"
                   << int64_t(m_settings.connection);
    }
  }

  ComPtr<IDeckLinkDisplayModeIterator> modeIt;
  ComPtr<IDeckLinkDisplayMode> mode;
  if(m_input->GetDisplayModeIterator(modeIt.put()) == S_OK && modeIt)
  {
    ComPtr<IDeckLinkDisplayMode> m;
    while(modeIt->Next(m.put()) == S_OK)
    {
      if(m->GetDisplayMode() == m_settings.displayMode)
      {
        mode = m;
        break;
      }
      m.reset();
    }
  }
  if(!mode)
  {
    qWarning() << "DeckLink input: display mode not supported";
    return false;
  }
  m_width = static_cast<int>(mode->GetWidth());
  m_height = static_cast<int>(mode->GetHeight());
  m_rowBytes = rowBytesFor(m_settings.pixelFormat, m_width);
  m_frameByteSize = static_cast<std::uint32_t>(m_rowBytes)
                    * static_cast<std::uint32_t>(m_height);

  // Format detection lets the SDK fire VideoInputFormatChanged when a signal
  // (re)appears — the hook the callback's re-arm logic needs to recover from
  // a capture that started before the source locked.
  m_enableFlags = bmdVideoInputFlagDefault;
  {
    ComPtr<IDeckLinkProfileAttributes> attrs;
    bool detect = false;
    if(m_device->QueryInterface(IID_IDeckLinkProfileAttributes, attrs.putVoid())
           == S_OK
       && attrs
       && attrs->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &detect)
              == S_OK
       && detect)
      m_enableFlags |= bmdVideoInputEnableFormatDetection;
  }

  // The driver releases a previous owner's input claim asynchronously: an app
  // (re)opening the device right after another one closed it — including our
  // own previous run — gets a transient failure here, more often on HDMI where
  // the RX front-end also retrains. One failed call must not condemn the whole
  // capture to the silent black-frame path, so retry with a short backoff.
  HRESULT hr = E_FAIL;
  for(int attempt = 0; attempt < 20; ++attempt)
  {
    hr = m_input->EnableVideoInput(
        m_settings.displayMode, m_settings.pixelFormat, m_enableFlags);
    if(hr == S_OK)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  qWarning() << "DeckLink input: EnableVideoInput failed, hr =" << Qt::hex
             << quint32(hr);
  return false;
}

Video::ImageFormat DeckLinkInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format
      = score::gfx::interop::toAVPixelFormat(toNeutralFormat(m_settings.pixelFormat));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
DeckLinkInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(
      toNeutralFormat(m_settings.pixelFormat), meta);
}

std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
DeckLinkInputBackend::makeCpuStrategy()
{
  return std::make_unique<DeckLinkCpuCapture>();
}

std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
DeckLinkInputBackend::pickStrategy(QRhi::Implementation impl)
{
  // NVIDIA-DVP GPU-direct via the shared shim. The InputCallback CPU-memcpies
  // each frame into the strategy's slot buffer (no card DMA into sysmem), so
  // the no-op DMA-lock policy is correct. GL + D3D11 only; other backends fall
  // back to the host-staged DeckLinkCpuCapture (makeCpuStrategy).
#if defined(VIDEOIO_DECKLINK_DVP_GL) || defined(VIDEOIO_DECKLINK_DVP_D3D11)
  const NvDvpFormat fmt = deckLinkDvpFormat(m_settings.pixelFormat);
  switch(impl)
  {
#if defined(VIDEOIO_DECKLINK_DVP_GL)
    case QRhi::OpenGLES2:
      return std::make_unique<
          score::gfx::interop::DvpCaptureGl<score::gfx::interop::NoDmaLock>>(
          score::gfx::interop::NoDmaLock{}, fmt, "DVP-GL");
#endif
#if defined(VIDEOIO_DECKLINK_DVP_D3D11)
    case QRhi::D3D11:
      return std::make_unique<
          score::gfx::interop::DvpCaptureD3D11<score::gfx::interop::NoDmaLock>>(
          score::gfx::interop::NoDmaLock{}, fmt, "DVP-D3D11");
#endif
    default:
      return {};
  }
#else
  (void)impl;
  return {};
#endif
}

void DeckLinkInputBackend::start()
{
  if(m_started || !m_input)
    return;
  auto* cb = new InputCallback(
      m_input.get(), m_settings, &m_strategy, m_ring, m_frameByteSize,
      m_rowBytes, m_height);
  cb->m_enabledFlags = m_enableFlags;
  m_cbStopping = &cb->stopping;
  m_callback = ComPtr<IDeckLinkInputCallback>(cb); // adopt ref
  if(m_input->SetCallback(m_callback.get()) != S_OK)
  {
    qWarning() << "DeckLink: SetCallback failed";
    m_callback.reset();
    return;
  }
  if(m_input->StartStreams() != S_OK)
  {
    qWarning() << "DeckLink: StartStreams failed";
    m_input->SetCallback(nullptr);
    m_callback.reset();
    return;
  }
  m_started = true;
}

void DeckLinkInputBackend::stop()
{
  if(m_cbStopping)
    m_cbStopping->store(true, std::memory_order_release);
  if(m_input && m_started)
  {
    // Sample order: stop + flush + disable the input, then detach the
    // callback. FlushStreams drops buffered frames so no in-flight
    // VideoInputFrameArrived can land after we return and the renderer
    // frees the slot ring.
    m_input->StopStreams();
    m_input->FlushStreams();
    m_input->DisableVideoInput();
    m_input->SetCallback(nullptr);
  }
  m_callback.reset();
  m_cbStopping = nullptr;
  m_started = false;
}

} // namespace Gfx::DeckLink
