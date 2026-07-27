#pragma once

/**
 * @file V4L2NvBufAllocator.hpp
 * @brief DmaBufAllocator backed by NvBufSurface, for V4L2 DmaBufImport on Tegra.
 *
 * NVIDIA's V4L2 camera path takes the importer role only — nvv4l2camerasrc
 * streams with V4L2_MEMORY_DMABUF — so on Jetson the buffers must come from
 * us, in a layout the ISP and the display engine both accept. NvBufSurface is
 * the allocator that produces those: `NvBufSurfaceParams::bufferDesc` is a
 * DMA-BUF fd, which is exactly what `V4L2Session` needs and exactly what the
 * existing `DMABufPlaneImporter` consumes on the GPU side.
 *
 * Why not NvSciBuf, which is where the JetPack 7 camera stack is heading:
 * NvSciBuf imports into **Vulkan SC**, via VK_NV_external_memory_sci_buf and
 * VK_NV_external_sci_sync2. Vulkan SC is a separate safety-critical driver,
 * not the standard Vulkan that Qt's RHI (and therefore score) runs on, so
 * there is no NvSciBuf -> standard-Vulkan import to write. The supported
 * bridge is NvSciBuf <-> NvBufSurface, and NvBufSurface already hands out a
 * DMA-BUF fd — so a SIPL frame reaches score through this class plus the
 * dmabuf importer we already have, with no NvSci code in score at all.
 *
 * The NvBufSurface entry points are dlopen'd, and the struct definitions come
 * from the real `nvbufsurface.h` rather than being mirrored here: guessing at
 * a vendor struct layout would produce silent memory corruption rather than a
 * link error. Where the header is absent the class compiles to an
 * always-unavailable stub, so the rung degrades on non-Tegra instead of
 * failing the build.
 */

#include <v4l2/V4L2Session.hpp>

#include <cstdint>
#include <string>

namespace Gfx::V4L2
{

/// True when built against the Tegra multimedia headers AND the runtime
/// library is present. Everywhere else this is false and the rung is skipped.
bool nvBufSurfaceAvailable() noexcept;

class NvBufAllocator final : public DmaBufAllocator
{
public:
  NvBufAllocator();
  ~NvBufAllocator() override;

  /// dlopens libnvbufsurface. False when unavailable, so the caller degrades
  /// to an MMAP rung rather than failing.
  bool init();

  const char* name() const noexcept override { return "nvbufsurface"; }

  bool allocate(
      std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
      std::size_t size, Buffer& out) override;

  void release(Buffer& b) noexcept override;

  const std::string& lastError() const noexcept;

private:
  struct Impl;
  Impl* d{};
};

} // namespace Gfx::V4L2
