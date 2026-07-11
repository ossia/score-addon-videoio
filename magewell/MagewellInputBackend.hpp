#pragma once
#include <magewell/Magewell.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <atomic>
#include <array>
#include <cstdint>
#include <thread>

namespace score::gfx::interop
{
struct GpuDirectCaptureSlotRing;
}

namespace Gfx::Magewell
{

struct MagewellInputSettings
{
  int deviceIndex{0};        ///< channel index (MWGetDevicePath)
  std::uint32_t fourcc{MWFOURCC_UYVY}; ///< capture FOURCC (pixel format)
};

/**
 * @brief Magewell Pro Capture backend (score::gfx::DMACaptureBackend).
 *
 * Capture-only + host-staged. Magewell auto-detects the incoming signal, so the
 * geometry (cx/cy/fps) comes from MWGetVideoSignalStatus at open(); only the
 * device index + FOURCC are configured.
 *
 * A dedicated capture thread runs the Pro Capture notify+capture loop:
 * WaitForSingleObject(notify) -> MWGetVideoBufferInfo ->
 * MWCaptureVideoFrameToVirtualAddress(newest full frame -> current slot) ->
 * WaitForSingleObject(capture) -> ingestFrame + publish the slot in the node's
 * ring. The render thread uploads the slot via MagewellCpuCapture.
 *
 * There is no GPU-direct path (pickStrategy returns {}); makeCpuStrategy() is
 * the only strategy. On-card colour-space conversion is available via the
 * MWCaptureVideoFrameToVirtualAddressEx variant — a future enhancement; v1
 * captures directly in the selected FOURCC with MWCaptureVideoFrameToVirtualAddress.
 */
class MagewellInputBackend final : public score::gfx::DMACaptureBackend
{
public:
  MagewellInputBackend(
      MagewellInputSettings settings,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring);
  ~MagewellInputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  pickStrategy(QRhi::Implementation) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::GpuDirectCaptureStrategy* s) noexcept override;
  void start() override;
  void stop() override;

private:
  void runLoop(); ///< Host-staged notify+capture reception loop.

  MagewellInputSettings m_settings;
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;

  HCHANNEL m_channel{nullptr};
  score::gfx::interop::GpuDirectCaptureStrategy* m_strategy{};

  MwEvent m_notifyEvent{mwNoEvent};
  MwEvent m_captureEvent{mwNoEvent};
  HNOTIFY m_notify{0};

  static constexpr std::size_t kMaxSlots = 3;
  std::array<unsigned char*, kMaxSlots> m_pinnedBuffers{}; ///< pinned slot ptrs (for unpin)

  std::thread m_thread;
  std::atomic<bool> m_running{false};

  int m_width{};
  int m_height{};
  std::uint32_t m_stride{};
  uint32_t m_frameByteSize{};
  bool m_captureStarted{}; ///< MWStartVideoCapture succeeded
  bool m_started{};
};

} // namespace Gfx::Magewell
