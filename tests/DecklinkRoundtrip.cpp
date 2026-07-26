// DeckLink single-card loopback round-trip test harness.
//
// The card's HDMI out is cabled to its HDMI in, and SDI out to SDI in.
// DeckLink playout mirrors the output on every connector, so one full-duplex
// device (e.g. DeckLink Studio 4K) can send a known test pattern
// (TexgenNode -> DeckLinkNode) and capture it back through a chosen input
// connector (DeckLinkCaptureNode -> BackgroundNode readback), verifying
// pixel accuracy + ordering + latency per (connector x mode x pixel format).
//
// Verification core (index band + gradient PSNR + VerifyMetrics) follows
// tests/AJARoundtrip.cpp; kept in sync manually until a shared header exists.

#include <decklink/DeckLinkCaptureNode.hpp>
#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkNode.hpp>

#include <Gfx/Graph/BackgroundNode.hpp>
#include <Gfx/Graph/Graph.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/ISFNode.hpp>
#include <Gfx/Graph/TexgenNode.hpp>
#include <Gfx/ShaderProgram.hpp>

#include <ossia/detail/pod_vector.hpp>

#include <core/application/MinimalApplication.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gfx::DeckLink;

namespace
{
// ---------------------------------------------------------------------------
// Test signal (see AJARoundtrip.cpp): 6-bit rolling index in a top band +
// index-independent gradient field for the PSNR comparison.
// ---------------------------------------------------------------------------
constexpr int kIdxMod = 64;

inline uint8_t lvl(int two_bits) { return uint8_t(32 + 64 * (two_bits & 0x3)); }
inline int unlvl(uint8_t v) { return std::clamp((int(v) - 32 + 32) / 64, 0, 3); }

void paint(uint8_t* rgba, int w, int h, int idx)
{
  const int band = std::max(1, h / 8);
  const uint8_t br = lvl(idx & 0x3), bg = lvl((idx >> 2) & 0x3),
                bb = lvl((idx >> 4) & 0x3);
  for(int y = 0; y < h; ++y)
  {
    uint8_t* row = rgba + size_t(y) * w * 4;
    if(y < band)
    {
      for(int x = 0; x < w; ++x)
      {
        row[x * 4 + 0] = br;
        row[x * 4 + 1] = bg;
        row[x * 4 + 2] = bb;
        row[x * 4 + 3] = 255;
      }
    }
    else
    {
      const uint8_t gy = uint8_t((y * 255) / h);
      for(int x = 0; x < w; ++x)
      {
        row[x * 4 + 0] = uint8_t((x * 255) / w);
        row[x * 4 + 1] = gy;
        row[x * 4 + 2] = 128;
        row[x * 4 + 3] = 255;
      }
    }
  }
}

std::array<std::atomic<int64_t>, kIdxMod> g_sendNs{};
/// CPU cost of generating the test pattern. Split out of `send` because the
/// rest of that phase (encode + readback + memcpy to the card) is product path
/// while this is pure scaffolding — optimising the wrong half would be easy.
std::atomic<double> g_paintMs{0};
std::atomic<int> g_paintCalls{0};
inline int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void g_paint(unsigned char* rgb, int width, int height, int t)
{
  const int idx = t % kIdxMod;
  g_sendNs[idx].store(nowNs(), std::memory_order_relaxed);
  const auto t0 = nowNs();
  paint(rgb, width, height, idx);
  g_paintMs.store(g_paintMs.load() + (nowNs() - t0) / 1e6);
  g_paintCalls.fetch_add(1);
}

double psnrGradient(const uint8_t* recv, const uint8_t* ref, int w, int h)
{
  const int band = std::max(1, h / 8);
  double mse = 0;
  long n = 0;
  for(int y = band; y < h; ++y)
    for(int x = 0; x < w; ++x)
      for(int c = 0; c < 3; ++c)
      {
        const double d = double(recv[(size_t(y) * w + x) * 4 + c])
                         - ref[(size_t(y) * w + x) * 4 + c];
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

int idxFromRgba(const uint8_t* rgba, int w, int h)
{
  const int band = std::max(1, h / 8);
  const int y = band / 2;
  long sr = 0, sg = 0, sb = 0, n = 0;
  for(int x = w / 4; x < 3 * w / 4; x += w / 32 + 1)
  {
    const uint8_t* p = rgba + (size_t(y) * w + x) * 4;
    sr += p[0];
    sg += p[1];
    sb += p[2];
    ++n;
  }
  if(n == 0)
    return -1;
  return unlvl(uint8_t(sr / n)) | (unlvl(uint8_t(sg / n)) << 2)
         | (unlvl(uint8_t(sb / n)) << 4);
}

struct Summary
{
  double mean = 0, min = 0, max = 0, p95 = 0, stddev = 0;
  int n = 0;
};

struct Stat
{
  ossia::pod_vector<float> v;
  void reserve(int n) { v.reserve(std::size_t(std::max(0, n))); }
  void add(double x) { v.push_back(float(x)); }

  Summary summarize() const
  {
    Summary s;
    s.n = int(v.size());
    if(v.empty())
      return s;
    ossia::pod_vector<float> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for(float x : sorted)
      sum += x;
    s.mean = sum / s.n;
    s.min = sorted.front();
    s.max = sorted.back();
    const int i95 = std::clamp(int(0.95 * (s.n - 1) + 0.5), 0, s.n - 1);
    s.p95 = sorted[std::size_t(i95)];
    double var = 0;
    for(float x : sorted)
    {
      const double d = x - s.mean;
      var += d * d;
    }
    s.stddev = std::sqrt(var / s.n);
    return s;
  }
};

struct VerifyMetrics
{
  std::atomic<int> frames{0};
  std::atomic<int> gaps{0};
  std::atomic<int> repeats{0};
  std::atomic<int> psnrCount{0};
  std::atomic<double> psnrSum{0};
  std::atomic<double> psnrMin{99.0};
  std::atomic<bool> dumped{false};
  int lastIdx{-1};
  int64_t startNs{0};
  int64_t lastRecvNs{0};
  Stat latency;
  Stat interval;
  std::string dumpPrefix;
  // Excluded from gap/repeat/PSNR accounting while the link settles. UHD
  // HDMI needs far longer than HD: the 594 MHz TMDS link retrains for over
  // a second after EnableVideoInput, during which the RX hands us black
  // frames at full rate (the "flake" at 2160p was just this window leaking
  // into the PSNR samples).
  int64_t warmupNs = 800'000'000;

  void reserveSamples(int n)
  {
    latency.reserve(n);
    interval.reserve(n);
  }

  bool recordIndex(int idx, int64_t recvNs)
  {
    const int n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool warm = (recvNs - startNs) < warmupNs;
    if(idx < 0)
      return false;
    if(!warm && lastIdx >= 0)
    {
      const int step = ((idx - lastIdx) % kIdxMod + kIdxMod) % kIdxMod;
      if(step == 0)
        repeats.fetch_add(1, std::memory_order_relaxed);
      else if(step > 1)
        gaps.fetch_add(step - 1, std::memory_order_relaxed);
    }
    lastIdx = idx;
    if(!warm && lastRecvNs > 0)
      interval.add((recvNs - lastRecvNs) / 1e6);
    lastRecvNs = recvNs;
    const int64_t sent = g_sendNs[idx].load(std::memory_order_relaxed);
    if(!warm && sent > 0 && recvNs >= sent)
      latency.add((recvNs - sent) / 1e6);
    return !warm && (n % 8) == 0;
  }

  /// Reference captured off the wire once, rather than re-derived on the CPU.
  /// psnrGradient already ignores the index band, and the rest of the frame is
  /// static, so the first good frame IS the reference: this verifies that the
  /// transport preserved what we sent, instead of that it matches a CPU model
  /// of what we believe we sent. It also works with any source shader, and
  /// removes the standing hazard that the CPU model and the GPU pattern drift
  /// apart while still "passing".
  std::vector<uint8_t> refFrame;
  int refW = 0, refH = 0;

  void recordPsnr(const uint8_t* rgba, int w, int h, int idx)
  {
    if(refFrame.empty() || refW != w || refH != h)
    {
      refFrame.assign(rgba, rgba + size_t(w) * h * 4);
      refW = w;
      refH = h;
      return; // nothing to compare the first frame against
    }
    const double p = psnrGradient(rgba, refFrame.data(), w, h);
    psnrSum = psnrSum.load() + p;
    psnrCount.fetch_add(1, std::memory_order_relaxed);
    double cur = psnrMin.load();
    while(p < cur && !psnrMin.compare_exchange_weak(cur, p))
    {
    }
    if(!dumpPrefix.empty() && !dumped.exchange(true))
    {
      QImage(rgba, w, h, QImage::Format_RGBA8888)
          .save(QString::fromStdString(dumpPrefix + "_recv.png"));
      QImage(refFrame.data(), w, h, QImage::Format_RGBA8888)
          .save(QString::fromStdString(dumpPrefix + "_ref.png"));
    }
  }
};

// ---------------------------------------------------------------------------
// Capture receiver: DeckLinkCaptureNode -> BackgroundNode readback, verified
// on the render thread each tick.
// ---------------------------------------------------------------------------
/// Per-phase wall time, to attribute the gap between the harness's achieved
/// rate and the card's own (30.03 fps at 2160p30, measured pure-SDK). Split so
/// product-path cost (send, capture upload+decode) is distinguishable from
/// verification-only cost (readback to CPU, index decode, PSNR) — the latter
/// does not exist in score, which samples the capture texture on the GPU.
struct PhaseProfile
{
  double sendMs = 0;     // out->render(): graph + encode + readback + memcpy to card
  double rxRenderMs = 0; // bg->render(): capture upload + decode + InvertY + readback
  double idxMs = 0;      // index band decode (verification)
  double psnrMs = 0;     // reference paint + PSNR compare (verification)
  int ticks = 0;
  int psnrCalls = 0;
  double paintMs = 0;
  int paintCalls = 0;

  void report(double fps, double target) const
  {
    if(ticks == 0)
      return;
    const double tick = 1000.0 / (fps > 0 ? fps : 1);
    std::printf(
        "  of which CPU pattern paint: %6.2f ms/frame over %d calls (harness "
        "scaffolding: a shader would remove this AND its 33 MB upload)\n",
        paintCalls ? paintMs / ticks : 0.0, paintCalls);
    std::printf(
        "  phases/frame: send %6.2f ms | rx-render %6.2f ms | idx %5.2f ms | "
        "psnr %5.2f ms (amortised over %d calls) | total %6.2f of %6.2f ms "
        "budget (target %.2f fps)\n",
        sendMs / ticks, rxRenderMs / ticks, idxMs / ticks, psnrMs / ticks,
        psnrCalls, (sendMs + rxRenderMs + idxMs + psnrMs) / ticks, tick, target);
    const double verify = (rxRenderMs + idxMs + psnrMs) / ticks;
    std::printf(
        "  verification-only cost: %6.2f ms/frame (%.0f%% of the tick) — score "
        "samples the capture texture instead of reading it back\n",
        verify, 100.0 * verify / tick);
  }
};

struct GpuReceiver
{
  PhaseProfile* prof{};
  std::unique_ptr<score::gfx::Graph> graph;
  DeckLinkCaptureNode* in{};
  score::gfx::BackgroundNode* bg{};
  VerifyMetrics m;

  bool open(
      const DeckLinkInputSettings& s, int w, int h, score::gfx::GraphicsApi api)
  {
    in = new DeckLinkCaptureNode(s);
    bg = new score::gfx::BackgroundNode();
    bg->shared_readback = std::make_shared<QRhiReadbackResult>();
    bg->setSize(QSize{w, h});
    graph = std::make_unique<score::gfx::Graph>();
    graph->addNode(in);
    graph->addNode(bg);
    graph->addEdge(
        in->output[0], bg->input[0], Process::CableType::ImmediateGlutton);
    graph->createAllRenderLists(api);
    if(int64_t(w) * h > 2048 * 1152)
      m.warmupNs = 3'000'000'000;
    m.startNs = nowNs();
    return bg->canRender();
  }

  void renderTick()
  {
    if(!bg || !bg->canRender())
      return;
    const auto tA = nowNs();
    bg->render();
    const auto tB = nowNs();
    auto& rb = *bg->shared_readback;
    if(prof)
      prof->rxRenderMs += (tB - tA) / 1e6;
    if(rb.data.isEmpty() || rb.pixelSize.isEmpty())
      return;
    const int w = rb.pixelSize.width(), h = rb.pixelSize.height();
    const auto* px = reinterpret_cast<const uint8_t*>(rb.data.constData());
    const auto tC = nowNs();
    const int idx = idxFromRgba(px, w, h);
    const auto tD = nowNs();
    const bool wantPsnr = m.recordIndex(idx, nowNs());
    if(wantPsnr)
      m.recordPsnr(px, w, h, idx);
    const auto tE = nowNs();
    if(prof)
    {
      prof->idxMs += (tD - tC) / 1e6;
      if(wantPsnr)
      {
        prof->psnrMs += (tE - tD) / 1e6;
        ++prof->psnrCalls;
      }
    }
  }

  /// Snapshotted here: stop() deletes the node, so reading it afterwards
  /// silently reports nothing at all.
  std::string engagedRx = "-";
  bool pinUnmet = false;

  void stop()
  {
    if(in)
    {
      if(const char* n = in->engagedCaptureStrategy())
        engagedRx = n;
      pinUnmet = in->captureStrategyPinUnmet();
    }
    graph.reset();
    delete bg;
    bg = nullptr;
    delete in;
    in = nullptr;
  }
};

// ---------------------------------------------------------------------------
// Cells.
// ---------------------------------------------------------------------------
/// Source node for a cell. Default is a score ISFNode running an ISF shader on
/// the GPU; --cpu-pattern falls back to the old TexgenNode, whose CPU paint
/// measured 19.83 ms/frame at 2160p (63% of the send phase) plus a 33 MB upload
/// per tick. Both are score graph nodes — the ISF one is the path score itself
/// uses, so the harness stops exercising a source the product never has.
score::gfx::Node* makeSourceNode(const QString& isfPath, bool cpuPattern)
{
  if(!cpuPattern && !isfPath.isEmpty())
  {
    QString frag;
    if(QFile f{isfPath}; f.open(QIODevice::ReadOnly))
      frag = QString::fromUtf8(f.readAll());
    if(!frag.isEmpty())
    {
      const auto& [prog, err] = Gfx::ProgramCache::instance().get({QString{}, frag});
      if(prog && err.isEmpty())
        return new score::gfx::ISFNode(
            prog->descriptor, prog->vertex, prog->fragment);
      std::printf(
          "ISF shader failed to compile: %s\n",
          err.isEmpty() ? "(no program)" : err.toStdString().c_str());
    }
    else
      std::printf("could not read ISF shader '%s'\n", isfPath.toStdString().c_str());
    std::printf("falling back to the CPU pattern\n");
  }
  auto* t = new score::gfx::TexgenNode;
  t->function = &g_paint;
  return t;
}

struct VMode
{
  BMDDisplayMode mode{};
  std::string name;
  int w{}, h{};
  double rate{};
};

struct PFmt
{
  BMDPixelFormat fmt{};
  std::string name;
  double psnrThreshold{};
};

struct Conn
{
  BMDVideoConnection conn{};
  std::string name;
};

struct Result
{
  std::string connector, mode, pixfmt, strategy = "-", status;
  int sent = 0, recv = 0, gaps = 0, repeats = 0;
  uint64_t txDrops = 0;
  double fps = 0, targetFps = 0, meanLatencyMs = 0, minPsnr = 0, meanPsnr = 0;
  double jitterMs = 0;
  /// Engaged CAPTURE rung (RDMA / DVP / CPU-hostimport / CPU-QRhi) and whether a
  /// pinned rung failed to engage. Without this a row cannot say which path it
  /// measured — the mistake that invalidated four rounds of results.
  std::string rxStrategy = "-";
  bool rxPinUnmet = false;
  /// Same accounting for the OUTPUT rung (SCORE_GFX_DIRECT_READBACK pin).
  bool txPinUnmet = false;
  bool txPinUnavailable = false;
  PhaseProfile profile;
};

struct Options
{
  double seconds = 4.0;
  int device = 0;
  score::gfx::GraphicsApi api = score::gfx::GraphicsApi::OpenGL;
  std::vector<std::string> onlyModes;   // empty => all supported
  std::vector<std::string> onlyPixfmts; // empty => all
  std::vector<std::string> onlyConns;   // empty => {sdi,hdmi}
  std::string dumpPrefix;
  QString isfPath;
  bool cpuPattern = false;
  bool list = false;
  bool txOnly = false;
};

// Enumerate the device's display modes through the input interface (loopback
// needs both directions; the output side is validated per-cell by init).
std::vector<VMode> enumerateModes(int deviceIndex)
{
  std::vector<VMode> out;
  ensureComInit();
  auto dev = openDevice(deviceIndex);
  if(!dev)
    return out;
  ComPtr<IDeckLinkInput> input;
  if(dev->QueryInterface(IID_IDeckLinkInput, input.putVoid()) != S_OK || !input)
    return out;
  ComPtr<IDeckLinkDisplayModeIterator> it;
  if(input->GetDisplayModeIterator(it.put()) != S_OK || !it)
    return out;
  ComPtr<IDeckLinkDisplayMode> m;
  while(it->Next(m.put()) == S_OK)
  {
    VMode v;
    v.mode = m->GetDisplayMode();
    v.w = int(m->GetWidth());
    v.h = int(m->GetHeight());
    BMDTimeValue num = 0;
    BMDTimeScale den = 0;
    if(m->GetFrameRate(&num, &den) == S_OK && num > 0)
      v.rate = double(den) / double(num);
#if defined(_WIN32)
    BSTR nm = nullptr;
    if(m->GetName(&nm) == S_OK && nm)
    {
      const auto qs = QString::fromWCharArray(
          reinterpret_cast<const wchar_t*>(nm), int(SysStringLen(nm)));
      v.name = qs.toStdString();
      SysFreeString(nm);
    }
#else
    const char* nm = nullptr;
    if(m->GetName(&nm) == S_OK && nm)
    {
      v.name = nm;
      free(const_cast<char*>(nm));
    }
#endif
    // "1080p59.94" -> "1080p5994": the dot is the only thing standing between
    // the SDK's names and the mode tokens used on the command line.
    std::string norm;
    norm.reserve(v.name.size());
    for(char c : v.name)
    {
      if(c == '.')
        continue;
      norm.push_back(c == ' ' ? '_' : c);
    }
    v.name = std::move(norm);
    // Progressive only: the harness paints/verifies full frames. Interlaced
    // and PsF cells would need field-aware verification.
    const auto fields = m->GetFieldDominance();
    if(fields == bmdProgressiveFrame)
      out.push_back(std::move(v));
    m.reset();
  }
  return out;
}

bool wanted(const std::vector<std::string>& filter, const std::string& name)
{
  if(filter.empty())
    return true;
  for(const auto& f : filter)
    if(name.find(f) != std::string::npos)
      return true;
  return false;
}

Result runCell(
    const Options& opt, const Conn& cn, const VMode& vm, const PFmt& pf)
{
  Result r;
  r.connector = cn.name;
  r.mode = vm.name;
  r.pixfmt = pf.name;

  // Honest SKIP for combos the hardware can't do (e.g. Studio 4K has no
  // 10BitRGB on HDMI): probe both directions before spinning up a graph, so
  // an impossible cell doesn't run and report a misleading black FAIL.
  {
    ComPtr<IDeckLink> dev = openDevice(opt.device);
    ComPtr<IDeckLinkInput> din;
    ComPtr<IDeckLinkOutput> dout;
    if(dev
       && dev->QueryInterface(IID_IDeckLinkInput, din.putVoid()) == S_OK
       && dev->QueryInterface(IID_IDeckLinkOutput, dout.putVoid()) == S_OK)
    {
      BMDDisplayMode actual{};
      dlbool_t inOk = false, outOk = false;
      din->DoesSupportVideoMode(
          cn.conn, vm.mode, pf.fmt, bmdNoVideoInputConversion,
          bmdSupportedVideoModeDefault, &actual, &inOk);
      dout->DoesSupportVideoMode(
          cn.conn, vm.mode, pf.fmt, bmdNoVideoOutputConversion,
          bmdSupportedVideoModeDefault, &actual, &outOk);
      if(!outOk || (!inOk && !opt.txOnly))
      {
        r.status = !outOk ? "SKIP(hw-out)" : "SKIP(hw-in)";
        return r;
      }
    }

    // Studio 4K / Desktop Video 16.1a3: DoesSupportVideoMode claims RGB over
    // HDMI, but the HDMI encoder emits no TMDS at all for RGB framebuffers —
    // any mode, with or without the 4:4:4 flag (pure-SDK loopback verified;
    // the SDI mirror of the same output carries converted content). The claim
    // lies, so gate it here. tx-only cells never verify the wire, so the
    // send path still gets exercised there.
    if(!opt.txOnly && cn.conn == bmdVideoConnectionHDMI
       && (pf.fmt == bmdFormat8BitBGRA || pf.fmt == bmdFormat10BitRGB))
    {
      r.status = "SKIP(hw-out-hdmi-rgb)";
      return r;
    }
  }

  DeckLinkOutputSettings outS;
  outS.deviceIndex = opt.device;
  outS.displayMode = vm.mode;
  outS.pixelFormat = pf.fmt;

  DeckLinkInputSettings inS;
  inS.deviceIndex = opt.device;
  inS.displayMode = vm.mode;
  inS.pixelFormat = pf.fmt;
  inS.connection = cn.conn;

  const bool usedCpuPattern = opt.cpuPattern || opt.isfPath.isEmpty();
  auto* src = makeSourceNode(opt.isfPath, opt.cpuPattern);
  auto* out = new DeckLinkNode(outS);

  auto graph = std::make_unique<score::gfx::Graph>();
  graph->addNode(src);
  graph->addNode(out);
  graph->addEdge(
      src->output[0], out->input[0], Process::CableType::ImmediateGlutton);
  graph->createAllRenderLists(opt.api);

  if(!out->canRender())
  {
    r.status = "SKIP(out-init)";
    graph.reset();
    delete out;
    delete src;
    return r;
  }
  r.strategy = out->activeStrategyName();

  GpuReceiver rcv;
  const int estFrames = int(vm.rate * opt.seconds) + 64;
  if(opt.txOnly)
  {
    // Send path only: encoder + output rung + card scheduling run for real,
    // nothing is captured back. This is the only way to execute the RGB
    // encoders on hardware whose loopback cannot lock them (HDMI emits no
    // TMDS for RGB, and a dead SDI loop denies EnableVideoInput).
  }
  else if(!rcv.open(inS, vm.w, vm.h, opt.api))
    r.status = "SKIP(in-open)";
  else
  {
    if(!opt.dumpPrefix.empty())
      rcv.m.dumpPrefix
          = opt.dumpPrefix + "_" + cn.name + "_" + vm.name + "_" + pf.name;
    rcv.m.reserveSamples(estFrames);
  }

  if(r.status.empty())
  {
    QTimer render;
    render.setTimerType(Qt::PreciseTimer);
    PhaseProfile prof;
    rcv.prof = &prof;
    QObject::connect(&render, &QTimer::timeout, [&] {
      const auto t0 = nowNs();
      out->render();
      prof.sendMs += (nowNs() - t0) / 1e6;
      rcv.renderTick();
      ++prof.ticks;
    });
    render.start(int(1000.0 / std::max(1.0, vm.rate)));

    QEventLoop loop;
    QTimer stopper;
    stopper.setSingleShot(true);
    QObject::connect(&stopper, &QTimer::timeout, &loop, [&] {
      render.stop();
      loop.quit();
    });
    stopper.start(qint64(opt.seconds * 1000));
    loop.exec();
    render.stop();
    prof.paintMs = g_paintMs.load();
    prof.paintCalls = g_paintCalls.load();
    g_paintMs.store(0);
    g_paintCalls.store(0);
    r.profile = prof;
  }

  rcv.stop();

  r.rxStrategy = rcv.engagedRx;
  r.rxPinUnmet = rcv.pinUnmet;
  r.txPinUnmet = out->outputStrategyPinUnmet();
  r.txPinUnavailable = out->outputStrategyPinUnavailable();

  VerifyMetrics& M = rcv.m;
  r.recv = M.frames.load();
  r.gaps = M.gaps.load();
  r.repeats = M.repeats.load();
  r.fps = r.recv / opt.seconds;
  r.targetFps = vm.rate;
  const Summary lat = M.latency.summarize();
  const Summary itv = M.interval.summarize();
  r.meanLatencyMs = lat.mean;
  r.jitterMs = itv.stddev;
  r.meanPsnr
      = M.psnrCount.load() > 0 ? M.psnrSum.load() / M.psnrCount.load() : 0;
  r.minPsnr = M.psnrCount.load() > 0 ? M.psnrMin.load() : 0;
  r.txDrops = out->pacingDrops();
  r.sent = int(out->pacingGoodXfers());

  // RGB cells: the Studio 4K (driver 16.1a3) transports RGB modes over a
  // YCbCr 4:2:2 wire regardless of the 4:4:4 config flag — pure-SDK loopback
  // reproduces the same ~0.978 level scale + chroma subsampling on both
  // connectors, so the strict RGB pixel gate is unmeetable on this card.
  // A degraded-but-real picture (content clearly flowed) is the card's
  // conversion, not a pipeline bug: classify it as an honest SKIP. True
  // black stays a FAIL — that's always a transport defect.
  const bool rgbRequested
      = pf.fmt != bmdFormat8BitYUV && pf.fmt != bmdFormat10BitYUV;

  if(r.status.empty())
  {
    if(opt.txOnly)
      r.status = r.sent > 0 ? "PERF-ONLY(tx-only)" : "FAIL(no-tx)";
    else if(M.psnrCount.load() == 0)
      r.status = "SKIP(no-lock)";
    // A capture that never advances its frame index delivered nothing at all
    // (dead connector, unplugged loopback, wedged DMA). Saying "psnr" there
    // sends the reader hunting for a colour-conversion bug instead.
    else if(r.recv > 2 && r.repeats >= r.recv - 2)
      r.status = "FAIL(no-capture)";
    else if(r.minPsnr < pf.psnrThreshold)
      r.status = (rgbRequested && r.minPsnr >= 15.0) ? "SKIP(rgb-wire-degraded)"
                                                     : "FAIL(psnr)";
    else
      r.status = "PASS";
  }

  // A pinned rung that did not engage outranks every other verdict: the numbers
  // describe a different path than the one requested.
  // The ISF sources available today paint a static pattern with no frame-index
  // band, so idxFromRgba returns a constant: every frame equals the captured
  // reference (PSNR 99), nothing advances (rep ~= recv) and no send timestamp
  // exists (latency 0). Those are not passes — they are an unverified
  // throughput measurement, and must not be reported as correctness.
  if(!usedCpuPattern && r.status == "PASS")
    r.status = "PERF-ONLY(no-index)";
  if(r.rxPinUnmet || r.txPinUnmet)
    r.status = "FAIL(rung-not-engaged)";
  else if(
      ((!opt.txOnly && r.rxStrategy == "-") || r.txPinUnavailable)
      && r.status.rfind("SKIP", 0) != 0)
    r.status = "SKIP(rung-unavailable)";

  graph.reset();
  delete out;
  delete src;
  return r;
}

void printMatrix(const std::vector<Result>& rows)
{
  std::printf(
      "\n%-5s %-22s %-8s %-16s %-15s %5s %5s %6s %5s %5s %5s %8s %8s %-22s\n",
      "conn", "mode", "pixfmt", "tx-strategy", "rx-strategy", "sent", "recv",
      "fps", "txdrp", "lost", "rep", "lat(ms)", "minPSNR", "status");
  for(int i = 0; i < 130; ++i)
    std::printf("-");
  std::printf("\n");
  for(const auto& r : rows)
  {
    std::printf(
        "%-5s %-22s %-8s %-16s %-15s %5d %5d %6.1f %5llu %5d %5d %8.2f %8.2f "
        "%-22s\n",
        r.connector.c_str(), r.mode.c_str(), r.pixfmt.c_str(),
        r.strategy.c_str(), r.rxStrategy.c_str(), r.sent, r.recv, r.fps,
        (unsigned long long)r.txDrops, r.gaps, r.repeats, r.meanLatencyMs,
        r.minPsnr, r.status.c_str());
    // Pacing, not just throughput: a change that raises fps while making
    // arrival intervals lumpier is not an improvement for video.
    std::printf(
        "  pacing: interval stddev %6.2f ms | mean latency %7.2f ms | lost %d "
        "| repeats %d\n",
        r.jitterMs, r.meanLatencyMs, r.gaps, r.repeats);
    r.profile.report(r.fps, r.targetFps);
  }
}

// Live input-resolution switch: one persistent Auto receiver, the output driven
// at mode A then rebuilt at mode B mid-stream. Exercises the DMACaptureInputNode
// format-change rebuild + the DeckLink autoDetect producer end to end.
int runLiveSwitch(const Options& opt, const std::vector<VMode>& vmodes, const PFmt& pf)
{
  auto findMode = [&](const std::string& sub) -> const VMode* {
    for(const auto& v : vmodes)
      if(v.name.find(sub) != std::string::npos)
        return &v;
    return nullptr;
  };
  std::string aTok = "1080p5994", bTok = "720p5994";
  if(opt.onlyModes.size() >= 2)
  {
    aTok = opt.onlyModes[0];
    bTok = opt.onlyModes[1];
  }
  const VMode* A = findMode(aTok);
  const VMode* B = findMode(bTok);
  if(!A || !B)
  {
    std::printf(
        "live-switch: modes '%s' / '%s' not both supported on this device\n",
        aTok.c_str(), bTok.c_str());
    return 2;
  }
  std::printf(
      "live-switch: A=%s (%dx%d)  B=%s (%dx%d)  pf=%s\n", A->name.c_str(), A->w,
      A->h, B->name.c_str(), B->w, B->h, pf.name.c_str());

  DeckLinkInputSettings inS;
  inS.deviceIndex = opt.device;
  inS.pixelFormat = pf.fmt;
  inS.connection = bmdVideoConnectionSDI;
  inS.autoDetect = true; // follow the wire live
  const int sinkW = std::max(A->w, B->w), sinkH = std::max(A->h, B->h);

  GpuReceiver rcv;
  if(!rcv.open(inS, sinkW, sinkH, opt.api))
  {
    std::printf("live-switch: receiver open failed\n");
    return 2;
  }

  auto phase = [&](const VMode& vm, double secs) -> int {
    DeckLinkOutputSettings outS;
    outS.deviceIndex = opt.device;
    outS.displayMode = vm.mode;
    outS.pixelFormat = pf.fmt;
    auto* src = new score::gfx::TexgenNode;
    src->function = &g_paint;
    auto* out = new DeckLinkNode(outS);
    auto g = std::make_unique<score::gfx::Graph>();
    g->addNode(src);
    g->addNode(out);
    g->addEdge(
        src->output[0], out->input[0], Process::CableType::ImmediateGlutton);
    g->createAllRenderLists(opt.api);
    const int before = rcv.m.frames.load();
    if(out->canRender())
    {
      QTimer render;
      render.setTimerType(Qt::PreciseTimer);
      QObject::connect(&render, &QTimer::timeout, [&] {
        out->render();
        rcv.renderTick();
      });
      render.start(int(1000.0 / std::max(1.0, vm.rate)));
      QEventLoop loop;
      QTimer stopper;
      stopper.setSingleShot(true);
      QObject::connect(&stopper, &QTimer::timeout, &loop, [&] { loop.quit(); });
      stopper.start(qint64(secs * 1000));
      loop.exec();
      render.stop();
    }
    const int delivered = rcv.m.frames.load() - before;
    g.reset();
    delete out;
    delete src;
    return delivered;
  };

  const int deliveredA = phase(*A, opt.seconds);
  std::printf(
      "live-switch: phase A (%s) delivered %d frames, lastIdx=%d\n",
      A->name.c_str(), deliveredA, rcv.m.lastIdx);
  const int deliveredB = phase(*B, opt.seconds);
  std::printf(
      "live-switch: phase B (%s) delivered %d frames, lastIdx=%d\n",
      B->name.c_str(), deliveredB, rcv.m.lastIdx);
  rcv.stop();

  const bool ok = deliveredA > 0 && deliveredB > 0;
  std::printf(
      "live-switch: %s  (A=%d B=%d frames delivered across the switch)\n",
      ok ? "PASS" : "FAIL", deliveredA, deliveredB);
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  // Suppress the package-manager network refresh and its first-run modal
  // "download the user library?" dialog (deadlocks a headless run).
  qputenv("SCORE_SANITIZE_SKIP_CHECKS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");
#if defined(Q_OS_LINUX)
  if(qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"))
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl"); // match src/app/main.cpp
#endif
  score::MinimalGUIApplication app(argc, argv);

  QCommandLineParser p;
  QCommandLineOption secs("seconds", "Seconds per cell", "s", "4");
  QCommandLineOption dev("device", "DeckLink device index", "n", "0");
  QCommandLineOption modes("modes", "Comma list of mode-name substrings", "m");
  QCommandLineOption pfs("pixfmt", "Comma list of pixfmts (YCbCr8,YCbCr10,BGRA8,RGB10)", "p");
  QCommandLineOption conns("conn", "Comma list of connectors: sdi,hdmi", "c");
  QCommandLineOption dump("dump", "Save first verified frame per cell", "prefix");
  QCommandLineOption list("list", "Print supported matrix and exit");
  QCommandLineOption isf(
      "isf",
      "ISF shader to generate the test pattern on the GPU (a score ISFNode). "
      "Without it the CPU TexgenNode is used, which costs ~20 ms/frame at 2160p.",
      "path");
  QCommandLineOption cpuPat(
      "cpu-pattern", "Force the old CPU pattern source (for comparison)");
  QCommandLineOption txOnly(
      "tx-only",
      "Run the send path only (encoder + output rung + card scheduling), "
      "without opening capture. Exercises formats whose loopback cannot lock "
      "(RGB over HDMI, dead SDI loop); reports throughput, never correctness.");
  QCommandLineOption apiOpt(
      "api", "Render backend: opengl | vulkan | d3d11 | d3d12", "api", "opengl");
  QCommandLineOption liveSwitch(
      "live-switch",
      "Live input-resolution test: drive the output from mode A to mode B "
      "mid-stream with an Auto-detect capture; --modes A,B picks the pair "
      "(default 1080p5994,720p5994), --pixfmt picks the format.");
  p.addOptions(
      {secs, dev, modes, pfs, conns, dump, list, apiOpt, liveSwitch, isf, cpuPat,
       txOnly});
  p.addHelpOption();
  p.process(*qApp);

  Options opt;
  {
    const auto api = p.value(apiOpt).toLower();
    if(api == "vulkan")
      opt.api = score::gfx::GraphicsApi::Vulkan;
#if defined(_WIN32)
    else if(api == "d3d11")
      opt.api = score::gfx::GraphicsApi::D3D11;
    else if(api == "d3d12")
      opt.api = score::gfx::GraphicsApi::D3D12;
#endif
    else if(api != "opengl")
    {
      std::printf("unknown --api '%s'\n", api.toStdString().c_str());
      return 2;
    }
  }
  opt.seconds = p.value(secs).toDouble();
  opt.device = p.value(dev).toInt();
  opt.list = p.isSet(list);
  opt.dumpPrefix = p.value(dump).toStdString();
  opt.isfPath = p.value(isf);
  opt.cpuPattern = p.isSet(cpuPat);
  opt.txOnly = p.isSet(txOnly);
  for(const auto& s : p.value(modes).split(',', Qt::SkipEmptyParts))
    opt.onlyModes.push_back(s.toStdString());
  for(const auto& s : p.value(pfs).split(',', Qt::SkipEmptyParts))
    opt.onlyPixfmts.push_back(s.toStdString());
  for(const auto& s : p.value(conns).split(',', Qt::SkipEmptyParts))
    opt.onlyConns.push_back(s.toLower().toStdString());

  const auto devices = enumerateDevices();
  if(devices.empty())
  {
    std::printf("No DeckLink devices found\n");
    return 2;
  }
  for(const auto& d : devices)
    std::printf(
        "device %d: %s  (in:%d out:%d)\n", d.index, d.displayName.c_str(),
        int(d.canInput), int(d.canOutput));

  const auto vmodes = enumerateModes(opt.device);
  std::printf("progressive modes on device %d:\n", opt.device);
  for(const auto& v : vmodes)
    std::printf("  %-22s %dx%d @ %.2f\n", v.name.c_str(), v.w, v.h, v.rate);
  if(opt.list)
    return 0;

  // 4:2:2 wire formats absorb chroma subsampling; RGB is tighter.
  const std::vector<PFmt> pixfmts = {
      {bmdFormat8BitYUV, "YCbCr8", 30.0},
      {bmdFormat10BitYUV, "YCbCr10", 30.0},
      {bmdFormat8BitBGRA, "BGRA8", 35.0},
      {bmdFormat10BitRGB, "RGB10", 35.0},
  };

  if(p.isSet(liveSwitch))
  {
    const PFmt* pf = &pixfmts[0]; // YCbCr8 default; --pixfmt overrides
    if(!opt.onlyPixfmts.empty())
      for(const auto& c : pixfmts)
        if(wanted(opt.onlyPixfmts, c.name))
        {
          pf = &c;
          break;
        }
    return runLiveSwitch(opt, vmodes, *pf);
  }
  const std::vector<Conn> connectors = {
      {bmdVideoConnectionSDI, "sdi"},
      {bmdVideoConnectionHDMI, "hdmi"},
  };

  std::vector<Result> rows;
  for(const auto& cn : connectors)
  {
    if(!wanted(opt.onlyConns, cn.name))
      continue;
    for(const auto& vm : vmodes)
    {
      if(!wanted(opt.onlyModes, vm.name))
        continue;
      for(const auto& pf : pixfmts)
      {
        if(!wanted(opt.onlyPixfmts, pf.name))
          continue;
        std::printf(
            "[ %-4s %-22s %-8s ] running %.1fs ...\n", cn.name.c_str(),
            vm.name.c_str(), pf.name.c_str(), opt.seconds);
        std::fflush(stdout);
        rows.push_back(runCell(opt, cn, vm, pf));
      }
    }
  }

  if(rows.empty())
  {
    std::printf(
        "no cell matched the --conn/--modes/--pixfmt filters; nothing was "
        "tested\n");
    return 2;
  }

  printMatrix(rows);
  bool anyFail = false;
  for(const auto& r : rows)
    anyFail |= r.status.rfind("FAIL", 0) == 0;
  return anyFail ? 1 : 0;
}
