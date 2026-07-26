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
#include <mutex>
#include <chrono>
#include <cstring>

namespace Gfx::DeckLink
{
namespace
{
/// App-owned, page-aligned frame storage for the pool, handed to
/// IDeckLinkOutput::CreateVideoFrameWithBuffer (SDK 12.9+).
///
/// By default CreateVideoFrame allocates frame memory inside the SDK with no
/// alignment guarantee and hands out GetBytes() pointers at an offset inside
/// its own buffers. Owning the buffer gives frame bytes at offset 0 of an
/// allocation the GPU can wrap: a VirtualAlloc region on Windows (the only
/// thing D3D12's OpenExistingHeapFromAddress accepts, sized in whole 64 KiB
/// so it covers the placed buffer's 64 KiB-aligned allocation size) and a
/// 4096-aligned allocation elsewhere (VK_EXT_external_memory_host /
/// GL_AMD_pinned_memory page requirement).
class HostFrameBuffer final : public IDeckLinkVideoBuffer
{
public:
#if defined(_WIN32)
  static constexpr std::size_t kRound = 65536;
#else
  static constexpr std::size_t kRound = 4096;
#endif

  explicit HostFrameBuffer(std::size_t bytes)
      : m_size{(bytes + kRound - 1) / kRound * kRound}
  {
#if defined(_WIN32)
    m_data = VirtualAlloc(nullptr, m_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    m_data = std::aligned_alloc(kRound, m_size);
#endif
  }
  ~HostFrameBuffer()
  {
#if defined(_WIN32)
    if(m_data)
      VirtualFree(m_data, 0, MEM_RELEASE);
#else
    std::free(m_data);
#endif
  }

  void* data() const noexcept { return m_data; }
  std::size_t size() const noexcept { return m_size; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
  {
    if(!ppv)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDeckLinkVideoBuffer))
    {
      *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
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

  HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
  {
    if(!buffer)
      return E_POINTER;
    *buffer = m_data;
    return m_data ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE GetSize(ULONGLONG* size) override
  {
    if(!size)
      return E_POINTER;
    *size = m_size;
    return S_OK;
  }
  // Plain host memory is always accessible; access bracketing has nothing to
  // map or flush.
  HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags) override
  {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags) override
  {
    return S_OK;
  }

private:
  std::atomic<ULONG> m_ref{1};
  void* m_data{};
  std::size_t m_size{};
};
} // namespace

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
    // Late/dropped completions are expected transients around producer
    // hiccups (submitFrame resyncs the schedule clock); log a sample, not a
    // storm — the totals are reported by the backend at close().
    if(result == bmdOutputFrameDisplayedLate || result == bmdOutputFrameDropped)
    {
      const auto n = m_backend.noteLateCompletion();
      if((n & (n - 1)) == 0) // 1, 2, 4, 8, ...
        qDebug() << "DeckLink: scheduled frame"
                 << (result == bmdOutputFrameDropped ? "dropped" : "late")
                 << "(total" << n << ")";
    }
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
  m_directPacing.store(false, std::memory_order_relaxed);
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

  // RGB pixel formats need 4:4:4 signalled on the SDI wire; without this the
  // card ships a muted/converted signal and a 4:4:4-expecting receiver sees
  // black. The config PERSISTS in the driver, so explicitly clear it for YUV
  // formats too (a previous RGB session must not poison a YUV one).
  {
    const bool rgbWire = m_settings.pixelFormat != bmdFormat8BitYUV
                         && m_settings.pixelFormat != bmdFormat10BitYUV;
    // The setting only holds while the configuration object lives (it reverts
    // on release), so this must be the long-lived member, not a temporary.
    if(m_device->QueryInterface(IID_IDeckLinkConfiguration, m_cfg.putVoid())
           == S_OK
       && m_cfg)
    {
      if(m_cfg->SetFlag(bmdDeckLinkConfig444SDIVideoOutput, rgbWire) != S_OK
         && rgbWire)
        qWarning() << "DeckLink: could not enable 4:4:4 SDI output";
      // The Studio 4K mirrors playout to SDI and HDMI, but its HDMI encoder
      // emits no TMDS at all for RGB framebuffers (Desktop Video 16.1a3,
      // SDK-verified) — only the SDI mirror carries (converted) video.
      if(rgbWire)
        qWarning() << "DeckLink: RGB playout — the HDMI mirror of this "
                      "output will carry no signal on this hardware; use the "
                      "SDI connector or a YUV pixel format for HDMI.";
    }
  }

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

