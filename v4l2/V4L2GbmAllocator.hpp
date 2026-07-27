#pragma once

/**
 * @file V4L2GbmAllocator.hpp
 * @brief DmaBufAllocator backed by GBM, for V4L2 `DmaBufImport` on desktop.
 *
 * Wraps the GBM surface already used by the PipeWire DMA-BUF output path
 * (`score::gfx::GbmDmaBufExport`), whose `allocSlotGbmOnly()` allocates a BO
 * and exports its fd without touching EGL or GL — which is exactly the shape
 * V4L2 needs, since the buffer is handed to the kernel rather than rendered
 * into locally. Reusing it also inherits its recorded NVIDIA quirk handling
 * (nvidia-drm rejects GBM_BO_USE_RENDERING and gbm_bo_create_with_modifiers2,
 * but allocates with USE_LINEAR alone).
 *
 * On Tegra this class is replaced, not extended: NVIDIA's V4L2 camera path
 * expects NvBufSurface-allocated buffers, which is the other implementation of
 * the same `DmaBufAllocator` interface.
 */

#include <v4l2/V4L2Session.hpp>

#include <score_addon_videoio_export.h>

#include <cstdint>
#include <unordered_map>

namespace score::gfx
{
struct GbmDmaBufExport;
}

namespace Gfx::V4L2
{

/// Maps a V4L2 pixel format to the DRM fourcc naming the same memory layout.
/// Returns 0 when there is no equivalent, which must abort the DmaBufImport
/// rung rather than silently allocate a differently-shaped buffer.
SCORE_ADDON_VIDEOIO_EXPORT std::uint32_t
drmFourccFromV4L2(std::uint32_t v4l2Fourcc) noexcept;

class SCORE_ADDON_VIDEOIO_EXPORT GbmAllocator final : public DmaBufAllocator
{
public:
  GbmAllocator();
  ~GbmAllocator() override;

  /// dlopens libgbm and opens a DRM render node. False when GBM is
  /// unavailable, so the caller degrades to an MMAP rung.
  bool init();

  const char* name() const noexcept override { return "gbm"; }

  bool allocate(
      std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
      std::size_t size, Buffer& out) override;

  void release(Buffer& b) noexcept override;

  /// Why the last allocate() failed, for honest rung reporting.
  const char* lastError() const noexcept;

private:
  struct Impl;
  Impl* d{};
};

} // namespace Gfx::V4L2
