#include "V4L2Session.hpp"

#include <linux/videodev2.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <array>

namespace Gfx::V4L2
{
namespace
{
int xioctl(int fd, unsigned long req, void* arg) noexcept
{
  int r;
  do
  {
    r = ::ioctl(fd, req, arg);
  } while(r == -1 && errno == EINTR);
  return r;
}

constexpr std::size_t kMaxPlanes = 4;
constexpr std::size_t kMaxSlots = 32;
/// How many consecutive error-flagged buffers to skip before giving up.
constexpr int kMaxErrorSkips = 8;
} // namespace

const char* toString(BufferMode m) noexcept
{
  switch(m)
  {
    case BufferMode::MmapRead:
      return "mmap";
    case BufferMode::MmapExport:
      return "mmap+expbuf";
    case BufferMode::DmaBufImport:
      return "dmabuf-import";
  }
  return "?";
}

BufferMode BufferCaps::preferred() const noexcept
{
  // Without driver-stated capabilities the only safe claim is plain MMAP:
  // ProCapture grants MMAP buffers but has no VIDIOC_EXPBUF at all, so
  // inferring MmapExport from "mmap works" picks a mode that cannot start.
  if(!reportsCaps)
    return BufferMode::MmapRead;
  if(dmabuf)
    return BufferMode::DmaBufImport;
  if(mmap)
    return BufferMode::MmapExport;
  return BufferMode::MmapRead;
}

struct Session::Impl
{
  int fd{-1};
  std::string path, driver, card, busInfo, lastError;

  bool multiplanar{};
  std::uint32_t bufType{V4L2_BUF_TYPE_VIDEO_CAPTURE};

  BufferCaps caps{};
  Format fmt{};

  BufferMode mode{BufferMode::MmapRead};
  bool streaming{};
  std::size_t nSlots{};
  std::array<Slot, kMaxSlots> slots{};
  std::array<bool, kMaxSlots> ownedByDriver{};

  DmaBufAllocator* allocator{};
  std::array<DmaBufAllocator::Buffer, kMaxSlots> external{};

  std::uint64_t errFrames{}, dropFrames{};
  std::uint64_t lastSequence{};
  bool haveSequence{};

  int lastErrno{};

  bool fail(const char* what) noexcept
  {
    lastErrno = errno;
    lastError = std::string(what) + ": " + ::strerror(errno);
    return false;
  }

  bool reqbufs(std::uint32_t count, std::uint32_t memory, std::uint32_t* capsOut)
  {
    v4l2_requestbuffers rb{};
    rb.count = count;
    rb.type = bufType;
    rb.memory = memory;
    if(xioctl(fd, VIDIOC_REQBUFS, &rb) < 0)
      return false;
    if(capsOut)
      *capsOut = rb.capabilities;
    return true;
  }

  /// Fill a v4l2_buffer (+ plane array) for the given slot and direction.
  void prepareBuffer(
      v4l2_buffer& buf, v4l2_plane (&planes)[kMaxPlanes], std::size_t i,
      std::uint32_t memory) noexcept
  {
    buf = {};
    buf.type = bufType;
    buf.memory = memory;
    buf.index = static_cast<std::uint32_t>(i);
    if(multiplanar)
    {
      ::memset(planes, 0, sizeof(planes));
      buf.m.planes = planes;
      buf.length = fmt.planeCount;
    }
  }

  bool queueSlot(std::size_t i)
  {
    v4l2_buffer buf{};
    v4l2_plane planes[kMaxPlanes]{};
    const std::uint32_t mem = (mode == BufferMode::DmaBufImport)
                                  ? V4L2_MEMORY_DMABUF
                                  : V4L2_MEMORY_MMAP;
    prepareBuffer(buf, planes, i, mem);

    if(mode == BufferMode::DmaBufImport)
    {
      // The API allows a different fd per QBUF, but the same fd is passed for
      // the slot's whole lifetime so videobuf2 keeps its cached mapping
      // instead of re-attaching the dma_buf every frame.
      if(multiplanar)
      {
        for(std::uint32_t p = 0; p < fmt.planeCount; ++p)
        {
          planes[p].m.fd = slots[i].dmabufFd[p];
          planes[p].length = fmt.planeSize[p];
        }
      }
      else
      {
        buf.m.fd = slots[i].dmabufFd[0];
        buf.length = fmt.sizeImage;
      }
    }

    if(xioctl(fd, VIDIOC_QBUF, &buf) < 0)
      return fail("VIDIOC_QBUF");
    ownedByDriver[i] = true;
    return true;
  }