  // Completion-tracked frame pool: the driver only ever sees these pool
  // frames, never CpuStagedVideoOutput's ring memory, so a frame's bytes stay
  // immutable from ScheduleVideoFrame until its completion (the
  // SignalGenerator sample's invariant). One pacing permit per pool frame.
  //
  // The frames wrap our own page-aligned HostFrameBuffers
  // (CreateVideoFrameWithBuffer) so the GPU can write them directly;
  // SCORE_DECKLINK_SDK_ALLOCATOR falls back to SDK-allocated frames (and
  // thereby disables the direct-readback path).
  {
    const bool ownMemory
        = !qEnvironmentVariableIsSet("SCORE_DECKLINK_SDK_ALLOCATOR");
    std::lock_guard lock{m_poolMutex};
    m_pool.reserve(kPoolSize);
    m_free.reserve(kPoolSize);
    for(int i = 0; i < kPoolSize; ++i)
    {
      ComPtr<IDeckLinkMutableVideoFrame> frame;
      if(ownMemory)
      {
        auto* raw = new HostFrameBuffer(std::size_t(m_frameByteSize));
        ComPtr<IDeckLinkVideoBuffer> buf{raw}; // adopt the initial ref
        if(raw->data()
           && m_output->CreateVideoFrameWithBuffer(
                  m_width, m_height, m_rowBytes, m_settings.pixelFormat,
                  bmdFrameFlagDefault, buf.get(), frame.put())
                  == S_OK
           && frame)
        {
          m_frameRegions.push_back({raw->data(), raw->size()});
          m_frameBuffers.push_back(std::move(buf));
        }
        else
        {
          qWarning() << "DeckLink: CreateVideoFrameWithBuffer failed at pool"
                     << i << "- falling back to SDK frame memory";
          frame.reset();
        }
      }
      if(!frame
         && (m_output->CreateVideoFrame(
                 m_width, m_height, m_rowBytes, m_settings.pixelFormat,
                 bmdFrameFlagDefault, frame.put())
                 != S_OK
             || !frame))
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
    if(m_lateResyncs || m_lateCompletions.load(std::memory_order_relaxed))
      qDebug() << "DeckLink output: session totals — late/dropped completions"
               << m_lateCompletions.load(std::memory_order_relaxed)
               << ", frames skipped by clock resync" << m_lateResyncs;
    if(!m_quiesced)
      quiesce();
    m_output->SetScheduledFrameCompletionCallback(nullptr);
    m_output->DisableVideoOutput();
  }
  releaseAcquiredFrames();
  {
    std::lock_guard lock{m_poolMutex};
    m_free.clear();
    m_pool.clear(); // releases our refs; the driver's are gone after quiesce()
  }
  // After the frames: each frame holds a reference to its buffer.
  m_frameRegions.clear();
  m_frameBuffers.clear();
  drainPermits();
  m_directPacing.store(false, std::memory_order_relaxed);
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
  return true; // nv_dvp_bridge linked: CpuStagedVideoOutput can DVP texture->sysmem
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
  // Direct-readback frames the pump drops carry a pool slot + permit; staging
  // ring pointers are not in m_acquired and pass through as a no-op.
  h.discard = [this](void* p) { cancelFrameMemory(p); };
  return h;
}

score::gfx::interop::FrameMemoryProvider DeckLinkOutputBackend::frameMemoryProvider()
{
  score::gfx::interop::FrameMemoryProvider p;
  p.acquire = [this] { return acquireFrameMemory(); };
  p.cancel = [this](void* bytes) { cancelFrameMemory(bytes); };
  return p;
}

score::gfx::interop::VendorFrameMemory DeckLinkOutputBackend::acquireFrameMemory()
{
  if(!m_output || m_frameRegions.empty())
    return {};
  // The permit is this frame's back-pressure: none free => the caller drops
  // the render tick, exactly as waitForTick would have.
  if(!m_freeSlots.try_acquire())
    return {};

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
    m_freeSlots.release();
    return {};
  }

