#pragma once
#include <AJA/AjaDmaLock.hpp>
#include <AJA/Tier3Common.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Gfx/Graph/interop/ComputeRingDispatcher.hpp>
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/interop/GpuRingBuffer.hpp>
#include <Gfx/Graph/interop/RdmaRingDepth.hpp>
#include <Gfx/Graph/interop/StageProfiler.hpp>
#include <Gfx/Graph/interop/VulkanCudaBounce.hpp>

#include <ntv2card.h>

#include <QDebug>

namespace Gfx::AJA
{

#if defined(SCORE_HAS_VULKAN_CUDA_BOUNCE)

/**
 * @brief Vulkan tier-3 RDMA output for AJA.
 *
 * Same architecture as the GL path, with the CUDA↔graphics bridge
 * inverted: the QRhi compute encoder writes plain QRhi storage buffers
 * (GpuRingBuffer Vulkan plain mode), and one vkCmdCopyBuffer per frame
 * moves the encoded wire frame into a CUDA-VMM bounce that Vulkan
 * imported (VulkanCudaBounce). The bounce is CUDA-allocator memory, so
 * DMABufferLock(inRDMA=true) pins it exactly like the GL path's
 * cuMemAlloc bounce — verified transfer-correct by probe. Frames never
 * touch system memory.
 *
 * Ordering: QRhi::finish() in prepareNextFrame() (mirrors the GL fence /
 * glFinish); timeline semaphores are a later optimisation.
 */
struct RdmaInteropVulkanTier3 final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropVulkanTier3(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};

  QRhi* m_rhi{};
  std::uint32_t m_frameBytes{};
  score::gfx::interop::GpuRingBuffer m_ring;
  score::gfx::interop::ComputeRingDispatcher m_dispatcher;
  score::gfx::interop::VulkanCudaBounce m_bounce;
  std::vector<bool> m_pinned;
  std::uint64_t m_fenceValue{0};

  const char* name() const noexcept override { return "RDMA-Vulkan/T3"; }

  static bool isSupported(QRhi* rhi, NTV2FrameBufferFormat fmt, int width)
  {
    return rhi && rhi->backend() == QRhi::Vulkan
           && rhi->isFeatureSupported(QRhi::Compute)
           && tier3SupportsFormat(fmt, width);
  }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    if(!isSupported(c.rhi, m_targetFormat, c.width) || !c.state)
    {
      qDebug() << "AJA RDMA(Vulkan/T3): unsupported —"
               << "backend" << (c.rhi ? int(c.rhi->backend()) : -1)
               << "compute"
               << (c.rhi && c.rhi->isFeatureSupported(QRhi::Compute))
               << "fmt" << tier3SupportsFormat(m_targetFormat, c.width)
               << "state" << (c.state != nullptr);
      return false;
    }
    m_rhi = c.rhi;
    m_frameBytes = c.frameByteSize;

    // BAR1 budget: same policy as the GL path (see RdmaInteropGLTier3).
    const int slots = score::gfx::interop::rdmaRingDepthForFrame(
        c.frameByteSize, {/*full=*/2, /*large=*/1});

    score::gfx::interop::VulkanCudaBounceConfig bc{};
    bc.rhi = c.rhi;
    bc.slotCount = slots;
    bc.frameBytes = c.frameByteSize;
    bc.debugName = "AJA-RDMA-VK-Bounce";
    if(!m_bounce.init(bc))
    {
      release();
      return false;
    }

    // Pin + verify: pin the CUDA side of each bounce, then probe one real
    // H2C DMA — pin success does not imply the PCIe path works (see the
    // GL path's rationale). Runs before AutoCirculateStart; framestore 0
    // scratch write is harmless.
    m_pinned.assign(std::size_t(slots), false);
    for(int i = 0; i < slots; ++i)
    {
      void* p = m_bounce.cudaPtr(std::size_t(i));
      if(!p || !ajaDmaLock(m_card, p, c.frameByteSize, /*rdma=*/true))
      {
        qWarning() << "AJA RDMA(Vulkan/T3): DMABufferLock failed at slot" << i;
        release();
        return false;
      }
      m_pinned[std::size_t(i)] = true;
    }
    if(!m_card->DMAWriteFrame(
           /*frameNumber=*/0,
           reinterpret_cast<ULWord*>(m_bounce.cudaPtr(0)), c.frameByteSize))
    {
      qWarning() << "AJA RDMA(Vulkan/T3): transfer probe failed — no P2P "
                    "playout path; falling back";
      release();
      return false;
    }

