#pragma once

/**
 * @file V4L2DonorAllocator.hpp
 * @brief Test-only DmaBufAllocator that sources buffers from a second V4L2 node.
 *
 * The `DmaBufImport` ingress mode is the one NVIDIA's V4L2 camera path requires
 * on Tegra, but it cannot be exercised end-to-end on a desktop NVIDIA box: GBM
 * allocates fine, and then vb2 rejects the buffer at QBUF because the
 * proprietary driver does not let a foreign device attach its exported
 * dma_bufs. That is a platform property, not a defect in the ingress code.
 *
 * To validate the code path regardless, this allocator hands out buffers
 * exported (VIDIOC_EXPBUF) from a *donor* V4L2 capture node. Those dma_bufs are
 * ordinary videobuf2 allocations that any V4L2 driver can import, so everything
 * specific to the mode — per-slot fd pinning, `m.fd` plumbing, the
 * dequeue/requeue ownership handoff — gets real coverage. What it does not
 * cover is GPU-allocated memory, which needs the Jetson.
 */

#include <v4l2/V4L2Session.hpp>

#include <string>
#include <vector>

namespace Gfx::V4L2
{

class DonorAllocator final : public DmaBufAllocator
{
public:
  explicit DonorAllocator(std::string donorPath);
  ~DonorAllocator() override;

  /// Opens the donor and sizes its buffers to at least `bytes`, matching the
  /// consumer's negotiated format so the exported buffers are large enough.
  bool init(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
            std::size_t bytes, std::size_t count);

  const char* name() const noexcept override { return "donor-v4l2"; }

  bool allocate(
      std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
      std::size_t size, Buffer& out) override;

  void release(Buffer& b) noexcept override;

  const std::string& lastError() const noexcept { return m_lastError; }

private:
  std::string m_path, m_lastError;
  int m_fd{-1};
  std::size_t m_next{};
  std::size_t m_count{};
  std::size_t m_bufSize{};
  std::vector<int> m_fds;
};

} // namespace Gfx::V4L2