  ComPtr<IDeckLinkVideoBuffer> buf;
  if(frame->QueryInterface(IID_IDeckLinkVideoBuffer, buf.putVoid()) != S_OK
     || !buf || buf->StartAccess(bmdBufferAccessWrite) != S_OK)
  {
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(frame);
    m_freeSlots.release();
    return {};
  }
  void* bytes = nullptr;
  score::gfx::interop::VendorFrameMemory mem;
  if(buf->GetBytes(&bytes) == S_OK && bytes)
  {
    for(const auto& [base, size] : m_frameRegions)
    {
      if(bytes >= base
         && static_cast<char*>(bytes) < static_cast<char*>(base) + size)
      {
        mem.regionBase = base;
        mem.regionBytes = size;
        break;
      }
    }
  }
  if(!bytes || !mem.regionBase)
  {
    if(bytes)
      qWarning() << "DeckLink: frame bytes" << bytes
                 << "are not from our buffers - direct readback unavailable";
    buf->EndAccess(bmdBufferAccessWrite);
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(frame);
    m_freeSlots.release();
    return {};
  }
  mem.bytes = bytes;

  {
    std::lock_guard lock{m_poolMutex};
    m_acquired.push_back({bytes, frame, std::move(buf)});
  }
  m_directPacing.store(true, std::memory_order_relaxed);
  return mem;
}

void DeckLinkOutputBackend::cancelFrameMemory(void* bytes)
{
  AcquiredFrame af;
  {
    std::lock_guard lock{m_poolMutex};
    auto it = std::find_if(
        m_acquired.begin(), m_acquired.end(),
        [&](const AcquiredFrame& a) { return a.bytes == bytes; });
    if(it == m_acquired.end())
      return;
    af = std::move(*it);
    m_acquired.erase(it);
  }
  af.buf->EndAccess(bmdBufferAccessWrite);
  {
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(af.frame);
  }
  m_freeSlots.release();
}

bool DeckLinkOutputBackend::waitForTick()
{
  // One permit == one free pool frame. The pump only calls this when a frame
  // is already pending, so a consumed permit is always followed by a submit
  // (which returns it on failure); completions release new permits. In direct
  // pacing the pending frame consumed its permit in acquireFrameMemory().
  if(m_directPacing.load(std::memory_order_relaxed))
    return true;
  return m_freeSlots.try_acquire_for(std::chrono::milliseconds(100));
}

bool DeckLinkOutputBackend::submitFrame(void* framePtr)
{
  if(!m_output || !framePtr)
  {
    if(!m_directPacing.load(std::memory_order_relaxed))
      m_freeSlots.release(); // give back the permit from waitForTick
    return false;
  }

  // Direct-readback frame: the GPU already wrote the pool frame's bytes -
  // close the write access and schedule that very frame.
  {
    AcquiredFrame af;
    bool found = false;
    {
      std::lock_guard lock{m_poolMutex};
      auto it = std::find_if(
          m_acquired.begin(), m_acquired.end(),
          [&](const AcquiredFrame& a) { return a.bytes == framePtr; });
      if(it != m_acquired.end())
      {
        af = std::move(*it);
        m_acquired.erase(it);
        found = true;
      }
    }
    if(found)
    {
      af.buf->EndAccess(bmdBufferAccessWrite);
      return scheduleFilledFrame(af.frame);
    }
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

  return scheduleFilledFrame(frame);
}

bool DeckLinkOutputBackend::scheduleFilledFrame(IDeckLinkMutableVideoFrame* frame)
{
  // Display-time resync. Times are absolute (frameCount * duration), so if the
  // producer ever falls behind the playback clock, every subsequent frame
  // would be scheduled in the past and completed "late/dropped" forever (the
  // SignalGenerator sample can't hit this: its producer IS the completion
  // callback). When the next slot is no longer ahead of the clock, jump the
  // counter to one full frame past the currently-displaying one.
  if(m_started)
  {
    BMDTimeValue streamTime = 0;
    double speed = 1.0;
    if(m_output->GetScheduledStreamTime(m_timeScale, &streamTime, &speed)
       == S_OK)
    {
      const auto next = static_cast<BMDTimeValue>(m_frameCount * m_frameDuration);
      if(next <= streamTime)
      {
        const std::uint64_t resynced
            = static_cast<std::uint64_t>(streamTime / m_frameDuration) + 2;
        m_lateResyncs += resynced - m_frameCount;
        m_frameCount = resynced;
      }
    }
  }

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

void DeckLinkOutputBackend::releaseAcquiredFrames() noexcept
{
  std::vector<AcquiredFrame> acquired;
  {
    std::lock_guard lock{m_poolMutex};
    acquired = std::move(m_acquired);
    m_acquired.clear();
  }
  for(auto& af : acquired)
  {
    af.buf->EndAccess(bmdBufferAccessWrite);
    std::lock_guard lock{m_poolMutex};
    m_free.push_back(af.frame);
  }
}

} // namespace Gfx::DeckLink
