#pragma once
#include <AJA/AJAInput.hpp>
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/RdmaRingDepth.hpp>
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

  static constexpr std::size_t kMaxSlots = 3;
  /// 2 at >=32 MB frames: pinned bounces live in the BAR1 aperture,
  /// shared with the output side (see the GL capture strategy).
  std::size_t m_slotCount = kMaxSlots;
  struct Slot
  {
    void* bouncePtr{};              // pinned CUDA bounce (AJA DMA target)
    bool dmaLocked{};

    // Double-buffered renderer-facing image: an exportable LINEAR VkImage whose
    // memory is CUDA-mapped as a flat buffer + QRhi-adopted for sampling.
    // ingestFrame writes bounce → this slot's image; the render thread samples
    // whichever slot was published last, so the capture-thread write and the
    // render-thread sample never touch the same VkImage (the single-image tear
    // this strategy used to have).
    score::gfx::vkinterop::ExternalImage image{};
    void* imgFlatPtr{};             // flat CUDA view of the image memory
    CudaP2PResourceHandle imgRes{};
    std::uint64_t linOffset{};
    std::uint64_t linPitch{};
    QRhiTexture* ownedTex{};
  };
  std::array<Slot, kMaxSlots> m_slots{};
  score::gfx::interop::CaptureSlotPublisher m_publisher;
  /// Slot the render thread most recently took (currentTexture() indexes it).
  int m_renderIdx{0};

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

    m_slotCount = score::gfx::interop::rdmaRingDepthForFrame(
        cfg.frameByteSize, {/*full=*/int(kMaxSlots), /*large=*/2});

    const QSize sz = cfg.outputTexture->pixelSize();
    m_texW = sz.width();
    m_texH = sz.height();
    m_rowBytes = std::uint32_t(m_texW) * 4u;
    const bool bgra = (cfg.outputTexture->format() == QRhiTexture::BGRA8);
    const VkFormat vkfmt = bgra ? VK_FORMAT_B8G8R8A8_UNORM
                                : VK_FORMAT_R8G8B8A8_UNORM;

    // Per slot: (1) an exportable LINEAR VkImage → CUDA flat ptr → QRhi-adopted
    // texture (the double-buffered sampled resource, so capture-write and
    // render-sample never collide on one image), and (2) a pinned CUDA bounce
    // the card P2P-DMAs into. DMABufferLock only accepts CUDA-allocator memory
    // — never CUDA-imported Vulkan memory — so the bounce stays separate; it
    // feeds the per-frame pitched copy into this slot's image.
    for(std::size_t i = 0; i < m_slotCount; ++i)
    {
      auto& s = m_slots[i];
      if(!createSlotImage(s, vkfmt, sz))
        return releaseFail("createSlotImage");

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

  // Build one double-buffer slot's exportable LINEAR VkImage, import it into
  // CUDA as a flat pointer, and adopt it into a QRhiTexture for sampling.
  bool createSlotImage(Slot& s, VkFormat vkfmt, const QSize& sz)
  {
    namespace vki = score::gfx::vkinterop;
    vki::ExternalImageDesc d{};
    d.format = vkfmt;
    d.extent = {std::uint32_t(m_texW), std::uint32_t(m_texH), 1};
    d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // LINEAR: CUDA writes through a flat buffer view with a queried rowPitch —
    // no block-linear/tiling interpretation on either side. (An OPTIMAL image
    // imported as a CUDA array scrambles per-tile on this driver regardless of
    // CUDA_ARRAY3D_COLOR_ATTACHMENT.)
    d.tiling = VK_IMAGE_TILING_LINEAR;
    d.handleType = vki::kOpaqueHandleType;
    d.dedicated = true;
    auto img = vki::createExportableImage(m_vk, d);
    if(!img)
      return false;
    s.image = *img;

    // The image starts UNDEFINED; transition to GENERAL once so QRhi (told
    // GENERAL via createFrom) and CUDA (which writes the memory) agree.
    if(!score::gfx::vkinterop::transitionImageLayout(
           m_vk, m_gfxQueue, std::uint32_t(m_gfxFamily), s.image.image,
           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL))
      return false;

    // Row layout of the linear image, for the pitched CUDA writes.
    {
      VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
      VkSubresourceLayout lay{};
      m_devFuncs->vkGetImageSubresourceLayout(
          m_vk.dev, s.image.image, &sub, &lay);
      s.linOffset = lay.offset;
      s.linPitch = lay.rowPitch;
      if(s.linPitch < m_rowBytes)
        return false;
    }

    auto handle
        = vki::exportMemoryHandle(m_vk, s.image.memory, vki::kOpaqueHandleType);
    if(!handle || !handle->isValid())
      return false;
    if(cuda_p2p_import_vulkan_buffer(
           m_cudaCtx, handle->osHandle(), s.image.size, &s.imgFlatPtr,
           &s.imgRes)
           != CUDA_P2P_SUCCESS
       || !s.imgFlatPtr)
      return false;

    s.ownedTex = cfg.rhi->newTexture(
        cfg.outputTexture->format(), sz, 1, QRhiTexture::UsedAsTransferSource);
    QRhiTexture::NativeTexture nt{
        reinterpret_cast<quint64>(s.image.image), VK_IMAGE_LAYOUT_GENERAL};
    if(!s.ownedTex->createFrom(nt))
      return false;
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

      if(s.imgRes && m_cudaCtx)
        cuda_p2p_release_buffer(m_cudaCtx, s.imgRes);
      s.imgRes = {};
      s.imgFlatPtr = nullptr;
      delete s.ownedTex;
      s.ownedTex = nullptr;
      if(s.image.image)
        score::gfx::vkinterop::destroyExternal(m_vk, s.image);
      s.image = {};
    }
    m_renderIdx = 0;
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
    // Capture thread: AJA P2P-wrote the frame into slot i's bounce VRAM.
    // Copy it into THIS slot's image (not a shared one), then publish i — so
    // the render thread samples a slot no capture write is currently touching.
    if(i >= m_slotCount || !m_slots[i].imgFlatPtr)
      return false;
    auto& s = m_slots[i];
    if(cuda_p2p_copy_dtod_2d(
           m_cudaCtx,
           static_cast<char*>(s.imgFlatPtr) + s.linOffset, s.linPitch,
           s.bouncePtr, m_rowBytes, m_rowBytes, std::uint32_t(m_texH))
       != CUDA_P2P_SUCCESS)
      return false;
    m_publisher.publish(i);
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override
  {
    return m_slots[0].ownedTex ? m_slots[0].ownedTex : cfg.outputTexture;
  }

  // The freshest completed slot's image — the renderer rebinds its passes to
  // this when it changes (double-buffering: never the slot capture is writing).
  QRhiTexture* currentTexture() const noexcept override
  {
    const auto i = static_cast<std::size_t>(m_renderIdx);
    if(i < m_slotCount && m_slots[i].ownedTex)
      return m_slots[i].ownedTex;
    return outputTexture();
  }

  // Consume the published slot so currentTexture() points at the fresh image;
  // the copy already happened on the capture thread.
  void acquireForRender() override
  {
    const int idx = m_publisher.consume();
    if(idx >= 0 && static_cast<std::size_t>(idx) < m_slotCount)
      m_renderIdx = idx;
  }
  void releaseAfterRender() override { }

private:
  bool releaseFail(const char* what)
  {
    qWarning() << "AJA RDMA-IN(Vulkan/T3):" << what << "failed";
    release();
    return false;
  }
};

} // namespace Gfx::AJA
