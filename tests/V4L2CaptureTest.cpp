// V4L2 capture harness: exercises every ingress mode a device advertises and
// reports what actually engaged, per the house convention that a result is only
// meaningful if the row says which path ran.
//
//   V4L2CaptureTest                       sweep every capture node, every mode
//   V4L2CaptureTest --device /dev/video0  one node
//   V4L2CaptureTest --mode mmap+expbuf    pin one mode (fails if unavailable)
//   V4L2CaptureTest --size 3840x2160 --fourcc UYVY
//   V4L2CaptureTest --frames 120 --list
//
// Exit code is nonzero if any pinned mode failed to engage or any cell FAILed.

#include "V4L2DonorAllocator.hpp"

#include <v4l2/V4L2GbmAllocator.hpp>
#include <v4l2/V4L2Session.hpp>

#include <linux/dma-buf.h>
#include <linux/videodev2.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Gfx::V4L2;

namespace
{
struct Args
{
  std::string device;
  std::string mode;
  std::string fourcc;
  int width{}, height{};
  int frames{60};
  int slots{4};
  bool list{};
  std::string donor;
};

std::string fourccStr(std::uint32_t f)
{
  char b[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff),
               char((f >> 24) & 0xff), 0};
  for(int i = 0; i < 4; ++i)
    if(b[i] < 32 || b[i] > 126)
      b[i] = '?';
  return b;
}

std::uint32_t parseFourcc(const std::string& s)
{
  if(s.size() != 4)
    return 0;
  return std::uint32_t(std::uint8_t(s[0])) | (std::uint32_t(std::uint8_t(s[1])) << 8)
         | (std::uint32_t(std::uint8_t(s[2])) << 16)
         | (std::uint32_t(std::uint8_t(s[3])) << 24);
}

/// Cheap content check: a frame that is entirely one byte value is either a
/// blanked buffer or a buffer the driver never wrote. Distinguishes "capture
/// ran" from "capture returned zeroed memory", which a frame count alone does
/// not.
struct ContentStats
{
  bool uniform{true};
  std::uint8_t first{};
  std::size_t nonZero{};
};

ContentStats inspect(const std::uint8_t* p, std::size_t n)
{
  ContentStats s;
  if(!p || n == 0)
    return s;
  s.first = p[0];
  const std::size_t step = n > 4096 ? n / 4096 : 1;
  for(std::size_t i = 0; i < n; i += step)
  {
    if(p[i] != s.first)
      s.uniform = false;
    if(p[i] != 0)
      s.nonZero++;
  }
  return s;
}

/// Reads back a DMA-BUF the kernel filled. dma_buf mmap needs the SYNC ioctl
/// bracket so the CPU view is coherent with the device write; without it a
/// cached exporter can hand back stale bytes and the check would be a lie.
ContentStats inspectDmaBuf(int fd, std::size_t bytes)
{
  ContentStats st;
  if(fd < 0 || bytes == 0)
    return st;
  void* p = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if(p == MAP_FAILED)
    return st;

  dma_buf_sync sync{};
  sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
  ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
  st = inspect(static_cast<const std::uint8_t*>(p), bytes);
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

  ::munmap(p, bytes);
  return st;
}

struct Result
{
  std::string device, mode;
  bool ran{}, pass{};
  std::string verdict;
  int frames{};
  double fps{};
  std::uint64_t dropped{}, errors{};
  bool contentOk{};
};

