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
// The kernel cells above need neither a GPU nor a display. `--api` adds the
// renderer cells on top, driving the real product path (V4L2CaptureNode ->
// DMACaptureInputNode renderer -> BackgroundNode readback) three times:
//   cpu/still     the reference. vivid's OSD counter is switched off so the
//                 source repeats exactly; the first frame becomes the golden.
//   dmabuf/still  content proof: every rendered frame is compared against that
//                 golden. The imported texture must decode to the same pixels
//                 the host-staged upload produced -- which a frame count or a
//                 non-black check cannot show.
//   dmabuf/live   liveness proof: with the OSD counter back on, every source
//                 frame differs, so counting readbacks that changed separates
//                 "buffers keep circulating" from "one buffer redrawn forever",
//                 which is what a broken borrowed-buffer requeue looks like.
//
//   V4L2CaptureTest --api opengl --device /dev/video0
//   V4L2CaptureTest --api vulkan --seconds 4
//
// Exit code is nonzero if any pinned mode failed to engage or any cell FAILed.

#include "V4L2DonorAllocator.hpp"

#include <v4l2/V4L2CaptureNode.hpp>
#include <v4l2/V4L2GbmAllocator.hpp>
#include <v4l2/V4L2NvBufAllocator.hpp>
#include <v4l2/V4L2Session.hpp>

#include <Gfx/Graph/BackgroundNode.hpp>
#include <Gfx/Graph/Graph.hpp>
#include <Gfx/Graph/RenderState.hpp>

#include <core/application/MinimalApplication.hpp>

#include <linux/dma-buf.h>
#include <linux/videodev2.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>

