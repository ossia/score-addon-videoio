#pragma once
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/GLCaptureUpload.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/ImportedGpuBufferRing.hpp>
#include <Gfx/Graph/interop/RdmaRingDepth.hpp>
#include <Gfx/Graph/interop/StageProfiler.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>

#include <QDebug>

#include <array>
#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief OpenGL tier-3 capture: AJA SDI → P2P DMA → CUDA bounce buffer →
 *        DtoD copy → GL StorageBuffer → glTexSubImage2D → QRhi GL texture.
 *
 * Symmetric inverse of `RdmaInteropGLTier3` (the OUTPUT path). The
 * `ImportedGpuBufferRing` primitive allocates N CUDA-imported GL StorageBuffers;
 * alongside each slot sits a CUDA-owned (cuMemAlloc) bounce buffer that
 * AJA P2P-DMAs the captured frame into (pinned via
 * `DMABufferLock(inRDMA=true)`). The bounce hop exists because
 * nvidia_p2p_get_pages — the kernel interface behind DMABufferLock — only
 * pins CUDA-allocator memory, never GL-owned memory mapped into CUDA.
 * No sysmem ring at all — the SDI data lands in VRAM directly and stays
 * there (one VRAM->VRAM copy per consumed frame).
 *
 * Capture-thread side: `slotBuffer(i)` returns the bounce GPU pointer,
 * which is what `AutoCirculateTransfer.SetVideoBuffer()` accepts when
 * the buffer was registered with `inRDMA=true` (AJA's AC dispatches on
 * physical-vs-flat pointer based on the DMABufferLock flags). `ingestFrame`
 * just publishes the slot index — the data is already on the GPU.
 *
 * Render-thread side: `acquireForRender()` DtoD-copies the latest slot's
 * bounce buffer into its GL StorageBuffer, binds that as
 * `GL_PIXEL_UNPACK_BUFFER` and issues a single
 * `glTexSubImage2D` into `outputTexture` (whose geometry matches the
 * AJA byte layout — see CaptureInteropConfig docs). The renderer's
 * PackedDecoder then samples that texture and decodes UYVY/v210/RGBA.
 *
 * Threading: capture thread owns `slotBuffer` + `ingestFrame`; render
 * thread owns `acquireForRender`/`releaseAfterRender`. The slot index
 * is handed across via `std::atomic<int>` (lock-free, single-producer
 * single-consumer).
 *
 * Requires: AJA Linux kernel module with RDMA support, NVIDIA driver
 * with CUDA + GPUDirect P2P, libcuda.so.1 loadable at runtime.
 */
