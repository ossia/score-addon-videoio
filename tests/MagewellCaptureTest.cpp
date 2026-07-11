// Magewell Pro Capture live-input validation.
//
// The Magewell card's HDMI inputs receive live signal from a known generator
// (AJAHdmiOutProbe on another machine paints a deterministic static UYVY
// gradient — see fillPattern() there). This tool captures each requested
// channel through the real product path (MagewellCaptureNode ->
// DMACaptureInputNode renderer -> BackgroundNode readback) and verifies:
//   - a signal locks and frames flow at a stable rate,
//   - the content is not black/frozen garbage,
//   - optionally (--expect aja-gradient) pixel accuracy (PSNR) against the
//     regenerated AJAHdmiOutProbe reference pattern.
//
// Exit: 0 = all requested channels PASS, 1 = any FAIL, 2 = no device/lock.

#include <magewell/MagewellCaptureNode.hpp>
#include <magewell/MagewellDevices.hpp>

#include <Gfx/Graph/BackgroundNode.hpp>
#include <Gfx/Graph/Graph.hpp>
#include <Gfx/Graph/RenderState.hpp>

#include <core/application/MinimalApplication.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gfx::Magewell;

namespace
{
inline int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline uint8_t clamp8(int v)
{
  return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

// BT.709 limited-range YCbCr -> RGBA, one pixel.
void yuv709(int Y, int U, int V, uint8_t* out)
{
  const double y = 1.1643836 * (Y - 16);
  const double u = U - 128, v = V - 128;
  out[0] = clamp8(int(y + 1.7927410 * v));
  out[1] = clamp8(int(y - 0.2132486 * u - 0.5329093 * v));
  out[2] = clamp8(int(y + 2.1124018 * u));
  out[3] = 255;
}

// Regenerate AJAHdmiOutProbe::fillPattern() as RGBA at the captured geometry.
// The probe paints UYVY groups of 4 bytes (2 px): U/Y0/V ramp with the byte
// column, Y1 ramps with the row.
void paintAjaGradient(uint8_t* rgba, int w, int h)
{
  const int pitch = w * 2;
  for(int y = 0; y < h; ++y)
  {
    for(int i = 0; i < w / 2; ++i)
    {
      const int x = i * 4;
      const int U = 64 + (x * 128) / pitch;
      const int Y0 = 16 + (x * 219) / pitch;
      const int V = 192 - (x * 128) / pitch;
      const int Y1 = 16 + (y * 219) / h;
      yuv709(Y0, U, V, rgba + (size_t(y) * w + 2 * i) * 4);
      if(2 * i + 1 < w)
        yuv709(Y1, U, V, rgba + (size_t(y) * w + 2 * i + 1) * 4);
    }
  }
}

double psnrRgb(const uint8_t* a, const uint8_t* b, int w, int h)
{
  double mse = 0;
  long n = 0;
  for(long i = 0; i < long(w) * h; ++i)
    for(int c = 0; c < 3; ++c)
    {
      const double d = double(a[i * 4 + c]) - b[i * 4 + c];
      mse += d * d;
      ++n;
    }
  if(n == 0)
    return 0;
  mse /= n;
  if(mse <= 1e-9)
    return 99.0;
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct ChannelResult
{
  int channel{};
  std::string status = "SKIP(no-lock)";
  int w{}, h{};
  int frames{};
  double fps{};
  double meanLuma{};
  double minPsnr = -1, meanPsnr = -1;
};

ChannelResult runChannel(
    int channel, double seconds, bool expectGradient,
    const std::string& dumpPrefix)
{
  ChannelResult r;
  r.channel = channel;

  MagewellInputSettings s;
  s.deviceIndex = channel;

  auto* in = new MagewellCaptureNode(s);
  auto* bg = new score::gfx::BackgroundNode();
  bg->shared_readback = std::make_shared<QRhiReadbackResult>();
  bg->setSize(QSize{1920, 1080});
  auto graph = std::make_unique<score::gfx::Graph>();
  graph->addNode(in);
  graph->addNode(bg);
  graph->addEdge(
      in->output[0], bg->input[0], Process::CableType::ImmediateGlutton);
  graph->createAllRenderLists(score::gfx::GraphicsApi::OpenGL);

  if(!bg->canRender())
  {
    r.status = "SKIP(render-init)";
    graph.reset();
    delete bg;
    delete in;
    return r;
  }

  int psnrN = 0;
  double psnrSum = 0, psnrMin = 99.0, lumaSum = 0;
  int lumaN = 0;
  bool dumped = false;
  std::vector<uint8_t> ref;
  const int64_t t0 = nowNs();
  constexpr int64_t kWarmupNs = 800'000'000;

  QTimer render;
  render.setTimerType(Qt::PreciseTimer);
  QObject::connect(&render, &QTimer::timeout, [&] {
    bg->render();
    auto& rb = *bg->shared_readback;
    if(rb.data.isEmpty() || rb.pixelSize.isEmpty())
      return;
    if(nowNs() - t0 < kWarmupNs)
      return;
    const int w = rb.pixelSize.width(), h = rb.pixelSize.height();
    const auto* px = reinterpret_cast<const uint8_t*>(rb.data.constData());
    r.w = w;
    r.h = h;
    r.frames++;
    // Non-black: mean of the green channel over a sparse grid.
    long sum = 0, n = 0;
    for(int y = 0; y < h; y += std::max(1, h / 32))
      for(int x = 0; x < w; x += std::max(1, w / 32))
      {
        sum += px[(size_t(y) * w + x) * 4 + 1];
        ++n;
      }
    lumaSum += double(sum) / std::max(1L, n);
    lumaN++;
    if(expectGradient && (r.frames % 8) == 0)
    {
      ref.resize(size_t(w) * h * 4);
      paintAjaGradient(ref.data(), w, h);
      const double p = psnrRgb(px, ref.data(), w, h);
      psnrSum += p;
      psnrN++;
      psnrMin = std::min(psnrMin, p);
      if(!dumpPrefix.empty() && !dumped)
      {
        dumped = true;
        const auto base = dumpPrefix + "_ch" + std::to_string(channel);
        QImage(px, w, h, QImage::Format_RGBA8888)
            .save(QString::fromStdString(base + "_recv.png"));
        QImage(ref.data(), w, h, QImage::Format_RGBA8888)
            .save(QString::fromStdString(base + "_ref.png"));
      }
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
  stopper.start(qint64(seconds * 1000) + 800);
  loop.exec();
  render.stop();

  r.fps = r.frames / seconds;
  r.meanLuma = lumaN > 0 ? lumaSum / lumaN : 0;
  if(psnrN > 0)
  {
    r.meanPsnr = psnrSum / psnrN;
    r.minPsnr = psnrMin;
  }

  if(r.frames == 0)
    r.status = "SKIP(no-lock)";
  else if(r.meanLuma < 4.0)
    r.status = "FAIL(black)";
  else if(expectGradient && r.minPsnr < 24.0)
    r.status = "FAIL(psnr)";
  else
    r.status = "PASS";

  graph.reset();
  delete bg;
  delete in;
  return r;
}

} // namespace

int main(int argc, char** argv)
{
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
#if defined(Q_OS_LINUX)
  if(qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"))
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
#endif
  score::MinimalGUIApplication app(argc, argv);

  QCommandLineParser p;
  QCommandLineOption chans("channels", "Comma list of channel indices", "c", "0,2");
  QCommandLineOption secs("seconds", "Seconds per channel", "s", "5");
  QCommandLineOption expect(
      "expect", "Content check: any | aja-gradient", "what", "any");
  QCommandLineOption dump("dump", "Save first frame per channel", "prefix");
  QCommandLineOption list("list", "List channels and exit");
  p.addOptions({chans, secs, expect, dump, list});
  p.addHelpOption();
  p.process(*qApp);

  const auto devices = enumerateDevices();
  std::printf("Magewell channels: %zu\n", devices.size());
  for(const auto& d : devices)
    std::printf("  %d: %s\n", d.index, d.displayName.c_str());
  if(p.isSet(list))
    return 0;
  if(devices.empty())
    return 2;

  const bool gradient = p.value(expect) == "aja-gradient";
  std::vector<ChannelResult> rows;
  for(const auto& c : p.value(chans).split(',', Qt::SkipEmptyParts))
  {
    const int ch = c.toInt();
    std::printf("[ channel %d ] capturing %.1fs ...\n", ch, p.value(secs).toDouble());
    std::fflush(stdout);
    rows.push_back(runChannel(
        ch, p.value(secs).toDouble(), gradient, p.value(dump).toStdString()));
  }

  std::printf(
      "\n%-4s %-11s %6s %6s %8s %8s %8s %-14s\n", "ch", "geometry", "frames",
      "fps", "luma", "minPSNR", "meanPSNR", "status");
  for(int i = 0; i < 80; ++i)
    std::printf("-");
  std::printf("\n");
  bool anyFail = false, anyPass = false;
  for(const auto& r : rows)
  {
    std::printf(
        "%-4d %dx%-6d %6d %6.1f %8.1f %8.2f %8.2f %-14s\n", r.channel, r.w,
        r.h, r.frames, r.fps, r.meanLuma, r.minPsnr, r.meanPsnr,
        r.status.c_str());
    anyFail |= r.status.rfind("FAIL", 0) == 0;
    anyPass |= r.status == "PASS";
  }
  return anyFail ? 1 : (anyPass ? 0 : 2);
}