Result runCell(
    const DeviceInfo& dev, BufferMode mode, const Args& a, bool modeWasPinned)
{
  Result r;
  r.device = dev.path;
  r.mode = toString(mode);

  Session s;
  if(!s.open(dev.path))
  {
    r.verdict = "FAIL(open): " + s.lastError();
    return r;
  }

  const auto caps = s.bufferCaps();
  if(!caps.probed)
  {
    // EBUSY here means another process is streaming, which is a different
    // fact from the driver lacking the mode: reporting it as unsupported
    // would send someone hunting a driver bug that is not there.
    r.verdict = "FAIL(device-busy): " + s.lastError();
    return r;
  }
  const bool supported = (mode == BufferMode::DmaBufImport) ? caps.dmabuf : caps.mmap;
  if(!supported)
  {
    r.verdict = modeWasPinned ? "FAIL(mode-not-supported)" : "SKIP(mode-unavailable)";
    return r;
  }

  if(a.width || a.height || !a.fourcc.empty())
  {
    if(!s.configure(a.width, a.height, parseFourcc(a.fourcc)))
    {
      r.verdict = "FAIL(configure): " + s.lastError();
      return r;
    }
  }
  const auto fmt = s.format();

  GbmAllocator gbm;
  DonorAllocator donor{a.donor};
  DmaBufAllocator* alloc = nullptr;
  if(mode == BufferMode::DmaBufImport && !a.donor.empty())
  {
    if(!donor.init(fmt.width, fmt.height, fmt.fourcc, fmt.sizeImage, a.slots))
    {
      r.verdict = "SKIP(donor): " + donor.lastError();
      return r;
    }
    alloc = &donor;
    r.mode = std::string(toString(mode)) + "/" + donor.name();
  }
  else if(mode == BufferMode::DmaBufImport)
  {
    if(!gbm.init())
    {
      r.verdict = "SKIP(no-gbm)";
      return r;
    }
    if(drmFourccFromV4L2(fmt.fourcc) == 0)
    {
      r.verdict = std::string("SKIP(no-drm-fourcc-for-") + fourccStr(fmt.fourcc) + ")";
      return r;
    }
    alloc = &gbm;
    r.mode = std::string(toString(mode)) + "/" + gbm.name();
  }

  if(!s.start(mode, a.slots, alloc))
  {
    char buf[160];
    std::snprintf(
        buf, sizeof(buf), "FAIL(start) [%ux%u %s stride=%u size=%u x%d]: %s",
        fmt.width, fmt.height, fourccStr(fmt.fourcc).c_str(), fmt.bytesPerLine,
        fmt.sizeImage, a.slots,
        (alloc && std::string(s.lastError()).find("Allocator") != std::string::npos
             ? gbm.lastError()
             : s.lastError().c_str()));
    r.verdict = buf;
    return r;
  }
  r.ran = true;

  const auto t0 = std::chrono::steady_clock::now();
  int got = 0;
  bool sawContent = false;
  std::uint64_t firstSeq = 0;
  bool haveFirst = false;

  while(got < a.frames)
  {
    const int idx = s.dequeue(2000);
    if(idx == -1)
    {
      r.verdict = "FAIL(timeout)";
      s.stop();
      return r;
    }
    if(idx < 0)
    {
      r.verdict = "FAIL(stream): " + s.lastError();
      s.stop();
      return r;
    }

    const auto& sl = s.slot(std::size_t(idx));
    if(!haveFirst)
    {
      firstSeq = sl.sequence;
      haveFirst = true;
    }
    if(mode == BufferMode::MmapRead && sl.mapped[0])
    {
      const auto st = inspect(
          static_cast<const std::uint8_t*>(sl.mapped[0]),
          sl.bytesUsed ? sl.bytesUsed : fmt.sizeImage);
      if(!st.uniform && st.nonZero > 0)
        sawContent = true;
    }
    else if(sl.dmabufFd[0] >= 0)
    {
      // Both fd-based modes: map the buffer the kernel just filled and prove
      // it holds a real frame. For DmaBufImport this is the assertion that
      // matters -- it is memory *we* supplied, so content here means the
      // driver really DMA'd into the external buffer.
      const auto st = inspectDmaBuf(
          sl.dmabufFd[0], sl.bytesUsed ? sl.bytesUsed : fmt.sizeImage);
      if(!st.uniform && st.nonZero > 0)
        sawContent = true;
    }

    if(!s.requeue(std::size_t(idx)))
    {
      r.verdict = "FAIL(requeue): " + s.lastError();
      s.stop();
      return r;
    }
    got++;
  }

  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  r.frames = got;
  r.fps = secs > 0 ? got / secs : 0;
  r.dropped = s.droppedFrames();
  r.errors = s.errorFrames();
  r.contentOk = sawContent;
  (void)firstSeq;

  s.stop();

  if(!sawContent)
    r.verdict = "FAIL(no-content)";
  else if(r.errors > 0)
    r.verdict = "FAIL(error-frames)";
  else
  {
    r.pass = true;
    r.verdict = "PASS";
  }
  return r;
}
} // namespace

