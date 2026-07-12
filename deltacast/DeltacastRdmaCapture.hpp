#pragma once
/**
 * @file DeltacastRdmaCapture.hpp
 * @brief DELTACAST RDMA GPU-direct capture (Vulkan QRhi + CUDA).
 *
 * The true GPU-direct counterpart to DeltacastCpuCapture: the Deltacast card
 * DMAs each received frame straight into RDMA-capable GPU VRAM (a CUDA VMM
 * allocation, BAR1-mappable / gpuDirectRDMACapable), then a CUDA copy moves it
 * into the decoder's output texture — no host transit.
 *
 * This is the symmetric inverse of AJA/AjaRdmaCaptureVulkan for the
 * Deltacast "application buffers" RDMA path. The ONLY structural differences
 * from that template are:
 *   (a) the per-slot RDMA buffer comes from score::gfx::interop::RdmaGpuBuffer
 *       (RdmaGpuApi::Cuda) instead of an exportable VkBuffer + AJA DMABufferLock
 *       pin — the card-side pinning is done by Deltacast's VHD_CreateSlotEx
 *       (RDMAEnabled=TRUE) in DeltacastInputBackend, which takes
 *       slotBuffer(i) == m_rdma.slot(i).gpuVA verbatim;
 *   (b) there is no vendor card object here — the VHD stream is owned by
 *       DeltacastInputBackend, which drives the queue/wait loop.
 *
 *   init (render thread):
 *     - allocate an exportable VkImage matching the decoder's output texture,
 *       import it into CUDA as a CUarray, adopt it into QRhi via createFrom —
 *       this IS the texture the decoder samples (the node swaps it in).
 *     - allocate kSlotCount RDMA GPU buffers via RdmaGpuBuffer(Cuda); each
 *       slot's gpuVA is BOTH the card's DMA target AND the CUDA copy source.
 *
 *   ingestFrame (capture thread): cuda_interop_copy_buffer_to_array copies the slot
 *     buffer -> the image's CUarray (stream-synced), then publishes the slot.
 *
 *   acquireForRender (render thread): nothing to upload — the image already
 *     holds the latest frame (the CUDA copy landed in the QRhi texture's
 *     backing memory on the capture thread).
 *
 * Requires the Vulkan QRhi backend + a CUDA driver with VMM support and an
 * RDMA-capable (Quadro/Tesla) GPU. On any failure init() returns false and the
 * node falls back to DeltacastCpuCapture (host-staged).
 */

#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/CudaFunctions.hpp>
#include <Gfx/Graph/interop/CudaInterop.h>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/RdmaGpuBuffer.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>
#include <Gfx/Graph/interop/VulkanRhiContext.hpp>

#include <score/gfx/Vulkan.hpp>

#include <QtGui/private/qrhivulkan_p.h>

#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <QDebug>

#include <cstdint>

namespace Gfx::Deltacast
{

/**
 * @brief Vulkan zero-copy capture for Deltacast (CUDA buffer->image copy).
 *
 * Consumes the existing score-plugin-gfx primitives RdmaGpuBuffer
 * allocator + CudaInterop (the Vulkan-image import and the per-frame
 * buffer->array copy). No new GPU primitive is introduced.
 */
struct DeltacastRdmaCapture final : score::gfx::interop::VideoCaptureStrategy
{
  score::gfx::interop::VideoCaptureStrategyConfig cfg{};

  // Vulkan handles (borrowed from the QRhi backend).
  score::gfx::vkinterop::VulkanCtx m_vk{};
  QVulkanDeviceFunctions* m_devFuncs{};
  VkQueue m_gfxQueue{VK_NULL_HANDLE};
  int m_gfxFamily{-1};

  // CUDA interop context (owns its primary-context retain + stream) and a
  // private dlopen'd driver-API table for the RdmaGpuBuffer allocations.
  CudaInteropContextHandle m_cudaCtx{};
  score::gfx::CudaFunctions m_cuda{};

  // Renderer-facing texture: an exportable VkImage, CUDA-mapped + QRhi-adopted.
  score::gfx::vkinterop::ExternalImage m_image{};
  void* m_imgArray{};               // CUarray (copy destination)
  CudaInteropImageHandle m_imgHandle{};
  QRhiTexture* m_ownedTex{};

  // RDMA-capable GPU VRAM slots: each slot.gpuVA is the card's DMA target AND
  // the CUDA copy source. The card-side RDMA pin is done by VHD_CreateSlotEx
  // (RDMAEnabled=TRUE) in DeltacastInputBackend, which reads slotBuffer(i).
  static constexpr std::size_t kSlotCount = 3;
  score::gfx::interop::RdmaGpuBuffer m_rdma;
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  int m_texW{}, m_texH{};
  std::uint32_t m_rowBytes{};

  const char* name() const noexcept override { return "Deltacast-RDMA-Vulkan"; }

