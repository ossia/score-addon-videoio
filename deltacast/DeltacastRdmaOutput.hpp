#pragma once
/**
 * @file DeltacastRdmaOutput.hpp
 * @brief DELTACAST RDMA GPU-direct playout (Vulkan QRhi + CUDA) — output "Seam B".
 *
 * The true GPU-direct counterpart to the host-staged Deltacast playout path and
 * the symmetric inverse of DeltacastRdmaCapture: instead of
 * (card -> VRAM -> cuda_p2p_copy_buffer_to_array -> decoder texture), the output
 * path is (RGBA scene -> wire encoder texture -> cuda_p2p_copy_array_to_buffer
 * -> RDMA-capable GPU VRAM -> the card DMAs it out). No host transit.
 *
 * Reuses, verbatim, the same primitives the capture strategy uses:
 *   - the Vulkan-handle acquisition + cuda_p2p_init + CudaFunctions load,
 *   - an exportable VkImage adopted as a QRhiTexture + cuda_p2p_import_vulkan_image,
 *   - RdmaGpuBuffer(RdmaGpuApi::Cuda) for the per-slot RDMA GPU VRAM,
 * plus the shared wire encoder (score::gfx::makeWireEncoder + the encoder's
 * outputTexture() virtual) for the RGBA->wire conversion.
 *
 *   init (render thread):
 *     - build the wire encoder; it renders RGBA -> the card's wire bytes into an
 *       RGBA8 QRhi texture (encTex).
 *     - allocate an exportable VkImage matching encTex, import it into CUDA as a
 *       CUarray (m_exportArray) and adopt it into QRhi (m_exportTex) — this is the
 *       blit *destination* for encTex and the CUDA copy *source*.
 *     - allocate kSlotCount RDMA GPU buffers via RdmaGpuBuffer(Cuda); each slot's
 *       gpuVA is BOTH the CUDA copy destination AND the card's DMA source. Then
 *       register the slots with the backend's VHD stream
 *       (DeltacastOutputBackend::registerRdmaOutputSlots -> VHD_CreateSlotEx
 *       RDMAEnabled=TRUE + VHD_StartStream).
 *
 *   encodeFrame (render thread, inside the offscreen frame): run the encoder
 *     (RGBA -> wire into encTex), then QRhi-copyTexture encTex -> m_exportTex so
 *     the bytes land in the CUDA-imported image's backing memory.
 *
 *   prepareNextFrame (render thread, after endOffscreenFrame): one synchronous
 *     CUDA copy array(m_exportArray) -> the next RDMA slot, and return that
 *     slot's gpuVA — which the backend's submitFrame() maps back to its VHD slot
 *     and VHD_QueueOutSlot()s (paced by VHD_WaitSlotSent()).
 *
 * Requires the Vulkan QRhi backend + a CUDA driver with VMM support and an
 * RDMA-capable (Quadro/Tesla) GPU. On any failure init() returns false and the
 * node falls back to the host-staged playout path.
 */

#include <deltacast/DeltacastOutputBackend.hpp>

#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/GPUVideoEncoder.hpp>
#include <Gfx/Graph/encoders/WireEncoderFactory.hpp>
#include <Gfx/Graph/interop/CudaFunctions.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/interop/InteropFence.hpp>
#include <Gfx/Graph/interop/RdmaGpuBuffer.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>
#include <Gfx/Graph/interop/VulkanRhiContext.hpp>

#include <score/gfx/Vulkan.hpp>

#include <QtGui/private/qrhivulkan_p.h>

