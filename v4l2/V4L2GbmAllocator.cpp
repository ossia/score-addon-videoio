#include "V4L2GbmAllocator.hpp"

#include <Gfx/Graph/interop/EglDmaBufExport.hpp>

#include <linux/videodev2.h>

#include <unistd.h>

#include <string>

namespace Gfx::V4L2
{
namespace
{
constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
{
  return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8)
         | (std::uint32_t(std::uint8_t(c)) << 16)
         | (std::uint32_t(std::uint8_t(d)) << 24);
}
} // namespace

std::uint32_t drmFourccFromV4L2(std::uint32_t v) noexcept
{
  // V4L2 and DRM chose the same 4CC spelling for these layouts, but that is a
  // coincidence of history rather than a rule, so the equivalence is asserted
  // per format instead of passing the code through unchanged.
  switch(v)
  {
    case V4L2_PIX_FMT_UYVY:
      return fourcc('U', 'Y', 'V', 'Y');
    case V4L2_PIX_FMT_YUYV:
      return fourcc('Y', 'U', 'Y', 'V');
    case V4L2_PIX_FMT_YVYU:
      return fourcc('Y', 'V', 'Y', 'U');
    case V4L2_PIX_FMT_VYUY:
      return fourcc('V', 'Y', 'U', 'Y');
    case V4L2_PIX_FMT_NV12:
      return fourcc('N', 'V', '1', '2');
    case V4L2_PIX_FMT_NV21:
      return fourcc('N', 'V', '2', '1');
    case V4L2_PIX_FMT_NV16:
      return fourcc('N', 'V', '1', '6');
    case V4L2_PIX_FMT_YUV420:
      return fourcc('Y', 'U', '1', '2');
    case V4L2_PIX_FMT_YVU420:
      return fourcc('Y', 'V', '1', '2');

    // Packed 32-bit: V4L2 names components in memory order, DRM names them
    // from the MSB of the packed little-endian word, so the two spellings are
    // reversed with respect to each other.
    case V4L2_PIX_FMT_ABGR32: // V4L2 "AR24": B,G,R,A in memory
      return fourcc('A', 'R', '2', '4'); // DRM_FORMAT_ARGB8888, same bytes
    case V4L2_PIX_FMT_XBGR32: // V4L2 "XR24"
      return fourcc('X', 'R', '2', '4'); // DRM_FORMAT_XRGB8888
    case V4L2_PIX_FMT_RGBA32: // V4L2 "AB24": R,G,B,A in memory
      return fourcc('A', 'B', '2', '4'); // DRM_FORMAT_ABGR8888
    case V4L2_PIX_FMT_RGBX32:
      return fourcc('X', 'B', '2', '4'); // DRM_FORMAT_XBGR8888
    default:
      return 0;
  }
}

struct GbmAllocator::Impl
{
  score::gfx::GbmDmaBufExport gbm;
  std::string lastError;
  bool ready{};
  std::unordered_map<int, score::gfx::GbmDmaBufExport::Slot> byFd;
};

GbmAllocator::GbmAllocator()
    : d{new Impl}
{
}

GbmAllocator::~GbmAllocator()
{
  if(d)
  {
    for(auto& [fd, slot] : d->byFd)
    {
      auto s = slot;
      d->gbm.destroySlotGbmOnly(s);
    }
    d->byFd.clear();
    if(d->ready)
      d->gbm.shutdown();
    delete d;
    d = nullptr;
  }
}

bool GbmAllocator::init()
{
  if(!d)
    return false;
  d->ready = d->gbm.init();
  return d->ready;
}

bool GbmAllocator::allocate(
    std::uint32_t width, std::uint32_t height, std::uint32_t v4l2Fourcc,
    std::size_t size, Buffer& out)
{
  out = {};
  if(!d || !d->ready)
    return false;

  const std::uint32_t drm = drmFourccFromV4L2(v4l2Fourcc);
  if(drm == 0)
    return false;

  score::gfx::GbmDmaBufExport::Slot slot{};
  if(!d->gbm.allocSlotGbmOnly(slot, width, height, drm))
  {
    d->lastError = "gbm rejected fourcc";
    return false;
  }

  // The kernel writes sizeImage bytes into this buffer; a BO smaller than
  // that would be a heap overflow performed by the driver, so a short
  // allocation has to fail the rung rather than be clamped.
  if(slot.size < size)
  {
    // gbm_bo_get_stride() reports the first plane only, so stride*height
    // under-counts every multi-planar layout (NV12 needs 1.5x). Allocating
    // anyway would have the kernel DMA past the end of the BO.
    d->lastError = "BO smaller than V4L2 sizeImage (multi-plane layout?)";
    d->gbm.destroySlotGbmOnly(slot);
    return false;
  }

  out.fd = slot.fd;
  out.modifier = slot.modifier;
  out.stride = slot.stride;
  out.offset = slot.offset;
  out.size = slot.size;
  d->byFd.emplace(slot.fd, slot);
  return true;
}

const char* GbmAllocator::lastError() const noexcept
{
  return d && !d->lastError.empty() ? d->lastError.c_str() : "";
}

void GbmAllocator::release(Buffer& b) noexcept
{
  if(!d || b.fd < 0)
    return;
  if(auto it = d->byFd.find(b.fd); it != d->byFd.end())
  {
    auto slot = it->second;
    d->byFd.erase(it);
    d->gbm.destroySlotGbmOnly(slot);
  }
  b = {};
}

} // namespace Gfx::V4L2