#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
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
  std::string api;    ///< empty = kernel cells only
  double seconds{4.}; ///< per renderer cell
  std::string dump;
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
  NvBufAllocator nvbuf;
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
  else if(mode == BufferMode::DmaBufImport && nvBufSurfaceAvailable()
          && nvbuf.init())
  {
    // Tegra: NVIDIA's V4L2 path takes the importer role only, so the buffers
    // have to come from NvBufSurface.
    alloc = &nvbuf;
    r.mode = std::string(toString(mode)) + "/" + nvbuf.name();
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
    // An allocator that cannot express this pixel format is a platform
    // limitation, not a defect: report it as an unavailable rung so it does
    // not read as a regression in the matrix.
    if(alloc && s.lastError().find("Allocator") != std::string::npos)
    {
      const std::string why = (alloc == &gbm)      ? gbm.lastError()
                              : (alloc == &nvbuf)  ? nvbuf.lastError().c_str()
                                                   : donor.lastError();
      r.verdict = "SKIP(allocator): " + why + " ["
                  + fourccStr(fmt.fourcc) + "]";
      return r;
    }
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

// ---------------------------------------------------------------------------
// Renderer cells: the real product path, per capture rung.
// ---------------------------------------------------------------------------

/// vivid stamps a per-frame counter over the test pattern. Off, the source is
/// bit-identical frame to frame, which is what makes a golden-frame comparison
/// exact; on, every frame differs, which is what makes the liveness cell able
/// to tell "buffers are still circulating" from "the picture froze".
/// Best-effort: the control does not exist on a real camera.
bool setOsdText(const std::string& path, bool off)
{
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if(fd < 0)
    return false;
  bool found = false;
  v4l2_queryctrl q{};
  q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
  while(::ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0)
  {
    const std::string name = reinterpret_cast<const char*>(q.name);
    if(name.find("OSD Text Mode") != std::string::npos)
    {
      // The menu is ordered All / Counters Only / None; pick by label rather
      // than by index, since index 0 is "All" -- the opposite of "no text".
      const char* label = off ? "None" : "All";
      int want = off ? q.maximum : q.minimum;
      for(int v = q.minimum; v <= q.maximum; ++v)
      {
        v4l2_querymenu m{};
        m.id = q.id;
        m.index = std::uint32_t(v);
        if(::ioctl(fd, VIDIOC_QUERYMENU, &m) == 0
           && std::string(reinterpret_cast<const char*>(m.name)) == label)
        {
          want = v;
          break;
        }
      }
      v4l2_control c{};
      c.id = q.id;
      c.value = want;
      found = ::ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
      break;
    }
    q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
  }
  ::close(fd);
  return found;
}

double psnrRgba(const std::uint8_t* a, const std::uint8_t* b, int w, int h)
{
  double mse = 0;
  long n = 0;
  for(long i = 0; i < long(w) * h; ++i)
    for(int c = 0; c < 3; ++c)
    {
      const double d = double(a[i * 4 + c]) - double(b[i * 4 + c]);
      mse += d * d;
      ++n;
    }
  if(n == 0)
    return 0;
  mse /= double(n);
  if(mse <= 1e-9)
    return 99.0;
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct RenderCell
{
  std::string device, pin;
  std::string engaged = "-";
  bool pinUnmet{};
  int w{}, h{};
  int frames{};
  double fps{};
  double meanLuma{};
  double minPsnr{-1}, meanPsnr{-1};
  /// Readbacks whose content differed from the previous one. On a source whose
  /// picture changes every frame this is the only thing that separates "frames
  /// keep arriving" from "the last buffer is being redrawn forever" -- which is
  /// exactly what a broken borrowed-buffer requeue looks like.
  int distinct{};
  bool uniform{true};
  std::string verdict = "SKIP";
  bool pass{};
};

/// One renderer run. Every rendered frame is compared against @p golden; when
/// that is empty the run's own first stable frame becomes the reference, which
/// is what turns the CPU cell into a source-stability floor for the GPU cell's
/// numbers rather than an assumed-perfect baseline.
RenderCell runRenderCell(
    const DeviceInfo& dev, const Args& a, score::gfx::GraphicsApi api,
    const char* pin, std::vector<std::uint8_t> golden,
    std::vector<std::uint8_t>* goldenOut)
{
  RenderCell r;
  r.device = dev.path;
  r.pin = pin;

  V4L2InputSettings s;
  s.device = dev.path;
  s.width = std::uint32_t(a.width);
  s.height = std::uint32_t(a.height);
  s.fourcc = parseFourcc(a.fourcc);

  // The output is sized to the wire geometry so the readback compares 1:1 with
  // the reference instead of through a rescale.
  QSize outSize{1280, 720};
  {
    Session probe;
    if(probe.open(dev.path))
    {
      if(a.width || a.height || !a.fourcc.empty())
        probe.configure(s.width, s.height, s.fourcc);
      const auto f = probe.format();
      if(f.width > 0 && f.height > 0)
        outSize = QSize(int(f.width), int(f.height));
    }
  }

  qputenv("SCORE_GFX_CAPTURE_STRATEGY", pin);

  auto* in = new Gfx::V4L2::V4L2CaptureNode(s);
  auto* bg = new score::gfx::BackgroundNode();
  bg->shared_readback = std::make_shared<QRhiReadbackResult>();
  bg->setSize(outSize);
  auto graph = std::make_unique<score::gfx::Graph>();
  graph->addNode(in);
  graph->addNode(bg);
  graph->addEdge(
      in->output[0], bg->input[0], Process::CableType::ImmediateGlutton);
  graph->createAllRenderLists(api);

  if(!bg->canRender())
  {
    r.verdict = "SKIP(render-init)";
    graph.reset();
    delete bg;
    delete in;
    return r;
  }

  const auto t0 = std::chrono::steady_clock::now();
  constexpr double kWarmupSecs = 1.0;
  double psnrSum = 0, psnrMin = 99.0, lumaSum = 0;
  int psnrN = 0, lumaN = 0;
  bool dumped = false;
  std::vector<std::uint8_t> prevGrid;

  QTimer render;
  render.setTimerType(Qt::PreciseTimer);
  QObject::connect(&render, &QTimer::timeout, [&] {
    bg->render();
    auto& rb = *bg->shared_readback;
    if(rb.data.isEmpty() || rb.pixelSize.isEmpty())
      return;
    if(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
       < kWarmupSecs)
      return;
    const int w = rb.pixelSize.width(), h = rb.pixelSize.height();
    const auto* px = reinterpret_cast<const std::uint8_t*>(rb.data.constData());
    r.w = w;
    r.h = h;
    r.frames++;

    long sum = 0, n = 0;
    const std::uint8_t first = px[1];
    std::vector<std::uint8_t> grid;
    for(int y = 0; y < h; y += std::max(1, h / 48))
      for(int x = 0; x < w; x += std::max(1, w / 48))
      {
        const auto g = px[(std::size_t(y) * w + x) * 4 + 1];
        if(g != first)
          r.uniform = false;
        grid.push_back(g);
        sum += g;
        ++n;
      }
    if(!prevGrid.empty() && grid != prevGrid)
      r.distinct++;
    prevGrid = std::move(grid);
    lumaSum += double(sum) / double(std::max(1L, n));
    lumaN++;

    if(golden.empty())
    {
      golden.assign(px, px + std::size_t(w) * h * 4);
      if(goldenOut)
        *goldenOut = golden;
      return;
    }

    if(golden.size() == std::size_t(w) * h * 4)
    {
      const double p = psnrRgba(px, golden.data(), w, h);
      psnrSum += p;
      psnrMin = std::min(psnrMin, p);
      psnrN++;
    }
    if(!a.dump.empty() && !dumped)
    {
      dumped = true;
      QImage(px, w, h, QImage::Format_RGBA8888)
          .save(QString::fromStdString(a.dump + "_" + std::string(pin) + ".png"));
    }
  });
  render.start(8);

  QEventLoop loop;
  QTimer stopper;
  stopper.setSingleShot(true);
  QObject::connect(&stopper, &QTimer::timeout, &loop, [&] {
    render.stop();
    loop.quit();
  });
  stopper.start(qint64((a.seconds + kWarmupSecs) * 1000));
  loop.exec();
  render.stop();

  r.fps = a.seconds > 0 ? r.frames / a.seconds : 0;
  r.meanLuma = lumaN > 0 ? lumaSum / lumaN : 0;
  if(psnrN > 0)
  {
    r.meanPsnr = psnrSum / psnrN;
    r.minPsnr = psnrMin;
  }
  if(const char* n = in->engagedCaptureStrategy())
    r.engaged = n;
  r.pinUnmet = in->captureStrategyPinUnmet();

  graph.reset();
  delete bg;
  delete in;
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
    else if(s == "--api")
      a.api = next();
    else if(s == "--seconds")
      a.seconds = std::atof(next().c_str());
    else if(s == "--dump")
      a.dump = next();
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

  if(a.api.empty())
    return failed ? 1 : 0;

  // --- renderer cells -------------------------------------------------------
  auto api = score::gfx::GraphicsApi::OpenGL;
  if(a.api == "vulkan")
    api = score::gfx::GraphicsApi::Vulkan;
  else if(a.api != "opengl")
  {
    std::printf("unknown --api '%s'\n", a.api.c_str());
    return 2;
  }

  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_SANITIZE_SKIP_CHECKS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");
  // The EGL DMA-BUF importer only works against an EGL-backed GL context;
  // Qt's XCB plugin defaults to GLX, where the rung cannot exist at all.
  if(qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"))
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
  score::MinimalGUIApplication app(argc, argv);

  std::printf(
      "\n%-14s %-11s %-22s %6s %6s %8s %8s %8s %8s %s\n", "device", "pin",
      "engaged-rung", "frames", "fps", "luma", "distinct", "minPSNR",
      "meanPSNR", "verdict");
  std::vector<RenderCell> cells;
  for(const auto& d : devices)
  {
    const bool hasOsd = setOsdText(d.path, /*off=*/true);

    std::vector<std::uint8_t> golden;
    auto ref = runRenderCell(d, a, api, "cpu", {}, &golden);
    // The CPU rung is the reference: without it there is nothing to compare
    // the zero-copy path's pixels against, so its failure invalidates the row.
    if(ref.frames == 0)
      ref.verdict = "FAIL(no-frames)";
    else if(ref.meanLuma < 4.0 || ref.uniform)
      ref.verdict = "FAIL(black-or-uniform)";
    else
    {
      ref.pass = true;
      ref.verdict = "PASS(reference)";
    }
    cells.push_back(ref);

    // Cross-run comparison is only meaningful for a source that repeats. The
    // reference cell measured that directly by comparing its own frames, so
    // the gate is its self-PSNR rather than a driver name: vivid with its OSD
    // counter off is bit-exact, a live camera with auto-exposure is not.
    const bool comparable = ref.pass && ref.minPsnr >= 20.0;
    auto gpu = runRenderCell(
        d, a, api, "dmabuf", comparable ? golden : std::vector<std::uint8_t>{},
        nullptr);
    // The zero-copy frames must be at least as faithful to the reference frame
    // as the CPU rung's own frames were -- an absolute threshold would either
    // pass a broken import on a noisy source or fail a correct one.
    const double floorPsnr
        = ref.minPsnr > 0 ? std::max(6.0, ref.minPsnr - 3.0) : 30.0;
    if(gpu.pinUnmet || gpu.engaged.find("dmabuf") == std::string::npos)
      gpu.verdict = "SKIP(no-dmabuf-import: " + gpu.engaged + ")";
    else if(gpu.frames == 0)
      gpu.verdict = "FAIL(no-frames)";
    else if(gpu.meanLuma < 4.0 || gpu.uniform)
      gpu.verdict = "FAIL(black-or-uniform)";
    else if(comparable && gpu.minPsnr < floorPsnr)
      gpu.verdict = "FAIL(content-mismatch)";
    else if(!hasOsd && ref.distinct > 0 && gpu.distinct < 2)
      // No OSD control to force a changing picture, but the source moved on its
      // own during the reference cell: the zero-copy cell must move too, or
      // slots stopped being handed back and one frame is being redrawn.
      gpu.verdict = "FAIL(frozen)";
    else
    {
      gpu.pass = true;
      gpu.verdict = comparable ? "PASS(matches-cpu)" : "PASS(content-only)";
    }
    cells.push_back(gpu);

    if(!hasOsd || !gpu.pass)
      continue;
    // Liveness: a source whose picture changes every frame. If the strategy
    // stopped handing slots back the driver would run out of buffers and the
    // renderer would keep redrawing the last one it bound.
    setOsdText(d.path, /*off=*/false);
    auto live = runRenderCell(d, a, api, "dmabuf", {}, nullptr);
    live.pin = "dmabuf/live";
    if(live.pinUnmet || live.engaged.find("dmabuf") == std::string::npos)
      live.verdict = "SKIP(no-dmabuf-import: " + live.engaged + ")";
    else if(live.distinct < 2)
      live.verdict = "FAIL(frozen)";
    else
    {
      live.pass = true;
      live.verdict = "PASS(live)";
    }
    cells.push_back(live);
    setOsdText(d.path, /*off=*/true);
  }

  for(const auto& c : cells)
  {
    std::printf(
        "%-14s %-11s %-22s %6d %6.1f %8.1f %8d %8.2f %8.2f %s\n",
        c.device.c_str(), c.pin.c_str(), c.engaged.c_str(), c.frames, c.fps,
        c.meanLuma, c.distinct, c.minPsnr, c.meanPsnr, c.verdict.c_str());
    if(c.pass)
      passed++;
    else if(c.verdict.rfind("SKIP", 0) == 0)
      skipped++;
    else
      failed++;
  }
  std::printf("\n%d PASS, %d FAIL, %d SKIP (kernel + renderer)\n", passed, failed,
              skipped);
  return failed ? 1 : 0;
}