#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <QDebug>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace Gfx::Deltacast
{

/**
 * @brief Vulkan GPU-direct (CUDA array->buffer) playout strategy for Deltacast.
 *
 * Consumes the existing score-plugin-gfx primitives RdmaGpuBuffer (Seam B
 * allocator), CudaP2PBridge (the Vulkan-image import + the per-frame
 * array->buffer copy) and makeWireEncoder. No new GPU primitive is introduced.
 */
struct DeltacastRdmaOutput final : score::gfx::interop::GpuDirectStrategy
{
  DeltacastRdmaOutput(
      DeltacastOutputBackend* backend,
      score::gfx::interop::VideoPixelFormat neutral) noexcept
      : m_backend{backend}
      , m_neutral{neutral}
  {
  }

  // Back-ref to the playout backend that owns the VHD stream: init() registers
  // its RDMA slots there and submitFrame() maps the returned gpuVA -> VHD slot.
  DeltacastOutputBackend* m_backend{};
  score::gfx::interop::VideoPixelFormat m_neutral{};

  score::gfx::interop::GpuDirectStrategyConfig cfg{};

  // Vulkan handles (borrowed from the QRhi backend).
  score::gfx::vkinterop::VulkanCtx m_vk{};
  QVulkanDeviceFunctions* m_devFuncs{};
  VkQueue m_gfxQueue{VK_NULL_HANDLE};
  int m_gfxFamily{-1};

  // CUDA P2P bridge context (owns its primary-context retain + stream) and a
  // private dlopen'd driver-API table for the RdmaGpuBuffer allocations.
  CudaP2PContextHandle m_cudaCtx{};
  score::gfx::CudaFunctions m_cuda{};

  // The RGBA->wire encoder. Its outputTexture() (encTex) is an RGBA8 texture
  // whose bytes are the card's wire format.
  std::unique_ptr<score::gfx::GPUVideoEncoder> m_encoder;
  QRhiTexture* m_encTex{};

  // Exportable VkImage mirroring encTex: CUDA-imported (m_exportArray) + adopted
  // as a QRhiTexture (m_exportTex). The QRhi copyTexture destination of encTex
  // and the CUDA copy source into the RDMA slots.
  score::gfx::vkinterop::ExternalImage m_exportImg{};
  void* m_exportArray{};              // CUarray (copy source)
  CudaP2PImageHandle m_exportHandle{};
  QRhiTexture* m_exportTex{};

  // RDMA-capable GPU VRAM slots: each slot.gpuVA is the CUDA copy destination
  // AND the card's DMA source (VHD_CreateSlotEx RDMAEnabled=TRUE, done by the
  // backend's registerRdmaOutputSlots()).
  static constexpr std::size_t kSlotCount = 4;
  score::gfx::interop::RdmaGpuBuffer m_rdma;
  std::size_t m_idx{0};

  std::uint32_t m_rowBytes{};
  std::uint32_t m_texH{};

  // Vulkan->CUDA ordering fence: QRhi signals a binary VkSemaphore at the
  // endOffscreenFrame queue submit (after the encoder + copyTexture), CUDA
  // waits on it before the array->buffer copy reads m_exportArray. Non-fatal:
  // if init fails, prepareNextFrame() falls back to a full cfg.rhi->finish().
  std::unique_ptr<score::gfx::interop::InteropFence> m_fence;
  bool m_fenceOk{};
  std::uint64_t m_fenceValue{};

  const char* name() const noexcept override { return "Deltacast-RDMA-Vulkan-OUT"; }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    cfg = c;
    if(!m_backend || !cfg.rhi || !cfg.state || !cfg.sourceTexture
       || cfg.rhi->backend() != QRhi::Vulkan)
      return false;
    if(!cuda_p2p_available())
      return false;

    score::gfx::interop::VulkanRhiContext vkc;
    if(!score::gfx::interop::acquireVulkanRhiContext(cfg.rhi, vkc))
      return false;
    m_vk = vkc.vk;
    m_devFuncs = vkc.devFuncs;
    m_gfxQueue = vkc.gfxQueue;
    m_gfxFamily = vkc.gfxFamily;

    // Retain device 0's primary context on this (render) thread; the
    // RdmaGpuBuffer CUDA allocations below run against that current context.
    if(cuda_p2p_init(&m_cudaCtx) != CUDA_P2P_SUCCESS || !m_cudaCtx)
      return false;
    if(!m_cuda.load() || !m_cuda.vmmSupported)
      return releaseFail("CUDA driver / VMM unsupported");

    // 1. The wire encoder. It reads the RGBA scene texture and renders the
    //    card's wire bytes into an RGBA8 texture (encTex). Disable the encoder's
    //    own GPU->CPU readback: this path copies encTex on the GPU instead.
    m_encoder = score::gfx::makeWireEncoder(m_neutral);
    if(!m_encoder)
      return releaseFail("no wire encoder for format");
    m_encoder->init(
        *cfg.rhi, *cfg.state, cfg.sourceTexture, cfg.width, cfg.height,
        m_backend->colorConversion());
    m_encoder->setReadbackEnabled(false);
    m_encTex = m_encoder->outputTexture();
    if(!m_encTex)
      return releaseFail("encoder produced no output texture");

    const QSize encSize = m_encTex->pixelSize();
    m_texH = std::uint32_t(encSize.height());
    m_rowBytes = std::uint32_t(encSize.width()) * 4u;

    // The CUDA copy moves m_rowBytes * m_texH bytes into a slot allocated at
    // cfg.frameByteSize; they must agree or the card would DMA garbage.
    if(m_rowBytes * m_texH != cfg.frameByteSize)
      return releaseFail("encoder output size != frame byte size");

    // 2. Exportable VkImage matching encTex (RGBA8) -> CUDA array -> QRhi tex.
    namespace vki = score::gfx::vkinterop;
    {
      vki::ExternalImageDesc d{};
      d.format = VK_FORMAT_R8G8B8A8_UNORM;
      d.extent = {std::uint32_t(encSize.width()), std::uint32_t(encSize.height()), 1};
      // QRhi copyTexture writes into it (TRANSFER_DST); TRANSFER_SRC + SAMPLED
      // mirror the capture image so QRhi's adopted-texture view creation agrees.
      d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT;
      d.tiling = VK_IMAGE_TILING_OPTIMAL;
      d.handleType = vki::kOpaqueHandleType;
      d.dedicated = true;
      auto img = vki::createExportableImage(m_vk, d);
      if(!img)
        return releaseFail("createExportableImage");
      m_exportImg = *img;

      // The image starts UNDEFINED; transition to GENERAL once so QRhi (told
      // GENERAL via createFrom) and CUDA (which reads the memory) agree.
      if(!score::gfx::vkinterop::transitionImageLayout(
             m_vk, m_gfxQueue, std::uint32_t(m_gfxFamily), m_exportImg.image,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL))
        return releaseFail("image layout transition");

      auto handle = vki::exportMemoryHandle(
          m_vk, m_exportImg.memory, vki::kOpaqueHandleType);
      if(!handle || !handle->isValid())
        return releaseFail("exportMemoryHandle(image)");
      CudaP2PImageDesc cd{
          std::uint32_t(encSize.width()), std::uint32_t(encSize.height()), 1, 4,
          CUDA_P2P_FORMAT_UNSIGNED_INT8, 0};
      if(cuda_p2p_import_vulkan_image(
             m_cudaCtx, handle->osHandle(), m_exportImg.size, &cd, 0,
             &m_exportArray, &m_exportHandle)
             != CUDA_P2P_SUCCESS
         || !m_exportArray)
        return releaseFail("cuda_p2p_import_vulkan_image");

      m_exportTex = cfg.rhi->newTexture(
          QRhiTexture::RGBA8, encSize, 1, QRhiTexture::UsedAsTransferSource);
      QRhiTexture::NativeTexture nt{
          reinterpret_cast<quint64>(m_exportImg.image), VK_IMAGE_LAYOUT_GENERAL};
      if(!m_exportTex->createFrom(nt))
        return releaseFail("QRhiTexture::createFrom");
    }

    // 3. RDMA-capable GPU VRAM slots (Seam B). Each slot.gpuVA is the CUDA copy
    //    destination and the card's DMA source. The CUDA primary context is
    //    current (cuda_p2p_init), so the VMM allocations land on device 0.
    if(!m_rdma.create(
           {score::gfx::interop::RdmaGpuApi::Cuda, std::uint32_t(kSlotCount),
            cfg.frameByteSize, &m_cuda, /*cudaDevice=*/0}))
      return releaseFail("RdmaGpuBuffer(Cuda) allocation");

    // 4. Hand the slot GPU VAs to the backend, which registers them as Deltacast
    //    "application buffers" (RDMAEnabled=TRUE) and starts the stream (the
    //    StartStream deferred from open()). Pass the smallest actual slot
    //    allocation so the backend can validate it against
    //    VHD_GetApplicationBuffersSize. On failure the node falls back to the
    //    host-staged path.
    void* gpuVAs[kSlotCount]{};
    std::size_t slotCapacity = SIZE_MAX;
    for(std::size_t i = 0; i < kSlotCount; ++i)
    {
      gpuVAs[i] = m_rdma.slot(i).gpuVA;
      slotCapacity = std::min(slotCapacity, m_rdma.slot(i).size);
    }
    if(!m_backend->registerRdmaOutputSlots(gpuVAs, kSlotCount, slotCapacity))
      return releaseFail("registerRdmaOutputSlots");

    // 5. Vulkan->CUDA ordering fence (non-fatal). If the binary-semaphore path
    //    can't be set up (extension missing, non-RDMA driver, ...), prepareNextFrame()
    //    falls back to cfg.rhi->finish() — correctness is preserved either way.
    m_fence = score::gfx::interop::makeInteropFence(*cfg.rhi);
    m_fenceOk = m_fence && m_fence->init(*cfg.rhi, m_cudaCtx);
    if(!m_fenceOk)
      qDebug() << "Deltacast RDMA-OUT(Vulkan): InteropFence unavailable — "
                  "using rhi->finish() fallback for Vulkan->CUDA ordering";

    return true;
  }

  void encodeFrame(QRhiCommandBuffer& cb) override
  {
    // RGBA scene -> wire bytes into encTex. exec() ends its own render pass
    // (readback disabled), so we are outside any pass afterwards.
    m_encoder->exec(*cfg.rhi, cb);

    // GPU copy encTex -> the CUDA-imported exportable image (a resource update
    // outside a pass — the established cb.resourceUpdate(batch) pattern).
    auto* rub = cfg.rhi->nextResourceUpdateBatch();
    rub->copyTexture(m_exportTex, m_encTex);
    cb.resourceUpdate(rub);

    // Ask QRhi to signal a binary VkSemaphore at the coming endOffscreenFrame
    // queue submit (after the encoder + copyTexture complete on the GPU). CUDA
    // waits on it in prepareNextFrame() before reading m_exportArray. Runs here,
    // inside the offscreen frame, so setQueueSubmitParams applies to that submit.
    if(m_fenceOk)
      m_fence->signalAfterEncode(cb, ++m_fenceValue);
  }

  void* prepareNextFrame() override
  {
    // endOffscreenFrame() (the node) has flushed the GPU copy above, so the wire
    // bytes are in the exportable image. One synchronous CUDA copy moves them
    // into the next RDMA slot's GPU VRAM (the card's DMA source).
    if(!m_exportArray)
      return nullptr;

    // Ensure the Vulkan encoder + copyTexture have finished before CUDA reads
    // the exportable image. With the fence, schedule a CUDA-stream wait on the
    // semaphore QRhi signalled at submit; without it, a full GPU wait (mirrors
    // the GL glFinish fence) — either way the copy below sees complete data.
    if(m_fenceOk)
      m_fence->waitOnCuda(m_fenceValue);
    else
      cfg.rhi->finish();

    // Skip slots the board is still reading (queued / being sent): with the
    // pump ring + the in-submit frame, up to ringDepth+1 slots can be in
    // flight, and a producer burst must not overwrite one of them mid-DMA
    // (the vendor sample waits WaitSlotSent before rewriting a slot).
    std::size_t idx = m_idx;
    std::size_t tries = 0;
    while(tries < kSlotCount && m_backend->rdmaSlotBusy(m_rdma.slot(idx).gpuVA))
    {
      idx = (idx + 1) % kSlotCount;
      ++tries;
    }
    if(tries == kSlotCount)
      return nullptr; // every slot in flight — drop this frame

    if(cuda_p2p_copy_array_to_buffer(
           m_cudaCtx, m_exportArray, m_rdma.slot(idx).gpuVA, m_rowBytes, m_texH,
           m_rowBytes)
       != CUDA_P2P_SUCCESS)
      return nullptr;
    m_idx = (idx + 1) % kSlotCount;
    return m_rdma.slot(idx).gpuVA;
  }

  void release() override
  {
    // Release the fence (its CUDA semaphores + VkSemaphores) while the CUDA
    // primary context is still alive.
    if(m_fence)
    {
      m_fence->release();
      m_fence.reset();
    }
    m_fenceOk = false;
    m_fenceValue = 0;

    // Destroy the RDMA VMM slots while the CUDA primary context is still
    // alive. The VHD slots (and the driver's DMA pin on this VRAM) were
    // already torn down by the backend's quiesce() -> VHD_StopStream, which
    // the node runs before releasing this strategy.
    m_rdma.destroy();
    if(m_exportHandle && m_cudaCtx)
      cuda_p2p_release_image(m_cudaCtx, m_exportHandle);
    m_exportHandle = {};
    m_exportArray = nullptr;
    delete m_exportTex;
    m_exportTex = nullptr;
    if(m_exportImg.image)
      score::gfx::vkinterop::destroyExternal(m_vk, m_exportImg);
    if(m_encoder)
    {
      m_encoder->release();
      m_encoder.reset();
    }
    m_encTex = nullptr;
    if(m_cudaCtx)
      cuda_p2p_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
  }

private:
  bool releaseFail(const char* what)
  {
    qWarning() << "Deltacast RDMA-OUT(Vulkan):" << what << "failed";
    release();
    return false;
  }
};

} // namespace Gfx::Deltacast