  bool init(const score::gfx::interop::VideoCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !cfg.outputTexture || cfg.rhi->backend() != QRhi::Vulkan)
      return false;
    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "Deltacast RDMA-IN(Vulkan):"))
      return false;
    if(!cuda_interop_available())
      return false;

    score::gfx::interop::VulkanRhiContext vkc;
    if(!score::gfx::interop::acquireVulkanRhiContext(cfg.rhi, vkc))
      return false;
    m_vk = vkc.vk;
    m_devFuncs = vkc.devFuncs;
    m_gfxQueue = vkc.gfxQueue;
    m_gfxFamily = vkc.gfxFamily;

    // The bridge retains device 0's primary context and makes it current on
    // this (render) thread; the RdmaGpuBuffer CUDA allocations below run
    // against that current context.
    if(cuda_interop_init(&m_cudaCtx) != CUDA_INTEROP_SUCCESS || !m_cudaCtx)
      return false;
    if(!m_cuda.load() || !m_cuda.vmmSupported)
      return releaseFail("CUDA driver / VMM unsupported");

    const QSize sz = cfg.outputTexture->pixelSize();
    m_texW = sz.width();
    m_texH = sz.height();
    m_rowBytes = std::uint32_t(m_texW) * 4u;
    const bool bgra = (cfg.outputTexture->format() == QRhiTexture::BGRA8);
    const VkFormat vkfmt = bgra ? VK_FORMAT_B8G8R8A8_UNORM
                                : VK_FORMAT_R8G8B8A8_UNORM;

    // 1. Exportable VkImage -> CUDA array -> QRhi-adopted texture.
    namespace vki = score::gfx::vkinterop;
    {
      vki::ExternalImageDesc d{};
      d.format = vkfmt;
      d.extent = {std::uint32_t(m_texW), std::uint32_t(m_texH), 1};
      d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      d.tiling = VK_IMAGE_TILING_OPTIMAL;
      d.handleType = vki::kOpaqueHandleType;
      d.dedicated = true;
      auto img = vki::createExportableImage(m_vk, d);
      if(!img)
        return releaseFail("createExportableImage");
      m_image = *img;

      // The image starts UNDEFINED; transition to GENERAL once so QRhi (told
      // GENERAL via createFrom) and CUDA (which writes the memory) agree.
      if(!score::gfx::vkinterop::transitionImageLayout(
             m_vk, m_gfxQueue, std::uint32_t(m_gfxFamily), m_image.image,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL))
        return releaseFail("image layout transition");

      auto handle
          = vki::exportMemoryHandle(m_vk, m_image.memory, vki::kOpaqueHandleType);
      if(!handle || !handle->isValid())
        return releaseFail("exportMemoryHandle(image)");
      CudaInteropImageDesc cd{std::uint32_t(m_texW), std::uint32_t(m_texH), 1, 4,
                          CUDA_INTEROP_FORMAT_UNSIGNED_INT8, 0};
      if(cuda_interop_import_vulkan_image(
             m_cudaCtx, handle->osHandle(),
             m_image.size, &cd, 0, &m_imgArray, &m_imgHandle)
             != CUDA_INTEROP_SUCCESS
         || !m_imgArray)
        return releaseFail("cuda_interop_import_vulkan_image");

      m_ownedTex = cfg.rhi->newTexture(
          cfg.outputTexture->format(), sz, 1, QRhiTexture::UsedAsTransferSource);
      QRhiTexture::NativeTexture nt{
          reinterpret_cast<quint64>(m_image.image), VK_IMAGE_LAYOUT_GENERAL};
      if(!m_ownedTex->createFrom(nt))
        return releaseFail("QRhiTexture::createFrom");
    }

    // 2. RDMA-capable GPU VRAM slots. Each slot.gpuVA is the card's
    //    DMA target (VHD_CreateSlotEx RDMAEnabled=TRUE) and the CUDA copy
    //    source. The CUDA primary context is current (cuda_interop_init), so the
    //    VMM allocations land on device 0.
    if(!m_rdma.create(
           {score::gfx::interop::RdmaGpuApi::Cuda, std::uint32_t(kSlotCount),
            cfg.frameByteSize, &m_cuda, /*cudaDevice=*/0}))
      return releaseFail("RdmaGpuBuffer(Cuda) allocation");

    return true;
  }

  void release() override
  {
    // Destroy the RDMA VMM slots while the CUDA primary context is still alive.
    m_rdma.destroy();
    if(m_imgHandle && m_cudaCtx)
      cuda_interop_release_image(m_cudaCtx, m_imgHandle);
    m_imgHandle = {};
    m_imgArray = nullptr;
    delete m_ownedTex;
    m_ownedTex = nullptr;
    if(m_image.image)
      score::gfx::vkinterop::destroyExternal(m_vk, m_image);
    if(m_cudaCtx)
      cuda_interop_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override { return kSlotCount; }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    return i < kSlotCount ? m_rdma.slot(i).gpuVA : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    // Capture thread: the card RDMA-wrote the frame into slot i's GPU VRAM.
    // Copy it into the renderer texture's CUDA array, then publish.
    // No InteropFence needed on capture: cuda_interop_copy_buffer_to_array is
    // stream-synchronized on return (cuStreamSynchronize), so the CUarray is
    // fully written before publish() hands it to the render thread.
    if(i >= kSlotCount || !m_imgArray)
      return false;
    if(cuda_interop_copy_buffer_to_array(
           m_cudaCtx, m_rdma.slot(i).gpuVA, m_imgArray, m_rowBytes,
           std::uint32_t(m_texH), m_rowBytes)
       != CUDA_INTEROP_SUCCESS)
      return false;
    m_publisher.publish(i);
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override
  {
    return m_ownedTex ? m_ownedTex : cfg.outputTexture;
  }

  // The image already holds the latest frame (CUDA copy done on capture
  // thread); nothing to do on the render thread.
  void acquireForRender() override { m_publisher.consume(); }
  void releaseAfterRender() override { }

private:
  bool releaseFail(const char* what)
  {
    qWarning() << "Deltacast RDMA-IN(Vulkan):" << what << "failed";
    release();
    return false;
  }
};

} // namespace Gfx::Deltacast
