#pragma once
#include <decklink/DeckLink.hpp>
#include <decklink/DeckLinkComPtr.hpp>

#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <vector>

namespace Gfx::DeckLink
{

struct DeckLinkOutputSettings
{
  int deviceIndex{0};
  BMDDisplayMode displayMode{bmdModeHD1080p5994};
  BMDPixelFormat pixelFormat{bmdFormat8BitYUV};
};

/**
 * @brief DeckLink playout backend (score::gfx::DirectVideoOutputBackend).
 *
 * Scheduled playback per the SignalGenerator sample model, with the sample's
 * key invariant: a frame's pixels are immutable from ScheduleVideoFrame until
 * its ScheduledFrameCompleted. The backend owns a small pool of SDK-allocated
 * IDeckLinkMutableVideoFrames; submitFrame() copies the staged bytes out of
 * the (reusable) host ring into a free pool frame and schedules that, so the
 * driver never retains a pointer into CpuStagedVideoOutput's ring. Pacing bridges
 * DeckLink's push-model completion callback to the pump's pull-model via a
 * counting semaphore: one permit per FREE POOL FRAME (seeded with the pool,
 * consumed by waitForTick — which the pump only calls when a frame is
 * pending — and released by each completion, or given back on a failed
 * submit). StartScheduledPlayback fires once GetBufferedVideoFrameCount
 * reaches the preroll depth, per the samples.
 */
class DeckLinkOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit DeckLinkOutputBackend(DeckLinkOutputSettings settings);
  ~DeckLinkOutputBackend() override;

  bool open(score::gfx::GraphicsApi api) override;
  void quiesce() override;
  void close() override;

  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  double frameRate() const noexcept override { return m_frameRate; }
  bool isOpen() const noexcept override { return m_open; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  int visibleRows() const noexcept override { return m_height; }
  score::gfx::interop::VideoPixelFormat wireFormat() const noexcept override;
  bool prefersFloatRender() const noexcept override;
  QString colorConversion() const override;
  std::vector<score::gfx::interop::HostStagedPlane> planes() const override;
  score::gfx::interop::VendorDmaRegistrar registrar() override;
  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
  gpuDirectCandidates(QRhi*, score::gfx::GraphicsApi) override
  {
    // No single-buffer GPU-direct strategy: DeckLink's async scheduled playback
    // needs a frame ring, which the host-staged path provides. Output DVP is
    // engaged via prefersGpuDownload() -> CpuStagedVideoOutput's HostPinnedRing.
    return {};
  }
  /// Opt into the GPU-direct (DVP) download in CpuStagedVideoOutput: the encoder
  /// texture is DMA'd straight into the pinned ring slots, skipping the QRhi
  /// readback; submitFrame() then copies the slot into a pool frame. Falls
  /// back to CPU readback when no DVP backend is present.
  bool prefersGpuDownload() const noexcept override;
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

  /// Driver-thread entry points, invoked by the completion callback.
  void onFrameCompleted(IDeckLinkVideoFrame* frame) noexcept;
  void onPlaybackStopped() noexcept;
  /// Count a late/dropped completion; returns the new total (for sampled logs).
  std::uint64_t noteLateCompletion() noexcept
  {
    return m_lateCompletions.fetch_add(1, std::memory_order_relaxed) + 1;
  }

private:
  bool waitForTick();
  bool submitFrame(void* framePtr);
  void startPlaybackIfPrerolled();
  void drainPermits() noexcept;

  DeckLinkOutputSettings m_settings;

  ComPtr<IDeckLink> m_device;
  ComPtr<IDeckLinkOutput> m_output;
  /// Config settings revert when the IDeckLinkConfiguration object is
  /// released — keep it alive for the stream's lifetime (444 wire flag).
  ComPtr<IDeckLinkConfiguration> m_cfg;
  ComPtr<IDeckLinkVideoOutputCallback> m_callback;

  // Completion-tracked frame pool. m_pool owns every frame for the session;
  // m_free holds the subset not currently scheduled (non-owning). Guarded by
  // m_poolMutex (pump thread pops, DeckLink's callback thread pushes).
  std::mutex m_poolMutex;
  std::vector<ComPtr<IDeckLinkMutableVideoFrame>> m_pool;
  std::vector<IDeckLinkMutableVideoFrame*> m_free;

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{};
  double m_frameRate{60.0};
  BMDTimeValue m_frameDuration{1001};
  BMDTimeScale m_timeScale{60000};

  std::uint64_t m_frameCount{0}; ///< scheduled-frame display-time counter
  std::uint64_t m_lateResyncs{0}; ///< frames skipped catching the clock up
  std::atomic<std::uint64_t> m_lateCompletions{0};
  bool m_started{false};
  bool m_quiesced{false};
  bool m_open{false};

  /// Frames the driver buffers before StartScheduledPlayback (samples gate on
  /// GetBufferedVideoFrameCount >= preroll).
  static constexpr uint32_t kPreroll = 3;
  /// Pool depth: preroll + headroom so a burst never starves the free list.
  static constexpr int kPoolSize = 5;
  std::counting_semaphore<64> m_freeSlots{0};

  // quiesce() waits here for ScheduledPlaybackHasStopped, per the SDK
  // samples' "recommended to wait before disabling output".
  std::mutex m_stopMutex;
  std::condition_variable m_stopCv;
  bool m_playbackStopped{false};
};

} // namespace Gfx::DeckLink