  /// Undo everything `start()` did, driver side included. REQBUFS can fail
  /// having already allocated part of the set, and a partial allocation is
  /// only released by REQBUFS(count=0) — without it the next attempt gets
  /// ENOMEM, so one failed rung would take the whole fallback ladder with it.
  void abortStart() noexcept
  {
    unmapAll();
    const std::uint32_t mem = (mode == BufferMode::DmaBufImport) ? V4L2_MEMORY_DMABUF
                                                                 : V4L2_MEMORY_MMAP;
    reqbufs(0, mem, nullptr);
    nSlots = 0;
  }

  void unmapAll() noexcept
  {
    for(std::size_t i = 0; i < nSlots; ++i)
    {
      for(std::size_t p = 0; p < kMaxPlanes; ++p)
      {
        if(slots[i].mapped[p])
        {
          ::munmap(slots[i].mapped[p], slots[i].mappedSize[p]);
          slots[i].mapped[p] = nullptr;
          slots[i].mappedSize[p] = 0;
        }
        if(slots[i].dmabufFd[p] >= 0 && mode != BufferMode::DmaBufImport)
        {
          ::close(slots[i].dmabufFd[p]);
          slots[i].dmabufFd[p] = -1;
        }
      }
    }
    if(mode == BufferMode::DmaBufImport && allocator)
    {
      for(std::size_t i = 0; i < nSlots; ++i)
      {
        allocator->release(external[i]);
        external[i] = {};
        for(auto& f : slots[i].dmabufFd)
          f = -1;
      }
    }
  }
};

Session::Session()
    : d{std::make_unique<Impl>()}
{
}

Session::~Session()
{
  close();
}

bool Session::isOpen() const noexcept
{
  return d->fd >= 0;
}
const std::string& Session::path() const noexcept
{
  return d->path;
}
const std::string& Session::driver() const noexcept
{
  return d->driver;
}
const std::string& Session::card() const noexcept
{
  return d->card;
}
BufferCaps Session::bufferCaps() const noexcept
{
  return d->caps;
}
const Format& Session::format() const noexcept
{
  return d->fmt;
}
BufferMode Session::mode() const noexcept
{
  return d->mode;
}
bool Session::isStreaming() const noexcept
{
  return d->streaming;
}
std::size_t Session::slotCount() const noexcept
{
  return d->nSlots;
}
const Slot& Session::slot(std::size_t i) const noexcept
{
  return d->slots[std::min(i, kMaxSlots - 1)];
}
std::uint64_t Session::errorFrames() const noexcept
{
  return d->errFrames;
}
std::uint64_t Session::droppedFrames() const noexcept
{
  return d->dropFrames;
}
const std::string& Session::lastError() const noexcept
{
  return d->lastError;
}

bool Session::lastErrorUnsupported() const noexcept
{
  return d->lastErrno == ENOTTY;
}

bool Session::open(const std::string& p)
{
  close();
  d->fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if(d->fd < 0)
    return d->fail("open");
  d->path = p;

  v4l2_capability cap{};
  if(xioctl(d->fd, VIDIOC_QUERYCAP, &cap) < 0)
  {
    d->fail("VIDIOC_QUERYCAP");
    close();
    return false;
  }
  d->driver = reinterpret_cast<const char*>(cap.driver);
  d->card = reinterpret_cast<const char*>(cap.card);
  d->busInfo = reinterpret_cast<const char*>(cap.bus_info);

  const auto caps
      = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
  {
    d->multiplanar = true;
    d->bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  }
  else if(caps & V4L2_CAP_VIDEO_CAPTURE)
  {
    d->multiplanar = false;
    d->bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  }
  else
  {
    d->lastError = "not a video capture device";
    close();
    return false;
  }
  if(!(caps & V4L2_CAP_STREAMING))
  {
    d->lastError = "device does not support streaming I/O";
    close();
    return false;
  }

  // Probe the queue's buffer capabilities. REQBUFS(count=0) reports the
  // capability bits without allocating, and is how the supported memory
  // types are discovered rather than inferred from the driver name.
  std::uint32_t bits = 0;
  if(!d->reqbufs(0, V4L2_MEMORY_MMAP, &bits))
  {
    d->fail("VIDIOC_REQBUFS(probe)");
  }
  else if(bits != 0)
  {
    d->caps.probed = true;
    d->caps.reportsCaps = true;
    d->caps.mmap = bits & V4L2_BUF_CAP_SUPPORTS_MMAP;
    d->caps.userptr = bits & V4L2_BUF_CAP_SUPPORTS_USERPTR;
    d->caps.dmabuf = bits & V4L2_BUF_CAP_SUPPORTS_DMABUF;
    d->caps.requests = bits & V4L2_BUF_CAP_SUPPORTS_REQUESTS;
    d->caps.orphanedBufs = bits & V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
  }
  else
  {
    // REQBUFS succeeded but reported nothing. `capabilities` is optional
    // (Linux 4.20+) and a driver that predates it, or an out-of-tree one that
    // never filled it in, leaves it zero -- Magewell's ProCapture does, while
    // REQBUFS(4, MMAP) on the same node happily grants 4 buffers. Treating
    // zero as "no mode supported" made every Magewell node unusable.
    // MMAP is the baseline every streaming-capture driver must implement, so
    // assume just that and let the real REQBUFS decide; the richer modes stay
    // off because there is no evidence for them, and abortStart() already
    // unwinds a rung that turns out not to work.
    d->caps.probed = true;
    d->caps.reportsCaps = false;
    d->caps.mmap = true;
  }

  v4l2_format f{};
  f.type = d->bufType;
  if(xioctl(d->fd, VIDIOC_G_FMT, &f) == 0)
    configure(0, 0, 0);

  return true;
}

void Session::close()
{
  if(!d)
    return;
  stop();
  if(d->fd >= 0)
  {
    ::close(d->fd);
    d->fd = -1;
  }
  d->path.clear();
  d->driver.clear();
  d->card.clear();
}

bool Session::configure(
    std::uint32_t width, std::uint32_t height, std::uint32_t fourcc)
{
  if(d->fd < 0)
    return false;

  v4l2_format f{};
  f.type = d->bufType;
  if(xioctl(d->fd, VIDIOC_G_FMT, &f) < 0)
    return d->fail("VIDIOC_G_FMT");

  if(d->multiplanar)
  {
    if(width)
      f.fmt.pix_mp.width = width;
    if(height)
      f.fmt.pix_mp.height = height;
    if(fourcc)
      f.fmt.pix_mp.pixelformat = fourcc;
  }
  else
  {
    if(width)
      f.fmt.pix.width = width;
    if(height)
      f.fmt.pix.height = height;
    if(fourcc)
      f.fmt.pix.pixelformat = fourcc;
  }

  // S_FMT never fails for an unsupported geometry: the driver substitutes the
  // closest it can do, so the granted format is read back, not assumed.
  if(xioctl(d->fd, VIDIOC_S_FMT, &f) < 0)
    return d->fail("VIDIOC_S_FMT");

  Format out{};
  if(d->multiplanar)
  {
    out.width = f.fmt.pix_mp.width;
    out.height = f.fmt.pix_mp.height;
    out.fourcc = f.fmt.pix_mp.pixelformat;
    out.multiplanar = true;
    out.planeCount = std::min<std::uint32_t>(f.fmt.pix_mp.num_planes, kMaxPlanes);
    for(std::uint32_t i = 0; i < out.planeCount; ++i)
    {
      out.planeStride[i] = f.fmt.pix_mp.plane_fmt[i].bytesperline;
      out.planeSize[i] = f.fmt.pix_mp.plane_fmt[i].sizeimage;
    }
    out.bytesPerLine = out.planeStride[0];
    out.sizeImage = out.planeSize[0];
  }
  else
  {
    out.width = f.fmt.pix.width;
    out.height = f.fmt.pix.height;
    out.fourcc = f.fmt.pix.pixelformat;
    out.planeCount = 1;
    out.bytesPerLine = f.fmt.pix.bytesperline;
    out.sizeImage = f.fmt.pix.sizeimage;
    out.planeStride[0] = out.bytesPerLine;
    out.planeSize[0] = out.sizeImage;
  }
  d->fmt = out;
  return true;
}

bool Session::start(
    BufferMode mode, std::size_t slotCount, DmaBufAllocator* allocator)
{
  if(d->fd < 0 || d->streaming)
    return false;
  slotCount = std::clamp<std::size_t>(slotCount, 2, kMaxSlots);

  if(mode == BufferMode::DmaBufImport)
  {
    if(!allocator)
    {
      d->lastError = "DmaBufImport requires a DmaBufAllocator";
      return false;
    }
    if(!d->caps.dmabuf)
    {
      d->lastError = "driver queue does not support V4L2_MEMORY_DMABUF";
      return false;
    }
  }
  else if(!d->caps.mmap)
  {
    d->lastError = "driver queue does not support V4L2_MEMORY_MMAP";
    return false;
  }

  d->mode = mode;
  d->allocator = allocator;
  d->errFrames = d->dropFrames = 0;
  d->haveSequence = false;

  const std::uint32_t mem = (mode == BufferMode::DmaBufImport) ? V4L2_MEMORY_DMABUF
                                                               : V4L2_MEMORY_MMAP;
  std::uint32_t bits = 0;
  if(!d->reqbufs(static_cast<std::uint32_t>(slotCount), mem, &bits))
  {
    d->fail("VIDIOC_REQBUFS");
    d->abortStart();
    return false;
  }

  // The driver may grant fewer buffers than requested.
  v4l2_requestbuffers probe{};
  probe.count = static_cast<std::uint32_t>(slotCount);
  probe.type = d->bufType;
  probe.memory = mem;
  d->nSlots = slotCount;

  for(std::size_t i = 0; i < d->nSlots; ++i)
  {
    d->slots[i] = {};
    d->slots[i].index = i;
    for(auto& f : d->slots[i].dmabufFd)
      f = -1;
  }

  if(mode == BufferMode::DmaBufImport)
  {
    for(std::size_t i = 0; i < d->nSlots; ++i)
    {
      DmaBufAllocator::Buffer b{};
      if(!allocator->allocate(
             d->fmt.width, d->fmt.height, d->fmt.fourcc, d->fmt.sizeImage, b))
      {
        d->lastError = "DmaBufAllocator::allocate failed";
        d->abortStart();
        return false;
      }
      d->external[i] = b;
      d->slots[i].dmabufFd[0] = b.fd;
      d->slots[i].modifier = b.modifier;
    }
  }
  else
  {
    for(std::size_t i = 0; i < d->nSlots; ++i)
    {
      v4l2_buffer buf{};
      v4l2_plane planes[kMaxPlanes]{};
      d->prepareBuffer(buf, planes, i, V4L2_MEMORY_MMAP);
      if(xioctl(d->fd, VIDIOC_QUERYBUF, &buf) < 0)
      {
        d->fail("VIDIOC_QUERYBUF");
        d->abortStart();
        return false;
      }

      const std::uint32_t np = d->multiplanar ? d->fmt.planeCount : 1u;
      for(std::uint32_t p = 0; p < np; ++p)
      {
        const std::size_t len
            = d->multiplanar ? planes[p].length : buf.length;
        const std::size_t off
            = d->multiplanar ? planes[p].m.mem_offset : buf.m.offset;

        if(mode == BufferMode::MmapRead)
        {
          void* m = ::mmap(
              nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
              static_cast<off_t>(off));
          if(m == MAP_FAILED)
          {
            d->fail("mmap");
            d->abortStart();
            return false;
          }
          d->slots[i].mapped[p] = m;
          d->slots[i].mappedSize[p] = len;
        }
        else
        {
          // VIDIOC_EXPBUF is documented as MMAP-only and one call per plane;
          // the returned fd is ours to close.
          v4l2_exportbuffer eb{};
          eb.type = d->bufType;
          eb.index = static_cast<std::uint32_t>(i);
          eb.plane = p;
          eb.flags = O_CLOEXEC | O_RDONLY;
          if(xioctl(d->fd, VIDIOC_EXPBUF, &eb) < 0)
          {
            d->fail("VIDIOC_EXPBUF");
            d->abortStart();
            return false;
          }
          d->slots[i].dmabufFd[p] = eb.fd;
        }
      }
    }
  }

  for(std::size_t i = 0; i < d->nSlots; ++i)
  {
    if(!d->queueSlot(i))
    {
      d->abortStart();
      return false;
    }
  }

  int type = static_cast<int>(d->bufType);
  if(xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
  {
    d->fail("VIDIOC_STREAMON");
    d->abortStart();
    return false;
  }
  d->streaming = true;
  return true;
}

void Session::stop()
{
  if(!d || d->fd < 0 || !d->streaming)
    return;
  int type = static_cast<int>(d->bufType);
  xioctl(d->fd, VIDIOC_STREAMOFF, &type);
  d->streaming = false;
  d->unmapAll();

  const std::uint32_t mem = (d->mode == BufferMode::DmaBufImport)
                                ? V4L2_MEMORY_DMABUF
                                : V4L2_MEMORY_MMAP;
  d->reqbufs(0, mem, nullptr);
  d->nSlots = 0;
}

int Session::dequeue(int timeoutMs)
{
  if(d->fd < 0 || !d->streaming)
    return -2;

  // A buffer flagged V4L2_BUF_FLAG_ERROR holds no usable payload -- uvcvideo
  // raises it for the incomplete first frame of almost every stream, and for
  // any frame that lost USB packets. Handing it on renders corruption, so it
  // is returned to the driver and the next one is taken. Bounded, because a
  // source producing nothing but errors must surface as a timeout rather than
  // spin here.
  for(int attempt = 0; attempt < kMaxErrorSkips; ++attempt)
  {

  pollfd pfd{};
  pfd.fd = d->fd;
  pfd.events = POLLIN;
  const int pr = ::poll(&pfd, 1, timeoutMs);
  if(pr == 0)
    return -1;
  if(pr < 0)
  {
    if(errno == EINTR)
      return -1;
    d->fail("poll");
    return -2;
  }
  if(pfd.revents & (POLLERR | POLLHUP))
  {
    d->lastError = "stream error (POLLERR/POLLHUP)";
    return -2;
  }

  v4l2_buffer buf{};
  v4l2_plane planes[kMaxPlanes]{};
  const std::uint32_t mem = (d->mode == BufferMode::DmaBufImport)
                                ? V4L2_MEMORY_DMABUF
                                : V4L2_MEMORY_MMAP;
  buf = {};
  buf.type = d->bufType;
  buf.memory = mem;
  if(d->multiplanar)
  {
    buf.m.planes = planes;
    buf.length = d->fmt.planeCount;
  }

  if(xioctl(d->fd, VIDIOC_DQBUF, &buf) < 0)
  {
    if(errno == EAGAIN)
      return -1;
    d->fail("VIDIOC_DQBUF");
    return -2;
  }

  const std::size_t idx = buf.index;
  if(idx >= d->nSlots)
  {
    d->lastError = "DQBUF returned an out-of-range index";
    return -2;
  }
  d->ownedByDriver[idx] = false;

  auto& s = d->slots[idx];
  s.bytesUsed = d->multiplanar ? planes[0].bytesused : buf.bytesused;
  s.sequence = buf.sequence;
  s.timestampNs = static_cast<std::uint64_t>(buf.timestamp.tv_sec) * 1000000000ull
                  + static_cast<std::uint64_t>(buf.timestamp.tv_usec) * 1000ull;

  if(buf.flags & V4L2_BUF_FLAG_ERROR)
  {
    d->errFrames++;
    d->slots[idx].bytesUsed = 0;
    requeue(idx);
    continue;
  }

  // A jump in the driver's sequence counter is the only reliable signal that
  // frames were produced but never reached us.
  if(d->haveSequence && buf.sequence > d->lastSequence + 1)
    d->dropFrames += buf.sequence - d->lastSequence - 1;
  d->lastSequence = buf.sequence;
  d->haveSequence = true;

  return static_cast<int>(idx);

  } // retry loop

  // Every attempt produced an error-flagged buffer.
  d->lastError = "only error-flagged buffers received";
  return -1;
}

bool Session::requeue(std::size_t index)
{
  if(d->fd < 0 || !d->streaming || index >= d->nSlots)
    return false;
  if(d->ownedByDriver[index])
    return true;
  return d->queueSlot(index);
}

std::vector<DeviceInfo> enumerateDevices()
{
  std::vector<DeviceInfo> out;
  DIR* dir = ::opendir("/dev");
  if(!dir)
    return out;

  std::vector<std::string> names;
  while(dirent* e = ::readdir(dir))
  {
    if(::strncmp(e->d_name, "video", 5) == 0)
      names.push_back(std::string("/dev/") + e->d_name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());

  for(const auto& n : names)
  {
    const int fd = ::open(n.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if(fd < 0)
      continue;
    v4l2_capability cap{};
    if(xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
      const auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps
                            : cap.capabilities;
      const bool mp = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
      const bool sp = caps & V4L2_CAP_VIDEO_CAPTURE;
      if((mp || sp) && (caps & V4L2_CAP_STREAMING))
      {
        DeviceInfo di;
        di.path = n;
        di.driver = reinterpret_cast<const char*>(cap.driver);
        di.card = reinterpret_cast<const char*>(cap.card);
        di.busInfo = reinterpret_cast<const char*>(cap.bus_info);
        di.canCapture = true;
        di.multiplanar = mp;
        out.push_back(std::move(di));
      }
    }
    ::close(fd);
  }
  return out;
}

} // namespace Gfx::V4L2
