#pragma once
#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace Gfx::Deltacast
{
struct DeltacastRdmaOutput;

struct DeltacastOutputSettings
{
  int deviceIndex{0};
  ULONG videoStandard{VHD_VIDEOSTD_S274M_1080p_60Hz}; ///< VHD_VIDEOSTANDARD
  ULONG bufferPacking{VHD_BUFPACK_VIDEO_YUV422_8};    ///< VHD_BUFFERPACKING
  bool fractionalClock{false}; ///< /1.001 rate (59.94/29.97/23.98) -> CLOCKDIV_1001
  bool useRDMA{true};  ///< Try the RDMA GPU-direct playout path (Vulkan+CUDA) first.
};

/**
 * @brief DELTACAST SDI playout backend (score::gfx::DirectVideoOutputBackend).
 *
 * Host-staged v1 using the SDK-owned slot model: CpuStagedVideoOutput renders +
 * reads back the wire-format frame into its pinned ring, then submitFrame()
 * copies it into a VHD slot (VHD_LockSlotHandle -> VHD_GetSlotBuffer ->
 * memcpy -> VHD_UnlockSlotHandle). VHD_UnlockSlotHandle blocks until the board
 * consumes the slot, which paces the pump to genlock. GPU-direct (RDMA) later.
 */
class DeltacastOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit DeltacastOutputBackend(DeltacastOutputSettings settings);
  ~DeltacastOutputBackend() override;

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
  gpuDirectCandidates(QRhi* rhi, score::gfx::GraphicsApi api) override;
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

  /// Called by DeltacastRdmaOutput::init() once its RDMA GPU VRAM slots exist:
  /// registers each gpuVA as a Deltacast "application buffer" (VHD_CreateSlotEx
  /// RDMAEnabled=TRUE) and starts the stream (the StartStream deferred from
  /// open() in RDMA mode). `slotCapacity` is the usable byte size of each GPU
  /// allocation — validated against VHD_GetApplicationBuffersSize so the card
  /// can never DMA past the end of a slot. Returns false on any failure (after
  /// destroying partial registrations and resetting the stream handle), so the
  /// strategy fails init() and the node falls back to the host-staged path.
  bool registerRdmaOutputSlots(
      void* const* gpuVAs, std::size_t n, std::size_t slotCapacity);

  /// True while `gpuVA` is queued to (or being sent by) the board.
  /// DeltacastRdmaOutput::prepareNextFrame() skips busy slots so the CUDA copy
  /// never overwrites VRAM the card is still DMA-reading.
  bool rdmaSlotBusy(void* gpuVA);

private:
  bool submitFrame(void* framePtr);
  bool applyStreamProperties();
  /// After a failed RDMA registration: the stream is stuck in application-
  /// buffers mode (VHD_InitApplicationBuffers cannot be undone), which the
  /// host-staged slot model cannot use. Close and reopen the stream handle
  /// with the original properties so the lazy StartStream fallback works.
  bool resetStreamForFallback();

  DeltacastOutputSettings m_settings;

  HANDLE m_board{nullptr};
  HANDLE m_stream{nullptr};

  // RDMA playout: gpuVA -> VHD slot handle map, so submitFrame() can resolve the
  // pointer prepareNextFrame() returned back to the slot to VHD_QueueOutSlot().
  std::vector<std::pair<void*, HANDLE>> m_rdmaSlots;
  bool m_rdmaMode{false};

  // Slots currently queued/being sent (pump thread inserts on QueueOutSlot,
  // erases when VHD_WaitSlotSent reports them sent; render thread reads via
  // rdmaSlotBusy).
  std::mutex m_busyMutex;
  std::vector<void*> m_busySlots;

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{};
  double m_frameRate{60.0};
  bool m_open{false};
  bool m_started{false};
};

} // namespace Gfx::Deltacast