struct CaptureInteropGLTier3 final : score::gfx::interop::VideoCaptureStrategy
{
  CaptureInteropGLTier3(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::VideoCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  QOpenGLContext* m_glCtx{};
  CudaP2PContextHandle m_cudaCtx{};
  score::gfx::interop::ImportedGpuBufferRing m_ring;

  static constexpr std::size_t kMaxSlots = 3;
  /// Runtime slot count: 3 normally; 2 at ≥32 MB frames so the pinned
  /// bounces (which occupy the GPU's 256 MiB BAR1 aperture, shared with
  /// the output side's bounce) still fit at UHD2/8K rasters.
  std::size_t m_slotCount = kMaxSlots;
  std::array<bool, kMaxSlots> m_dmaLocked{};
  /// CUDA-owned (cuMemAlloc) DMA targets, parallel to the ring slots —
  /// the pointers AJA's AC actually writes; see class doc.
  std::array<void*, kMaxSlots> m_bounce{};

  // Single-producer single-consumer slot handoff. Capture thread stores
  // the latest filled slot; render thread does an acquire-exchange to
  // pull and consume. Shared with CaptureInteropCpu via GLCaptureUpload.hpp.
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  // Option (a): register the decoder's RGBA8 input texture with CUDA and
  // cuMemcpy2D the bounce straight into its level-0 array — collapsing the
  // two render-thread VRAM copies (DtoD→SSBO + glTexSubImage2D upload) into a
  // single DtoD→texture-array. Non-null when engaged; when the register fails
  // at init we leave it null and fall back to the SSBO-ring (m_ring) path.
  CudaP2PResourceHandle m_texRes{};
  bool m_imageInterop{false};
  int m_texW{}, m_texH{};
  std::uint32_t m_rowBytes{};

  // Per-stage render-thread copy timing (SCORE_AJA_PROFILE=1).
  score::gfx::interop::StageProfiler m_profImage{"aja-gl-t3 copy(image)"};
  score::gfx::interop::StageProfiler m_profBuffer{"aja-gl-t3 copy(buf+pbo)"};

  const char* name() const noexcept override { return "RDMA-GL/T3"; }

  bool init(const score::gfx::interop::VideoCaptureStrategyConfig& c) override
  {
    cfg = c;
    m_slotCount = score::gfx::interop::rdmaRingDepthForFrame(
        cfg.frameByteSize, {/*full=*/int(kMaxSlots), /*large=*/2});
    if(!cfg.rhi || !m_card || !cfg.outputTexture)
      return false;
    if(!cuda_p2p_available())
    {
      qDebug() << "AJA RDMA-IN(GL/T3): GPUDirect RDMA not available";
      return false;
    }

    auto* native
        = static_cast<const QRhiGles2NativeHandles*>(cfg.rhi->nativeHandles());
    if(!native || !native->context)
      return false;
    m_glCtx = native->context;

    if(cuda_p2p_init(&m_cudaCtx) != CUDA_P2P_SUCCESS || !m_cudaCtx)
      return false;

    // Pinning a GPU buffer is necessary but NOT sufficient: on cross-host-bridge
    // topologies the card→GPU P2P write is silently dropped even though every
    // call reports success. Probe an actual transfer before committing so we
    // fall back to CPU staging instead of publishing all-zero frames.
    if(!ajaCaptureRdmaDelivers(m_card, m_cudaCtx, cfg.frameByteSize))
    {
      release();
      return false;
    }

    // Validate the output texture geometry matches the AJA frame size
    // up front so we don't DMA into a too-small/too-large buffer.
    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "AJA RDMA-IN(GL/T3):"))
    {
      release();
      return false;
    }

    // CUDA-owned bounces are the AJA P2P-DMA targets in BOTH paths (option (a)
    // image-interop and the SSBO+PBO fallback both DMA into these first —
    // inRDMA=true only pins CUDA-allocator memory, never GL-owned memory).
    for(std::size_t i = 0; i < m_slotCount; ++i)
    {
      if(cuda_p2p_alloc_buffer(m_cudaCtx, cfg.frameByteSize, &m_bounce[i])
             != CUDA_P2P_SUCCESS
         || !m_bounce[i])
      {
        qWarning() << "AJA RDMA-IN(GL/T3): bounce alloc slot" << i << "failed:"
                   << cuda_p2p_get_error_string(m_cudaCtx);
        release();
        return false;
      }
      if(!ajaDmaLock(m_card, m_bounce[i], cfg.frameByteSize, /*rdma=*/true))
      {
        qWarning() << "AJA RDMA-IN(GL/T3): DMABufferLock(GPU) slot" << i
                   << "failed";
        release();
        return false;
      }
      m_dmaLocked[i] = true;
    }

    // Row geometry for the pitched bounce → texture-array blit. The bounce is
    // tightly packed (frameByteSize == width*4*height, validated above), so
    // src pitch == row width == texW*4.
    {
      const QSize sz = cfg.outputTexture->pixelSize();
      m_texW = sz.width();
      m_texH = sz.height();
      m_rowBytes = std::uint32_t(m_texW) * 4u;
    }

