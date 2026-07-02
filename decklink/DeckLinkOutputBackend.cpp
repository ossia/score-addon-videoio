#include <decklink/DeckLinkOutputBackend.hpp>

#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkFormats.hpp>

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

namespace Gfx::DeckLink
{
namespace
{

/// Scheduled-playback completion callback. Routes the driver-thread events to
/// the backend: each completed frame returns to the free pool (+ one pacing
/// permit), and ScheduledPlaybackHasStopped wakes quiesce().
class CompletionCallback final : public IDeckLinkVideoOutputCallback
{
public:
  explicit CompletionCallback(DeckLinkOutputBackend& backend)
      : m_backend{backend}
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
  {
    if(!ppv)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IUnknown)
       || IsEqualIID(iid, IID_IDeckLinkVideoOutputCallback))
    {
      *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
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

  HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
      IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result) override
  {
    if(result == bmdOutputFrameDisplayedLate || result == bmdOutputFrameDropped)
      qDebug() << "DeckLink: scheduled frame"
               << (result == bmdOutputFrameDropped ? "dropped" : "late");
    m_backend.onFrameCompleted(frame);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
  {
    m_backend.onPlaybackStopped();
    return S_OK;
  }

private:
  DeckLinkOutputBackend& m_backend;
  std::atomic<ULONG> m_ref{1};
};

} // namespace

DeckLinkOutputBackend::DeckLinkOutputBackend(DeckLinkOutputSettings settings)
    : m_settings{settings}
{
}

DeckLinkOutputBackend::~DeckLinkOutputBackend()
{
  close();
}

bool DeckLinkOutputBackend::open(score::gfx::GraphicsApi)
{
  ensureComInit();
  m_device = openDevice(m_settings.deviceIndex);
  if(!m_device)
    return false;
  if(m_device->QueryInterface(IID_IDeckLinkOutput, m_output.putVoid()) != S_OK
     || !m_output)
    return false;

  // A previous session must not leak pacing/clock state into this one: the
  // scheduled-playback clock restarts at 0 on StartScheduledPlayback, so the
  // display-time counter and the permit count both restart with it.
  drainPermits();
  m_frameCount = 0;
  m_started = false;
  m_quiesced = false;
  m_playbackStopped = false;

  // Resolve the display mode -> geometry + frame rate.
  ComPtr<IDeckLinkDisplayMode> mode;
  ComPtr<IDeckLinkDisplayModeIterator> modeIt;
  if(m_output->GetDisplayModeIterator(modeIt.put()) == S_OK && modeIt)
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
    qWarning() << "DeckLink: display mode not supported";
    return false;
  }
  m_width = static_cast<int>(mode->GetWidth());
  m_height = static_cast<int>(mode->GetHeight());
  BMDTimeValue dur = 0;
  BMDTimeScale scale = 0;
  if(mode->GetFrameRate(&dur, &scale) == S_OK && dur > 0)
  {
    m_frameDuration = dur;
    m_timeScale = scale;
    m_frameRate = double(scale) / double(dur);
  }

  int rb = 0;
  if(m_output->RowBytesForPixelFormat(m_settings.pixelFormat, m_width, &rb)
         != S_OK
     || rb <= 0)
  {
    qWarning() << "DeckLink: RowBytesForPixelFormat failed for this "
                  "mode/pixel-format combination";
    return false;
  }
  m_rowBytes = rb;
  m_frameByteSize = static_cast<uint32_t>(rb) * static_cast<uint32_t>(m_height);

  if(m_output->EnableVideoOutput(m_settings.displayMode, bmdVideoOutputFlagDefault)
     != S_OK)
  {
    qWarning() << "DeckLink: EnableVideoOutput failed";
    return false;
  }

  m_callback = ComPtr<IDeckLinkVideoOutputCallback>(
      new CompletionCallback(*this)); // adopt the initial ref
  if(m_output->SetScheduledFrameCompletionCallback(m_callback.get()) != S_OK)
  {
    qWarning() << "DeckLink: SetScheduledFrameCompletionCallback failed";
    close();
    return false;
  }