    score::gfx::interop::GpuRingBufferConfig rc{};
    rc.rhi = c.rhi;
    rc.cudaCtx = nullptr; // Vulkan plain mode: no bridge import
    rc.bufferSize = c.frameByteSize;
    rc.slotCount = slots;
    rc.debugName = "AJA-RDMA-VK-Ring";
    if(!m_ring.create(rc))
    {
      release();
      return false;
    }

    score::gfx::interop::ComputeRingDispatcherConfig dc{};
    dc.rhi = c.rhi;
    dc.state = c.state;
    dc.ring = &m_ring;
    dc.sourceTexture = c.sourceTexture;
    dc.width = c.width;
    dc.height = c.height;
    dc.encoderFactory = [fmt = m_targetFormat] { return makeTier3Encoder(fmt); };
    dc.colorConversion = score::gfx::colorMatrixOut(
        AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
    dc.fence = nullptr; // ordering via QRhi::finish() in prepareNextFrame
    if(!m_dispatcher.init(dc))
    {
      qWarning() << "AJA RDMA(Vulkan/T3): dispatcher init failed";
      release();
      return false;
    }
    return true;
  }

  void release() override
  {
    m_dispatcher.release();
    m_ring.destroy();
    for(std::size_t i = 0; i < m_pinned.size(); ++i)
      if(m_pinned[i] && m_bounce.cudaPtr(i))
        ajaDmaUnlock(m_card, m_bounce.cudaPtr(i), m_frameBytes);
    m_pinned.clear();
    m_bounce.release();
    m_fenceValue = 0;
  }

  void encodeFrame(QRhiCommandBuffer& cb) override
  {
    if(!m_ring.valid())
      return;
    m_dispatcher.encode(cb, ++m_fenceValue);
  }

  void* prepareNextFrame() override
  {
    if(!m_ring.valid() || !m_rhi)
      return nullptr;
    SCORE_STAGE_PROFILE(vkPrep, "vkt3-prepare-total");
    // 1. Bridge ring -> bounce in a dedicated tiny frame. Recording the
    //    copy inside the encode frame via beginExternal is NOT reliable on
    //    the deferred-recording Vulkan backend outside a pass — the raw
    //    command lands before the replayed compute pass. A separate frame
    //    gives unambiguous ordering: the copy's pre-barrier's first sync
    //    scope covers everything earlier in SUBMISSION ORDER on the queue
    //    (the already-submitted encode frame), so no host wait is needed
    //    between encode and copy.
    const std::size_t idx = m_ring.writeIndex();
    QRhiCommandBuffer* cb2{};
    if(m_rhi->beginOffscreenFrame(&cb2) != QRhi::FrameOpSuccess || !cb2)
      return nullptr;
    m_bounce.recordCopyToSlot(
        *cb2, m_ring.slot(idx).nativeVkBuffer, m_frameBytes,
        idx % m_bounce.slotCount());
    m_rhi->endOffscreenFrame();
    // 2. Order Vulkan-copy -> CUDA-DtoD. Fast path: empty submit signals
    //    the exported timeline semaphore (covers all prior queue work);
    //    CUDA waits it on the bridge stream — the host never blocks on
    //    Vulkan. Fallback: full finish().
    if(!(m_bounce.signalCopyDoneOnQueue() && m_bounce.waitCopyDoneOnStream()))
      m_rhi->finish();
    // 3. CUDA-side DtoD from the exportable buffer's CUDA view into the
    //    vendor-pinned bounce (stream-synced — the single host block per
    //    frame; the card may DMA on return).
    m_bounce.flushToBounce(idx % m_bounce.slotCount(), m_frameBytes);
    void* p = m_bounce.cudaPtr(idx % m_bounce.slotCount());
    m_ring.advance();
    return p;
  }
};

#else // !SCORE_HAS_VULKAN_CUDA_BOUNCE

struct RdmaInteropVulkanTier3 final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropVulkanTier3(CNTV2Card*, NTV2FrameBufferFormat) noexcept { }
  const char* name() const noexcept override { return "RDMA-Vulkan/T3-stub"; }
  static bool isSupported(QRhi*, NTV2FrameBufferFormat, int) { return false; }
  bool init(const score::gfx::interop::GpuDirectStrategyConfig&) override
  {
    return false;
  }
  void release() override { }
  void encodeFrame(QRhiCommandBuffer&) override { }
  void* prepareNextFrame() override { return nullptr; }
};

#endif

} // namespace Gfx::AJA
