#include "V4L2DonorAllocator.hpp"

#include <linux/videodev2.h>

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

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
} // namespace

DonorAllocator::DonorAllocator(std::string donorPath)
    : m_path{std::move(donorPath)}
{
}

DonorAllocator::~DonorAllocator()
{
  for(int fd : m_fds)
    if(fd >= 0)
      ::close(fd);
  m_fds.clear();
  if(m_fd >= 0)
  {
    v4l2_requestbuffers rb{};
    rb.count = 0;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    xioctl(m_fd, VIDIOC_REQBUFS, &rb);
    ::close(m_fd);
    m_fd = -1;
  }
}

bool DonorAllocator::init(
    std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
    std::size_t bytes, std::size_t count)
{
  m_fd = ::open(m_path.c_str(), O_RDWR | O_CLOEXEC);
  if(m_fd < 0)
  {
    m_lastError = std::string("open donor: ") + ::strerror(errno);
    return false;
  }

  // Match the consumer's geometry so the donor's sizeimage is at least what
  // the consumer needs; a smaller donor buffer would be rejected downstream.
  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(xioctl(m_fd, VIDIOC_G_FMT, &f) < 0)
  {
    m_lastError = std::string("donor G_FMT: ") + ::strerror(errno);
    return false;
  }
  f.fmt.pix.width = width;
  f.fmt.pix.height = height;
  f.fmt.pix.pixelformat = fourcc;
  if(xioctl(m_fd, VIDIOC_S_FMT, &f) < 0)
  {
    m_lastError = std::string("donor S_FMT: ") + ::strerror(errno);
    return false;
  }
  m_bufSize = f.fmt.pix.sizeimage;
  if(m_bufSize < bytes)
  {
    m_lastError = "donor sizeimage smaller than consumer's";
    return false;
  }

  v4l2_requestbuffers rb{};
  rb.count = static_cast<std::uint32_t>(count);
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
  if(xioctl(m_fd, VIDIOC_REQBUFS, &rb) < 0)
  {
    m_lastError = std::string("donor REQBUFS: ") + ::strerror(errno);
    return false;
  }
  m_count = rb.count;

  for(std::size_t i = 0; i < m_count; ++i)
  {
    v4l2_exportbuffer eb{};
    eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    eb.index = static_cast<std::uint32_t>(i);
    eb.plane = 0;
    eb.flags = O_CLOEXEC | O_RDWR;
    if(xioctl(m_fd, VIDIOC_EXPBUF, &eb) < 0)
    {
      m_lastError = std::string("donor EXPBUF: ") + ::strerror(errno);
      return false;
    }
    m_fds.push_back(eb.fd);
  }
  m_next = 0;
  return true;
}

bool DonorAllocator::allocate(
    std::uint32_t, std::uint32_t, std::uint32_t, std::size_t size, Buffer& out)
{
  out = {};
  if(m_next >= m_fds.size())
  {
    m_lastError = "donor ran out of buffers";
    return false;
  }
  if(m_bufSize < size)
  {
    m_lastError = "donor buffer smaller than requested size";
    return false;
  }
  out.fd = m_fds[m_next++];
  out.modifier = 0;
  out.stride = 0;
  out.offset = 0;
  out.size = m_bufSize;
  return true;
}

void DonorAllocator::release(Buffer& b) noexcept
{
  // The fds stay owned by the allocator for its whole lifetime and are closed
  // in the destructor: the consumer pins one fd per slot, so releasing here
  // would close a descriptor the driver may still hold.
  b = {};
}

} // namespace Gfx::V4L2
