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
#include <cstring>

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
      score::gfx::interop::GpuDirectCaptureStrategy** strategy,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring,
      std::uint32_t frameByteSize, int rowBytes, int height)
      : m_strategy{strategy}
      , m_ring{ring}
      , m_frameByteSize{frameByteSize}
      , m_rowBytes{rowBytes}
      , m_height{height}
  {
  }

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
      BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*,
      BMDDetectedVideoInputFormatFlags) override
  {
    return S_OK; // format autodetect handled later; fixed mode for now
  }

  HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
      IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*) override
  {
    auto* strat = *m_strategy;
    if(!strat || !frame)
      return S_OK;
    // Signal loss produces placeholder frames flagged bmdFrameHasNoInputSource;
    // the samples never forward those (the consumer keeps the last good frame).
    if(frame->GetFlags() & bmdFrameHasNoInputSource)
      return S_OK;
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
  score::gfx::interop::GpuDirectCaptureStrategy** m_strategy{};
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;
  std::uint32_t m_frameByteSize{};
  int m_rowBytes{};
  int m_height{};
  std::size_t m_slot{0};
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

  if(m_input->EnableVideoInput(
         m_settings.displayMode, m_settings.pixelFormat,
         bmdVideoInputFlagDefault)
     != S_OK)
  {
    qWarning() << "DeckLink input: EnableVideoInput failed";
    return false;
  }
  return true;
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
  m_callback = ComPtr<IDeckLinkInputCallback>(new InputCallback(
      &m_strategy, m_ring, m_frameByteSize, m_rowBytes, m_height)); // adopt ref
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
  m_started = false;
}

} // namespace Gfx::DeckLink
