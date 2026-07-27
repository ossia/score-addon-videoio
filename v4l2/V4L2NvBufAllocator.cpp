#include "V4L2NvBufAllocator.hpp"

#include <linux/videodev2.h>

// The Tegra multimedia headers are only present on a Jetson install. Their
// struct layouts are ABI, so they are included rather than mirrored: a
// hand-copied vendor struct that drifts produces silent corruption, not a
// diagnostic.
#if defined(__aarch64__) && __has_include(<nvbufsurface.h>)
#define SCORE_HAS_NVBUFSURFACE 1
#include <nvbufsurface.h>
#else
#define SCORE_HAS_NVBUFSURFACE 0
#endif

#if SCORE_HAS_NVBUFSURFACE
#include <ossia/detail/dylib_loader.hpp>

#include <optional>
#include <unordered_map>
#endif

namespace Gfx::V4L2
{

bool nvBufSurfaceAvailable() noexcept
{
#if SCORE_HAS_NVBUFSURFACE
  try
  {
    ossia::dylib_loader probe(std::vector<std::string_view>{
        "libnvbufsurface.so", "libnvbufsurface.so.1.0.0"});
    return probe.symbol<void*>("NvBufSurfaceCreate") != nullptr;
  }
  catch(...)
  {
    return false;
  }
#else
  return false;
#endif
}

#if SCORE_HAS_NVBUFSURFACE

namespace
{
/// V4L2 pixel format -> NvBufSurface colour format. Only the layouts a Tegra
/// camera actually delivers are mapped; anything else fails the rung rather
/// than allocating a differently-shaped buffer.
NvBufSurfaceColorFormat colorFormatFromV4L2(std::uint32_t v) noexcept
{
  switch(v)
  {
    case V4L2_PIX_FMT_UYVY:
      return NVBUF_COLOR_FORMAT_UYVY;
    case V4L2_PIX_FMT_YUYV:
      return NVBUF_COLOR_FORMAT_YUYV;
    case V4L2_PIX_FMT_NV12:
      return NVBUF_COLOR_FORMAT_NV12;
    case V4L2_PIX_FMT_NV16:
      return NVBUF_COLOR_FORMAT_NV16;
    case V4L2_PIX_FMT_ABGR32:
      return NVBUF_COLOR_FORMAT_BGRA;
    case V4L2_PIX_FMT_RGBA32:
      return NVBUF_COLOR_FORMAT_RGBA;
    default:
      return NVBUF_COLOR_FORMAT_INVALID;
  }
}
} // namespace

struct NvBufAllocator::Impl
{
  using FN_Create = int (*)(NvBufSurface**, std::uint32_t, NvBufSurfaceCreateParams*);
  using FN_Destroy = int (*)(NvBufSurface*);

  std::optional<ossia::dylib_loader> lib;
  FN_Create create{};
  FN_Destroy destroy{};
  std::string lastError;
  std::unordered_map<int, NvBufSurface*> byFd;
};

NvBufAllocator::NvBufAllocator()
    : d{new Impl}
{
}

NvBufAllocator::~NvBufAllocator()
{
  if(d)
  {
    for(auto& [fd, surf] : d->byFd)
      if(surf && d->destroy)
        d->destroy(surf);
    d->byFd.clear();
    delete d;
    d = nullptr;
  }
}

bool NvBufAllocator::init()
{
  if(!d)
    return false;
  try
  {
    d->lib.emplace(std::vector<std::string_view>{
        "libnvbufsurface.so", "libnvbufsurface.so.1.0.0"});
  }
  catch(...)
  {
    d->lastError = "libnvbufsurface not found";
    return false;
  }
  d->create = d->lib->symbol<Impl::FN_Create>("NvBufSurfaceCreate");
  d->destroy = d->lib->symbol<Impl::FN_Destroy>("NvBufSurfaceDestroy");
  if(!d->create || !d->destroy)
  {
    d->lastError = "NvBufSurfaceCreate/Destroy missing";
    d->lib.reset();
    return false;
  }
  return true;
}

bool NvBufAllocator::allocate(
    std::uint32_t width, std::uint32_t height, std::uint32_t v4l2Fourcc,
    std::size_t size, Buffer& out)
{
  out = {};
  if(!d || !d->create)
    return false;

  const auto colour = colorFormatFromV4L2(v4l2Fourcc);
  if(colour == NVBUF_COLOR_FORMAT_INVALID)
  {
    d->lastError = "no NvBufSurface colour format for this V4L2 fourcc";
    return false;
  }

  NvBufSurfaceCreateParams params{};
  params.gpuId = 0;
  params.width = width;
  params.height = height;
  params.colorFormat = colour;
  params.layout = NVBUF_LAYOUT_PITCH;
  // SURFACE_ARRAY is the memory type whose bufferDesc carries a DMA-BUF fd;
  // the other types leave it unset, which would silently hand V4L2 an
  // invalid descriptor.
  params.memType = NVBUF_MEM_SURFACE_ARRAY;

  NvBufSurface* surf = nullptr;
  if(d->create(&surf, 1, &params) != 0 || !surf || surf->numFilled < 1)
  {
    d->lastError = "NvBufSurfaceCreate failed";
    return false;
  }

  const auto& p = surf->surfaceList[0];
  const int fd = static_cast<int>(p.bufferDesc);
  if(fd < 0)
  {
    d->destroy(surf);
    d->lastError = "NvBufSurface produced no DMA-BUF fd";
    return false;
  }
  if(p.dataSize < size)
  {
    d->destroy(surf);
    d->lastError = "NvBufSurface smaller than the V4L2 sizeImage";
    return false;
  }

  out.fd = fd;
  out.stride = p.pitch;
  out.offset = 0;
  out.size = p.dataSize;
  out.modifier = 0;
  d->byFd.emplace(fd, surf);
  return true;
}

void NvBufAllocator::release(Buffer& b) noexcept
{
  if(!d || b.fd < 0)
    return;
  if(auto it = d->byFd.find(b.fd); it != d->byFd.end())
  {
    if(it->second && d->destroy)
      d->destroy(it->second);
    d->byFd.erase(it);
  }
  b = {};
}

const std::string& NvBufAllocator::lastError() const noexcept
{
  static const std::string empty;
  return d ? d->lastError : empty;
}

#else // !SCORE_HAS_NVBUFSURFACE

struct NvBufAllocator::Impl
{
  std::string lastError{"built without the Tegra multimedia headers"};
};

NvBufAllocator::NvBufAllocator()
    : d{new Impl}
{
}
NvBufAllocator::~NvBufAllocator()
{
  delete d;
  d = nullptr;
}
bool NvBufAllocator::init()
{
  return false;
}
bool NvBufAllocator::allocate(
    std::uint32_t, std::uint32_t, std::uint32_t, std::size_t, Buffer& out)
{
  out = {};
  return false;
}
void NvBufAllocator::release(Buffer& b) noexcept
{
  b = {};
}
const std::string& NvBufAllocator::lastError() const noexcept
{
  static const std::string empty;
  return d ? d->lastError : empty;
}

#endif

} // namespace Gfx::V4L2