  // Completion-tracked frame pool: the driver only ever sees these SDK-
  // allocated frames, never HostStagedOutput's ring memory, so a frame's
  // bytes stay immutable from ScheduleVideoFrame until its completion (the
  // SignalGenerator sample's invariant). One pacing permit per pool frame.
  {
    std::lock_guard lock{m_poolMutex};
    m_pool.reserve(kPoolSize);
    m_free.reserve(kPoolSize);
    for(int i = 0; i < kPoolSize; ++i)
    {
      ComPtr<IDeckLinkMutableVideoFrame> frame;
      if(m_output->CreateVideoFrame(
             m_width, m_height, m_rowBytes, m_settings.pixelFormat,
             bmdFrameFlagDefault, frame.put())
             != S_OK
         || !frame)
      {
        qWarning() << "DeckLink: CreateVideoFrame failed at pool slot" << i;
        m_pool.clear();
        m_free.clear();
        close();
        return false;
      }
      m_free.push_back(frame.get());
      m_pool.push_back(std::move(frame));
    }
  }
  m_freeSlots.release(kPoolSize);

  m_open = true;
  return true;
}

void DeckLinkOutputBackend::quiesce()
{
  // Stop the scheduled queue and wait for ScheduledPlaybackHasStopped before
  // the node releases the staging ring — the samples' documented shutdown
  // order ("recommended to wait ... before disabling output"). After this the
  // driver holds no reference into any of our buffers.
  if(!m_output || !m_started)
  {
    m_quiesced = true;
    return;
  }

  {
    std::lock_guard lock{m_stopMutex};
    m_playbackStopped = false;
  }
  BMDTimeValue actualStop = 0;
  m_output->StopScheduledPlayback(0, &actualStop, m_timeScale);
  {
    std::unique_lock lock{m_stopMutex};
    if(!m_stopCv.wait_for(lock, std::chrono::milliseconds(500), [this] {
         return m_playbackStopped;
       }))
      qWarning() << "DeckLink: timed out waiting for ScheduledPlaybackHasStopped";
  }
  m_started = false;
  m_quiesced = true;
}

void DeckLinkOutputBackend::close()
{
  if(m_output)
  {
    if(!m_quiesced)
      quiesce();
    m_output->SetScheduledFrameCompletionCallback(nullptr);
    m_output->DisableVideoOutput();
  }
  {
    std::lock_guard lock{m_poolMutex};
    m_free.clear();
    m_pool.clear(); // releases our refs; the driver's are gone after quiesce()
  }
  drainPermits();
  m_frameCount = 0;
  m_started = false;
  m_quiesced = false;
  m_callback.reset();
  m_output.reset();
  m_device.reset();
  m_open = false;
}

score::gfx::interop::VideoPixelFormat
DeckLinkOutputBackend::wireFormat() const noexcept
{
  return toNeutralFormat(m_settings.pixelFormat);
}

bool DeckLinkOutputBackend::prefersFloatRender() const noexcept
{
  switch(m_settings.pixelFormat)
  {
    case bmdFormat10BitYUV:
    case bmdFormat10BitRGB:
    case bmdFormat12BitRGB:
    case bmdFormat12BitRGBLE:
      return true;
    default:
      return false;
  }
}

QString DeckLinkOutputBackend::colorConversion() const
{
  // SDR BT.709 limited range. HDR (via IDeckLinkVideoFrameMutableMetadata
  // Extensions) is a later pass.
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
}

std::vector<score::gfx::interop::HostStagedPlane>
DeckLinkOutputBackend::planes() const
{
  return {{m_rowBytes, m_frameByteSize}}; // DeckLink wire formats are packed (1 plane)
}

score::gfx::interop::VendorDmaRegistrar DeckLinkOutputBackend::registrar()
{
  // The staging ring is never handed to the driver (submitFrame copies into
  // the completion-tracked pool), so the slots need no vendor registration.
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [](void*, std::uint32_t) { return true; };
  reg.releaseSlot = [](void*, std::uint32_t) {};
  return reg;
}

bool DeckLinkOutputBackend::prefersGpuDownload() const noexcept
{
#if defined(SCORE_HAS_AJA_DVP_BRIDGE)
  return true; // nv_dvp_bridge linked: HostStagedOutput can DVP texture->sysmem
#else
  return false;
#endif
}