int main(int argc, char** argv)
{
  Args a;
  for(int i = 1; i < argc; ++i)
  {
    const std::string s = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if(s == "--device")
      a.device = next();
    else if(s == "--mode")
      a.mode = next();
    else if(s == "--fourcc")
      a.fourcc = next();
    else if(s == "--frames")
      a.frames = std::atoi(next().c_str());
    else if(s == "--slots")
      a.slots = std::atoi(next().c_str());
    else if(s == "--donor")
      a.donor = next();
    else if(s == "--list")
      a.list = true;
    else if(s == "--size")
    {
      const auto v = next();
      const auto x = v.find('x');
      if(x != std::string::npos)
      {
        a.width = std::atoi(v.substr(0, x).c_str());
        a.height = std::atoi(v.substr(x + 1).c_str());
      }
    }
  }

  auto devices = enumerateDevices();
  if(!a.device.empty())
  {
    std::vector<DeviceInfo> filtered;
    for(auto& d : devices)
      if(d.path == a.device)
        filtered.push_back(d);
    devices = filtered;
    if(devices.empty())
    {
      std::printf("no such capture device: %s\n", a.device.c_str());
      return 2;
    }
  }
  if(devices.empty())
  {
    std::printf("no V4L2 capture devices\n");
    return 2;
  }

  std::printf("== %zu capture device(s) ==\n", devices.size());
  for(auto& d : devices)
  {
    Session s;
    if(!s.open(d.path))
      continue;
    const auto c = s.bufferCaps();
    const auto f = s.format();
    std::printf(
        "%-14s %-12s %s  %ux%u %s  buf:%s%s%s\n", d.path.c_str(), d.driver.c_str(),
        d.multiplanar ? "MP" : "SP", f.width, f.height, fourccStr(f.fourcc).c_str(),
        c.mmap ? "mmap " : "", c.dmabuf ? "dmabuf " : "", c.userptr ? "userptr" : "");
  }
  if(a.list)
    return 0;

  const BufferMode allModes[]
      = {BufferMode::MmapRead, BufferMode::MmapExport, BufferMode::DmaBufImport};
  std::vector<BufferMode> modes;
  if(a.mode.empty())
    for(auto m : allModes)
      modes.push_back(m);
  else
  {
    for(auto m : allModes)
      if(a.mode == toString(m))
        modes.push_back(m);
    if(modes.empty())
    {
      std::printf("unknown --mode '%s'\n", a.mode.c_str());
      return 2;
    }
  }

  std::printf(
      "\n%-14s %-14s %-7s %-8s %-6s %-6s %s\n", "device", "mode", "frames", "fps",
      "drop", "err", "verdict");
  std::vector<Result> results;
  for(const auto& d : devices)
    for(auto m : modes)
    {
      auto r = runCell(d, m, a, !a.mode.empty());
      std::printf(
          "%-14s %-14s %-7d %-8.2f %-6llu %-6llu %s\n", r.device.c_str(),
          r.mode.c_str(), r.frames, r.fps, (unsigned long long)r.dropped,
          (unsigned long long)r.errors, r.verdict.c_str());
      results.push_back(std::move(r));
    }

  int failed = 0, passed = 0, skipped = 0;
  for(const auto& r : results)
  {
    if(r.pass)
      passed++;
    else if(r.verdict.rfind("SKIP", 0) == 0)
      skipped++;
    else
      failed++;
  }
  std::printf("\n%d PASS, %d FAIL, %d SKIP\n", passed, failed, skipped);
  return failed ? 1 : 0;
}
