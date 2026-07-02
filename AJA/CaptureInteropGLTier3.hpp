#pragma once
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/GLCaptureUpload.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuRingBuffer.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>

#include <QDebug>

#include <array>
#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief OpenGL tier-3 capture: AJA SDI → P2P DMA → GL StorageBuffer →
 *        glTexSubImage2D → QRhi GL texture.
 *
 * Symmetric inverse of `RdmaInteropGLTier3` (the OUTPUT path). The same
 * `GpuRingBuffer` primitive allocates N CUDA-imported GL StorageBuffers;
 * AJA P2P-DMAs the captured frame into each slot's flat GPU pointer
 * (pinned via `DMABufferLock(inRDMA=true)`). No sysmem ring at all —
 * the SDI data lands in VRAM directly, sharing the same address space
 * as the renderer.
 *
 * Capture-thread side: `slotBuffer(i)` returns the GPU device pointer,
 * which is what `AutoCirculateTransfer.SetVideoBuffer()` accepts when
 * the buffer was registered with `inRDMA=true` (AJA's AC dispatches on
 * physical-vs-flat pointer based on the DMABufferLock flags). `ingestFrame`
 * just publishes the slot index — the data is already on the GPU.
 *
 * Render-thread side: `acquireForRender()` binds the latest slot's GL
 * buffer as `GL_PIXEL_UNPACK_BUFFER` and issues a single
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
struct CaptureInteropGLTier3 final : score::gfx::interop::GpuDirectCaptureStrategy
{
  CaptureInteropGLTier3(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  QOpenGLContext* m_glCtx{};
  CudaP2PContextHandle m_cudaCtx{};
  score::gfx::interop::GpuRingBuffer m_ring;

  static constexpr std::size_t kSlotCount = 3;
  std::array<bool, kSlotCount> m_dmaLocked{};

  // Single-producer single-consumer slot handoff. Capture thread stores
  // the latest filled slot; render thread does an acquire-exchange to
  // pull and consume. Shared with CaptureInteropCpu via GLCaptureUpload.hpp.
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  const char* name() const noexcept override { return "RDMA-GL/T3"; }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
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

    // Validate the output texture geometry matches the AJA frame size
    // up front so we don't DMA into a too-small/too-large buffer.
    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "AJA RDMA-IN(GL/T3):"))
    {
      release();
      return false;
    }

    score::gfx::interop::GpuRingBufferConfig rcfg{
        cfg.rhi, m_cudaCtx, cfg.frameByteSize,
        static_cast<int>(kSlotCount), "AJA-RDMA-GL-Capture"};
    if(!m_ring.create(rcfg))
    {
      qWarning() << "AJA RDMA-IN(GL/T3): GpuRingBuffer::create failed";
      release();
      return false;
    }

    for(std::size_t i = 0; i < kSlotCount; ++i)
    {
      // GPUDirect P2P: lock the GPU device pointer (inRDMA=true) so AJA's AC
      // DMAs the captured frame straight into VRAM.
      if(!ajaDmaLock(
             m_card, m_ring.slot(i).gpuDevicePtr, cfg.frameByteSize,
             /*rdma=*/true))
      {
        qWarning() << "AJA RDMA-IN(GL/T3): DMABufferLock(GPU) slot" << i
                   << "failed";
        release();
        return false;
      }
      m_dmaLocked[i] = true;
    }
    return true;
  }

  void release() override
  {
    for(std::size_t i = 0; i < kSlotCount; ++i)
    {
      if(m_dmaLocked[i] && m_ring.slotCount() > i)
        ajaDmaUnlock(m_card, m_ring.slot(i).gpuDevicePtr, cfg.frameByteSize);
      m_dmaLocked[i] = false;
    }
    m_ring.destroy();
    if(m_cudaCtx)
      cuda_p2p_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
    m_glCtx = nullptr;
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override
  {
    return m_ring.valid() ? m_ring.slotCount() : 0;
  }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    // Per AJA AC semantics: when the buffer was DMABufferLock'd with
    // inRDMA=true, the pointer passed to AutoCirculateTransfer is the
    // GPU device pointer, NOT a sysmem buffer. AJA's AC dispatches on
    // the lock flag internally.
    return (i < m_ring.slotCount()) ? m_ring.slot(i).gpuDevicePtr : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    // No copy needed on the capture thread: AJA P2P-wrote straight into
    // GPU VRAM. Publish the slot to the renderer.
    if(i >= m_ring.slotCount())
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
    if(slotIdx < 0 || static_cast<std::size_t>(slotIdx) >= m_ring.slotCount())
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
    // gpuVA in GpuRingBuffer/consumers must be refreshed each frame) — a
    // redesign to do together with the first Linux GL bring-up, not blindly.
    // In practice NVIDIA keeps the mapping coherent here, but do not ship the
    // Linux GL tier-3 path without revisiting this.
    auto& slot = m_ring.slot(static_cast<std::size_t>(slotIdx));
    if(!slot.qrhiBuffer)
      return;
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
