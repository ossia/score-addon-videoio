#pragma once
#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <atomic>
#include <array>
#include <cstdint>
#include <thread>

namespace score::gfx::interop
{
struct VideoCaptureSlotRing;
}

namespace Gfx::Deltacast
{
struct DeltacastRdmaCapture;

struct DeltacastInputSettings
{
  int deviceIndex{0};
  ULONG videoStandard{0}; ///< 0 = auto-detect the incoming standard
  ULONG bufferPacking{VHD_BUFPACK_VIDEO_YUV422_8};
  bool useRDMA{true};     ///< Try the RDMA GPU-direct path (Vulkan+CUDA) first.
};

/**
 * @brief DELTACAST capture backend (score::gfx::DMACaptureBackend).
 *
 * Host-staged v1: a reception thread runs the VHD slot loop
 * (VHD_LockSlotHandle -> VHD_GetSlotBuffer -> memcpy into the capture
 * strategy's next slot -> VHD_UnlockSlotHandle) and publishes the slot in the
 * node's ring. The render thread uploads the slot via DeltacastCpuCapture.
 * RDMA GPU-direct is a later pass.
 */
class DeltacastInputBackend final : public score::gfx::DMACaptureBackend
{
public:
  DeltacastInputBackend(
      DeltacastInputSettings settings,
      score::gfx::interop::VideoCaptureSlotRing& ring);
  ~DeltacastInputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  pickStrategy(
      QRhi::Implementation,
      const score::gfx::interop::GpuCapabilities&) override;
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::VideoCaptureStrategy* s) noexcept override;
  void start() override;
  void stop() override;

private:
  void runLoop();      ///< Host-staged reception loop (VHD_Lock/Get/Unlock).
  void runLoopRdma();  ///< RDMA reception loop (VHD_QueueInSlot/WaitSlotFilled).
  bool rdmaActive() const noexcept; ///< True once an RDMA strategy is bound.

  DeltacastInputSettings m_settings;
  score::gfx::interop::VideoCaptureSlotRing& m_ring;

  HANDLE m_board{nullptr};
  HANDLE m_stream{nullptr};
  score::gfx::interop::VideoCaptureStrategy* m_strategy{};

  // RDMA path: the strategy created by pickStrategy (also the bound strategy
  // unless its init() failed and the node swapped in a CPU fallback). Used to
  // detect RDMA mode + read slotBuffer(i) for VHD_CreateSlotEx.
  DeltacastRdmaCapture* m_rdmaStrategy{};
  static constexpr std::size_t kRdmaSlotCount = 3;
  std::array<HANDLE, kRdmaSlotCount> m_vhdSlots{};

  std::thread m_thread;
  std::atomic<bool> m_running{false};

  int m_width{};
  int m_height{};
  uint32_t m_frameByteSize{};
  bool m_started{};
  bool m_streamStarted{}; ///< Whether VHD_StartStream succeeded (RDMA defers it).
};

} // namespace Gfx::Deltacast
