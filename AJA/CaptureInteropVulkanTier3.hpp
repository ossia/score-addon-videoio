#pragma once
#include <AJA/AJAInput.hpp>
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <score/gfx/Vulkan.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhivulkan_p.h>

#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <QDebug>

#include <array>
#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief Vulkan tier-3 zero-copy capture (Design B: CUDA buffer->image copy).
 *
 * The symmetric inverse of CaptureInteropGLTier3 for the Vulkan backend. Unlike
 * GL — where the AJA-DMA'd buffer is uploaded into the decoder's texture with a
 * raw glTexSubImage2D on the render thread — Vulkan has no raw upload call to
 * hand inside acquireForRender(), so the per-frame copy is done on the CAPTURE
 * thread via CUDA, straight into the renderer texture's memory:
 *
 *   init (render thread):
 *     - allocate an exportable VkImage matching the decoder's input texture,
 *       import it into CUDA as a CUarray, and adopt it into QRhi via
 *       QRhiTexture::createFrom — this IS the texture the decoder samples
 *       (AJAInputNode swaps it in for its decoder's sampler).
 *     - per slot: allocate an exportable VkBuffer, import into CUDA -> flat GPU
 *       pointer, AJA DMABufferLock(inRDMA=true) so AutoCirculate P2P-DMAs the
 *       captured frame straight into that VRAM.
 *
 *   ingestFrame (capture thread): cuda_p2p_copy_buffer_to_array copies the slot
 *     buffer -> the image's CUarray (stream-synced), then publishes the slot.
 *
 *   acquireForRender (render thread): nothing to upload — the image already
 *     holds the latest frame. (A VkCuda timeline semaphore would be the robust
 *     cross-API sync; the capture-thread streamSync is the pragmatic baseline.)
 *
 * Requires nvidia-peermem (AJA GPUDirect RDMA pin); on hosts without it the
 * DMABufferLock(inRDMA=true) fails and AJAInputNode falls back to CPU staging.
 * The Vulkan<->CUDA machinery (export + image-map + copy) is validated by
 * AJARoundtrip --vk-interop-probe on consumer GPUs.
 */
struct CaptureInteropVulkanTier3 final : score::gfx::interop::GpuDirectCaptureStrategy
{
  CaptureInteropVulkanTier3(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  score::gfx::vkinterop::VulkanCtx m_vk{};
  QVulkanDeviceFunctions* m_devFuncs{};
  VkQueue m_gfxQueue{VK_NULL_HANDLE};
  int m_gfxFamily{-1};

  CudaP2PContextHandle m_cudaCtx{};

  // Renderer-facing texture: an exportable LINEAR VkImage whose memory is
  // CUDA-mapped as a flat buffer + QRhi-adopted for sampling.
  score::gfx::vkinterop::ExternalImage m_image{};
  void* m_imgFlatPtr{};             // flat CUDA view of the image memory
  CudaP2PResourceHandle m_imgRes{};
  std::uint64_t m_linOffset{};
  std::uint64_t m_linPitch{};
  QRhiTexture* m_ownedTex{};

  static constexpr std::size_t kMaxSlots = 3;
  /// 2 at >=32 MB frames: pinned bounces live in the BAR1 aperture,
  /// shared with the output side (see the GL capture strategy).
  std::size_t m_slotCount = kMaxSlots;
  struct Slot
  {
    void* bouncePtr{};              // pinned CUDA bounce (AJA DMA target)
    bool dmaLocked{};
  };
  std::array<Slot, kMaxSlots> m_slots{};
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  int m_texW{}, m_texH{};
  std::uint32_t m_rowBytes{};

  const char* name() const noexcept override { return "RDMA-Vulkan/T3"; }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !m_card || !cfg.outputTexture
       || cfg.rhi->backend() != QRhi::Vulkan)
      return false;
    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "AJA RDMA-IN(Vulkan/T3):"))
      return false;
    if(!cuda_p2p_available())
      return false;

    auto* h = static_cast<const QRhiVulkanNativeHandles*>(cfg.rhi->nativeHandles());
    QVulkanInstance* qInst = score::gfx::staticVulkanInstance(false);
    if(!h || !h->dev || !h->physDev || !qInst)
      return false;
    m_vk = {qInst->vkInstance(), h->physDev, h->dev, qInst};
    m_devFuncs = qInst->deviceFunctions(h->dev);
    m_gfxQueue = h->gfxQueue;
    m_gfxFamily = h->gfxQueueFamilyIdx;
    if(!m_devFuncs || !m_gfxQueue || m_gfxFamily < 0)
      return false;

    if(cuda_p2p_init(&m_cudaCtx) != CUDA_P2P_SUCCESS || !m_cudaCtx)
      return false;

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
      // LINEAR: CUDA writes through a flat buffer view with a queried
      // rowPitch — no block-linear/tiling interpretation on either side.
      // (An OPTIMAL image imported as a CUDA array scrambles per-tile on
      // this driver regardless of CUDA_ARRAY3D_COLOR_ATTACHMENT.)
      d.tiling = VK_IMAGE_TILING_LINEAR;
      d.handleType = vki::kOpaqueHandleType;
      d.dedicated = true;
      auto img = vki::createExportableImage(m_vk, d);
      if(!img)
        return releaseFail("createExportableImage");
      m_image = *img;

      // The image starts UNDEFINED; transition to GENERAL once so QRhi (told
      // GENERAL via createFrom) and CUDA (which writes the memory) agree.
      if(!transitionImageToGeneral())
        return releaseFail("image layout transition");

      // Row layout of the linear image, for the pitched CUDA writes.
      {
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout lay{};
        m_devFuncs->vkGetImageSubresourceLayout(
            m_vk.dev, m_image.image, &sub, &lay);
        m_linOffset = lay.offset;
        m_linPitch = lay.rowPitch;
        if(m_linPitch < m_rowBytes)
          return releaseFail("linear image pitch");
      }

      auto handle
          = vki::exportMemoryHandle(m_vk, m_image.memory, vki::kOpaqueHandleType);
      if(!handle || !handle->isValid())
        return releaseFail("exportMemoryHandle(image)");
      if(cuda_p2p_import_vulkan_buffer(
             m_cudaCtx, handle->osHandle(), m_image.size, &m_imgFlatPtr,
             &m_imgRes)
             != CUDA_P2P_SUCCESS
         || !m_imgFlatPtr)
        return releaseFail("cuda_p2p_import_vulkan_buffer(image mem)");

      m_ownedTex = cfg.rhi->newTexture(
          cfg.outputTexture->format(), sz, 1, QRhiTexture::UsedAsTransferSource);
      QRhiTexture::NativeTexture nt{
          reinterpret_cast<quint64>(m_image.image), VK_IMAGE_LAYOUT_GENERAL};
      if(!m_ownedTex->createFrom(nt))
        return releaseFail("QRhiTexture::createFrom");
    }

    // 2. Per-slot pinned CUDA bounce. DMABufferLock only accepts
    //    CUDA-allocator memory — never CUDA-imported Vulkan memory (same
    //    lesson as the GL ring). The bounce feeds copy_buffer_to_array
    //    directly, so no intermediate Vulkan buffer is needed at all.
    m_slotCount = cfg.frameByteSize >= (32u << 20) ? 2 : kMaxSlots;
    for(std::size_t i = 0; i < m_slotCount; ++i)
    {
      auto& s = m_slots[i];
      if(cuda_p2p_alloc_buffer(m_cudaCtx, cfg.frameByteSize, &s.bouncePtr)
             != CUDA_P2P_SUCCESS
         || !s.bouncePtr)
        return releaseFail("bounce alloc");
      if(!ajaDmaLock(m_card, s.bouncePtr, cfg.frameByteSize, /*rdma=*/true))
        return releaseFail("DMABufferLock(inRDMA=true)");
      s.dmaLocked = true;
    }
    // Content-verified delivery probe — pin + return codes lie on some
    // PCIe topologies (same probe the GL capture path runs).
    if(!ajaCaptureRdmaDelivers(m_card, m_cudaCtx, cfg.frameByteSize))
      return releaseFail("delivery probe (card->GPU writes dropped)");
    return true;
  }

  void release() override
  {
    for(auto& s : m_slots)
    {
      if(s.dmaLocked)
        ajaDmaUnlock(m_card, s.bouncePtr, cfg.frameByteSize);
      s.dmaLocked = false;
      if(s.bouncePtr && m_cudaCtx)
        cuda_p2p_free_buffer(m_cudaCtx, s.bouncePtr);
      s.bouncePtr = nullptr;
    }
    if(m_imgRes && m_cudaCtx)
      cuda_p2p_release_buffer(m_cudaCtx, m_imgRes);
    m_imgRes = {};
    m_imgFlatPtr = nullptr;
    delete m_ownedTex;
    m_ownedTex = nullptr;
    if(m_image.image)
      score::gfx::vkinterop::destroyExternal(m_vk, m_image);
    if(m_cudaCtx)
      cuda_p2p_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override { return m_slotCount; }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    return i < m_slotCount ? m_slots[i].bouncePtr : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    // Capture thread: AJA P2P-wrote the frame into slot i's VkBuffer VRAM.
    // Copy it into the renderer texture's CUDA array, then publish.
    if(i >= m_slotCount || !m_imgFlatPtr)
      return false;
    if(cuda_p2p_copy_dtod_2d(
           m_cudaCtx,
           static_cast<char*>(m_imgFlatPtr) + m_linOffset, m_linPitch,
           m_slots[i].bouncePtr, m_rowBytes, m_rowBytes,
           std::uint32_t(m_texH))
       != CUDA_P2P_SUCCESS)
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
    qWarning() << "AJA RDMA-IN(Vulkan/T3):" << what << "failed";
    release();
    return false;
  }

  // One-time UNDEFINED -> GENERAL transition via a transient command buffer,
  // so the QRhi-adopted image is genuinely in the layout createFrom claims.
  bool transitionImageToGeneral()
  {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = std::uint32_t(m_gfxFamily);
    if(m_devFuncs->vkCreateCommandPool(m_vk.dev, &pci, nullptr, &pool)
       != VK_SUCCESS)
      return false;

    VkCommandBuffer cb{VK_NULL_HANDLE};
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    bool ok = m_devFuncs->vkAllocateCommandBuffers(m_vk.dev, &ai, &cb)
              == VK_SUCCESS;
    if(ok)
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      m_devFuncs->vkBeginCommandBuffer(cb, &bi);

      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = m_image.image;
      b.subresourceRange
          = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      m_devFuncs->vkCmdPipelineBarrier(
          cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
          &b);
      m_devFuncs->vkEndCommandBuffer(cb);

      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      ok = m_devFuncs->vkQueueSubmit(m_gfxQueue, 1, &si, VK_NULL_HANDLE)
           == VK_SUCCESS;
      if(ok)
        m_devFuncs->vkQueueWaitIdle(m_gfxQueue);
      m_devFuncs->vkFreeCommandBuffers(m_vk.dev, pool, 1, &cb);
    }
    m_devFuncs->vkDestroyCommandPool(m_vk.dev, pool, nullptr);
    return ok;
  }
};

} // namespace Gfx::AJA