    // Option (a): register the decoder's RGBA8 input texture with CUDA. On
    // success we memcpy2D bounce → its array on the render thread — one copy,
    // no SSBO staging, no glTexSubImage2D. If the register fails (driver lacks
    // the symbols, or the QRhi texture isn't CUDA-registrable) we fall through
    // to the proven SSBO-ring + glTexSubImage2D path — no regression.
    // SCORE_AJA_GL_NO_IMAGE_INTEROP=1 forces the SSBO+PBO fallback (for A/B
    // profiling and as an escape hatch if a driver mishandles the image path).
    const bool tryImage
        = !qEnvironmentVariableIsSet("SCORE_AJA_GL_NO_IMAGE_INTEROP");
    if(const auto nt = cfg.outputTexture->nativeTexture();
       tryImage && nt.object)
    {
      const auto glTex = static_cast<std::uint32_t>(nt.object);
      if(cuda_p2p_register_gl_image(m_cudaCtx, glTex, GL_TEXTURE_2D, &m_texRes)
             == CUDA_P2P_SUCCESS
         && m_texRes)
      {
        m_imageInterop = true;
        qDebug() << "AJA RDMA-IN(GL/T3): engaged image-interop path "
                    "(cuGraphicsGLRegisterImage) — single render-thread copy";
      }
      else
      {
        qDebug() << "AJA RDMA-IN(GL/T3): image-interop register failed ("
                 << cuda_p2p_get_error_string(m_cudaCtx)
                 << "); falling back to SSBO+PBO path";
      }
    }

