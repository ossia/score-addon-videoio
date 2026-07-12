#pragma once
#include <AJA/AJAInput.hpp>
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/CpuStagedCapture.hpp>

#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief AJA-specific policy for the shared CpuStagedCapture: AJA SDI ->
 *        page-locked sysmem -> QRhi texture, with the raw-GL fast path.
 *
 * AJA DMAs the captured frame into a page-locked host buffer
 * (DMABufferLock inRDMA=false); the render thread uploads it into the decoder's
 * input texture. On GL it uses a single raw glTexSubImage2D (one driver copy);
 * on Vulkan/Metal/D3D the portable QRhiResourceUpdateBatch::uploadTexture.
 * SCORE_AJA_FORCE_PORTABLE_UPLOAD=1 forces the portable path even on GL.
 *
 * The decoder's input-texture format already matches the AJA byte order
 * (BGRA8 for an ARGB framestore, RGBA8 otherwise), so neither upload path needs
 * a channel swizzle.
 */
struct AjaCpuCapturePolicy
{
  static constexpr bool has_dma_lock = true;
  static constexpr bool has_gl_fast_path = true;
  static constexpr const char* gl_engaged_name = "CPU-GL";
  static constexpr const char* gl_fallback_name = "CPU-QRhi";
  static constexpr const char* log_tag = "AJA CPU-IN:";
  static constexpr const char* force_portable_env
      = "SCORE_AJA_FORCE_PORTABLE_UPLOAD";

  CNTV2Card* card{};
  AJAInputPixelFormat pixelFormat{};

  bool valid() const noexcept { return card != nullptr; }
  bool dmaLock(void* ptr, std::uint32_t bytes) noexcept
  {
    // Page-lock (paged, not RDMA) so AutoCirculate's DMA into the host buffer
    // doesn't re-pin pages every frame.
    return ajaDmaLock(card, ptr, bytes, /*rdma=*/false);
  }
  bool bgra() const noexcept { return pixelFormat == AJAInputPixelFormat::ARGB; }
};

/// Universal CPU-staging AJA capture. Works on every backend.
using CaptureInteropCpu
    = score::gfx::interop::CpuStagedCapture<AjaCpuCapturePolicy>;

} // namespace Gfx::AJA