score::gfx::interop::PacedFramePump::Hooks DeckLinkOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [this] { return waitForTick(); };
  h.canAccept = {}; // back-pressure is the free-pool semaphore in waitForTick
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool DeckLinkOutputBackend::waitForTick()
{
  // One permit == one free pool frame. The pump only calls this when a frame
  // is already pending, so a consumed permit is always followed by a submit
  // (which returns it on failure); completions release new permits.
  return m_freeSlots.try_acquire_for(std::chrono::milliseconds(100));
}

bool DeckLinkOutputBackend::submitFrame(void* framePtr)
{
  if(!m_output || !framePtr)
  {
    m_freeSlots.release(); // give back the permit from waitForTick
    return false;
  }

  IDeckLinkMutableVideoFrame* frame = nullptr;
  {
    std::lock_guard lock{m_poolMutex};
    if(!m_free.empty())
    {
      frame = m_free.back();
      m_free.pop_back();
    }
  }
  if(!frame)
  {
    // Cannot happen while the permit accounting holds; recover anyway.
    m_freeSlots.release();
    return false;
  }

  // SDK 16.0: frame bytes are accessed via IDeckLinkVideoBuffer, bracketed by
  // StartAccess/EndAccess (the samples' ScopedBufferBytes pattern).
  ComPtr<IDeckLinkVideoBuffer> buf;
  if(frame->QueryInterface(IID_IDeckLinkVideoBuffer, buf.putVoid()) != S_OK
     || !buf || buf->StartAccess(bmdBufferAccessWrite) != S_OK)
  {
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(frame);
    m_freeSlots.release();
    return false;
  }
  void* dst = nullptr;
  if(buf->GetBytes(&dst) != S_OK || !dst)
  {
    buf->EndAccess(bmdBufferAccessWrite);
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(frame);
    m_freeSlots.release();
    return false;
  }
  std::memcpy(dst, framePtr, m_frameByteSize);
  buf->EndAccess(bmdBufferAccessWrite);

  const HRESULT hr = m_output->ScheduleVideoFrame(
      frame, static_cast<BMDTimeValue>(m_frameCount * m_frameDuration),
      m_frameDuration, m_timeScale);
  if(FAILED(hr))
  {
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(frame);
    m_freeSlots.release();
    return false;
  }

  ++m_frameCount;
  if(!m_started)
    startPlaybackIfPrerolled();
  return true;
}

void DeckLinkOutputBackend::startPlaybackIfPrerolled()
{
  // Gate on the device-reported buffered count, not our own submit count, so
  // driver-side preroll drops can't wedge the start (sample practice).
  uint32_t buffered = 0;
  if(m_output->GetBufferedVideoFrameCount(&buffered) != S_OK
     || buffered < kPreroll)
    return;
  if(m_output->StartScheduledPlayback(0, m_timeScale, 1.0) == S_OK)
    m_started = true;
  else
    qWarning() << "DeckLink: StartScheduledPlayback failed";
}

void DeckLinkOutputBackend::onFrameCompleted(IDeckLinkVideoFrame* frame) noexcept
{
  auto* mutableFrame = static_cast<IDeckLinkMutableVideoFrame*>(frame);
  {
    std::lock_guard lock{m_poolMutex};
    // Only pool frames come back through here; guard against duplicates or a
    // completion racing close() (pool already cleared).
    const bool inPool = std::any_of(
        m_pool.begin(), m_pool.end(),
        [&](const auto& f) { return f.get() == mutableFrame; });
    const bool alreadyFree
        = std::find(m_free.begin(), m_free.end(), mutableFrame) != m_free.end();
    if(!inPool || alreadyFree)
      return;
    m_free.push_back(mutableFrame);
  }
  m_freeSlots.release();
}

void DeckLinkOutputBackend::onPlaybackStopped() noexcept
{
  {
    std::lock_guard lock{m_stopMutex};
    m_playbackStopped = true;
  }
  m_stopCv.notify_all();
}

void DeckLinkOutputBackend::drainPermits() noexcept
{
  while(m_freeSlots.try_acquire())
    ;
}

} // namespace Gfx::DeckLink