    if(!m_imageInterop)
    {
      score::gfx::interop::ImportedGpuBufferRingConfig rcfg{
          cfg.rhi, m_cudaCtx, cfg.frameByteSize,
          static_cast<int>(m_slotCount), "AJA-RDMA-GL-Capture",
          /*glRegisterOnly=*/true};
      if(!m_ring.create(rcfg))
      {
        qWarning() << "AJA RDMA-IN(GL/T3): ImportedGpuBufferRing::create failed";
        release();
        return false;
      }
    }
    return true;
  }

  void release() override
  {
    for(std::size_t i = 0; i < m_slotCount; ++i)
    {
      if(m_dmaLocked[i] && m_bounce[i])
        ajaDmaUnlock(m_card, m_bounce[i], cfg.frameByteSize);
      m_dmaLocked[i] = false;
      if(m_bounce[i] && m_cudaCtx)
        cuda_p2p_free_buffer(m_cudaCtx, m_bounce[i]);
      m_bounce[i] = nullptr;
    }
    if(m_texRes && m_cudaCtx)
      cuda_p2p_release_buffer(m_cudaCtx, m_texRes);
    m_texRes = nullptr;
    m_imageInterop = false;
    m_ring.destroy();
    if(m_cudaCtx)
      cuda_p2p_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
    m_glCtx = nullptr;
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override
  {
    // With image-interop there is no SSBO ring; the bounce ring is the depth.
    if(m_imageInterop)
      return m_slotCount;
    return m_ring.valid() ? m_ring.slotCount() : 0;
  }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    // Per AJA AC semantics: when the buffer was DMABufferLock'd with
    // inRDMA=true, the pointer passed to AutoCirculateTransfer is the
    // GPU device pointer, NOT a sysmem buffer. AJA's AC dispatches on
    // the lock flag internally.
    return (i < m_slotCount) ? m_bounce[i] : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    // No copy needed on the capture thread: AJA P2P-wrote straight into
    // GPU VRAM. Publish the slot to the renderer.
    if(i >= slotCount())
      return false;
    m_publisher.publish(i);
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override
  {
    return cfg.outputTexture;
  }

  void acquireForRender() override
  {
    if(!m_glCtx)
      return;
    const int slotIdx = m_publisher.consume();
    if(slotIdx < 0 || static_cast<std::size_t>(slotIdx) >= m_slotCount)
      return;
    void* bounce = m_bounce[static_cast<std::size_t>(slotIdx)];
    if(!bounce)
      return;

    if(m_imageInterop)
    {
      // Option (a): ONE bounce → decoder-texture-array VRAM copy. cuMemcpy2D
      // into the registered texture's level-0 array, map/unmap per frame (the
      // unmap makes the CUDA write visible to the subsequent GL sample — the
      // same coherency discipline the buffer path relied on). No SSBO staging,
      // no glTexSubImage2D upload.
      score::gfx::interop::StageProfiler::Scope prof{m_profImage};
      if(cuda_p2p_gl_write_image(
             m_cudaCtx, m_texRes, bounce, m_rowBytes, std::uint32_t(m_texH),
             m_rowBytes)
         != CUDA_P2P_SUCCESS)
      {
        qWarning() << "AJA RDMA-IN(GL/T3): image write failed:"
                   << cuda_p2p_get_error_string(m_cudaCtx);
      }
      return;
    }

    // ---- Fallback: SSBO ring + glTexSubImage2D (two render-thread copies) ----
    score::gfx::interop::StageProfiler::Scope prof{m_profBuffer};
    if(static_cast<std::size_t>(slotIdx) >= m_ring.slotCount())
      return;

    // Extract the slot's native GL buffer name (the P2P DMA target). The
    // storage buffer can be bound to GL_PIXEL_UNPACK_BUFFER even though it was
    // allocated for GL_SHADER_STORAGE_BUFFER — both are server-side memory and
    // the binding target picks the usage.
    //
    // KNOWN LIMITATION (review 2026-07): CudaP2PBridge maps the registered GL
    // buffer once and keeps it mapped for its lifetime, and CUDA specifies
    // that accessing a registered resource through GL *while mapped* is
    // undefined. Fixing it properly means per-access map/unmap in the bridge
    // (mapped pointers are only valid per map cycle, so every cached slot
    // gpuVA in ImportedGpuBufferRing/consumers must be refreshed each frame) — a
    // redesign to do together with the first Linux GL bring-up, not blindly.
    // In practice NVIDIA keeps the mapping coherent here, but do not ship the
    // Linux GL tier-3 path without revisiting this.
    auto& slot = m_ring.slot(static_cast<std::size_t>(slotIdx));
    if(!slot.qrhiBuffer || !slot.cudaHandle)
      return;

    // Bridge the AJA-DMA'd bounce buffer into the slot's GL storage buffer.
    // gl_write_buffer maps the GL buffer, copies (VRAM->VRAM), and unmaps —
    // the unmap flushes the CUDA write so the glTexSubImage2D below reads
    // coherent data. A plain copy into a permanently-mapped buffer is NOT
    // seen by GL (the copy lands but GL samples stale memory).
    if(!m_bounce[static_cast<std::size_t>(slotIdx)]
       || cuda_p2p_gl_write_buffer(
              m_cudaCtx, slot.cudaHandle,
              m_bounce[static_cast<std::size_t>(slotIdx)], cfg.frameByteSize)
              != CUDA_P2P_SUCCESS)
    {
      qWarning() << "AJA RDMA-IN(GL/T3): bounce copy failed:"
                 << cuda_p2p_get_error_string(m_cudaCtx);
      return;
    }

    auto nb = slot.qrhiBuffer->nativeBuffer();
    if(nb.slotCount <= 0 || !nb.objects[0])
      return;
    const std::uint32_t glBuf
        = *static_cast<const std::uint32_t*>(nb.objects[0]);

    // Texture layout per CaptureInteropConfig contract: matches AJA byte
    // layout; sampled as RGBA8 / BGRA8 with the PackedDecoder shader unpacking
    // UYVY / v210 / RGBA in the fragment stage.
    score::gfx::interop::uploadGLBufferToGLTexture(
        *m_glCtx, *cfg.outputTexture, glBuf,
        /*bgra=*/m_pixelFormat == AJAInputPixelFormat::ARGB);
  }

  void releaseAfterRender() override
  {
    // No texture mapping to release; the glTexSubImage2D copy already
    // committed the data to the texture's GL storage.
  }
};

} // namespace Gfx::AJA
