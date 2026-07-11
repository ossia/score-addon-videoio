// AJA card-to-card round-trip test harness.
//
// Sends a known test pattern from one AJA card (real gfx output path:
// TexgenNode -> AJANode) over SDI, captures it on a second card (real
// CPU-staging capture path: makeAJACapture -> Video::ExternalInput), and
// verifies pixel-accuracy + ordering + latency across the firmware-supported
// mode matrix. See plan: sharded-launching-rabbit.md.

#include <AJA/AJACpuCapture.hpp>
#include <AJA/AJAInput.hpp>
#include <AJA/AjaFormatMap.hpp>
#include <AJA/AJAInputNode.hpp>
#include <AJA/AJAOutput.hpp>
#include <AJA/AJAOutputNode.hpp>

#include <Gfx/Graph/BackgroundNode.hpp>
#include <Gfx/Graph/Graph.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/TexgenNode.hpp>
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/interop/StageProfiler.hpp>

#include "ShaderTexgenNode.hpp"

#include <QtGui/private/qrhi_p.h>
#if QT_CONFIG(opengl)
#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#endif

// Vulkan<->CUDA external-memory probe support (the zero-copy capture
// foundation). Gated like the CaptureInteropVulkanTier3 strategy.
#if QT_CONFIG(vulkan) && defined(SCORE_HAS_AJA_CUDA_BRIDGE)
#define AJA_HAS_VK_INTEROP_PROBE 1
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <score/gfx/Vulkan.hpp>

#include <QtGui/private/qrhivulkan_p.h>
#endif

#include <Video/ExternalInput.hpp>

#include <ossia/detail/pod_vector.hpp>

#include <core/application/MinimalApplication.hpp>

#include <ntv2card.h>
#include <ntv2devicefeatures.h>
#include <ntv2enums.h>
#include <ntv2formatdescriptor.h>
#include <ntv2utils.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <array>
#include <set>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace Gfx::AJA;

namespace
{
// ---------------------------------------------------------------------------
// Test signal. A frame carries a 6-bit rolling index in a top band (coarse
// 3-channel encoding, robust to 4:2:2 / 8-bit / BT.709 limited round-trip)
// plus an index-independent gradient field used for the PSNR comparison.
// ---------------------------------------------------------------------------
constexpr int kIdxMod = 64; // 6 bits across 3 channels (2 bits each)

// 2-bit channel level -> 8-bit value with wide margin (steps of 64).
inline uint8_t lvl(int two_bits) { return uint8_t(32 + 64 * (two_bits & 0x3)); }
inline int unlvl(uint8_t v) { return std::clamp((int(v) - 32 + 32) / 64, 0, 3); }

// Pure painter: deterministic from (idx). Band height = h/8.
void paint(uint8_t* rgba, int w, int h, int idx)
{
  const int band = std::max(1, h / 8);
  const uint8_t br = lvl(idx & 0x3), bg = lvl((idx >> 2) & 0x3), bb = lvl((idx >> 4) & 0x3);
  for(int y = 0; y < h; ++y)
  {
    uint8_t* row = rgba + size_t(y) * w * 4;
    if(y < band)
    {
      for(int x = 0; x < w; ++x)
      {
        row[x * 4 + 0] = br; row[x * 4 + 1] = bg; row[x * 4 + 2] = bb; row[x * 4 + 3] = 255;
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

// Send-time per index slot, written by the render-thread painter.
std::array<std::atomic<int64_t>, kIdxMod> g_sendNs{};
inline int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// Lightweight performance sample accumulator. Stores raw samples in a
// pod_vector (no per-push zero-init) and derives mean / min / max / percentiles
// / stddev once at the end — never on the hot path. Single-producer (the
// owning thread); read only after that thread has stopped.
// ---------------------------------------------------------------------------
struct Summary
{
  double mean = 0, min = 0, max = 0, p50 = 0, p95 = 0, p99 = 0, stddev = 0;
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
    const auto pct = [&](double p) {
      const int i = std::clamp(int(p * (s.n - 1) + 0.5), 0, s.n - 1);
      return double(sorted[std::size_t(i)]);
    };
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
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

// TexgenNode painter (func_t): paint + stamp send time. `t` is TexgenNode's
// monotonic frame counter; we fold it to the rolling index.
void g_paint(unsigned char* rgb, int width, int height, int t)
{
  const int idx = t % kIdxMod;
  g_sendNs[idx].store(nowNs(), std::memory_order_relaxed);
  SCORE_STAGE_PROFILE(profPaint, "texgen-cpu-paint");
  paint(rgb, width, height, idx);
}

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// BT.709 limited-range YCbCr -> RGB for one pixel.
void yuv709(int Y, int U, int V, uint8_t* out)
{
  const double y = 1.1643836 * (Y - 16);
  const double u = U - 128, v = V - 128;
  out[0] = clamp8(int(y + 1.7927410 * v));
  out[1] = clamp8(int(y - 0.2132486 * u - 0.5329093 * v));
  out[2] = clamp8(int(y + 2.1124018 * u));
  out[3] = 255;
}

// Cheap index recovery straight from the source AVFrame (samples a few band
// pixels) — avoids a full-frame conversion on the hot path.
int recoverIndexRaw(const AVFrame* f)
{
  const int w = f->width, h = f->height;
  const int band = std::max(1, h / 8);
  const int y = band / 2;
  long sr = 0, sg = 0, sb = 0, n = 0;
  uint8_t px[4];
  for(int x = w / 4; x < 3 * w / 4; x += w / 32 + 1)
  {
    switch(f->format)
    {
      case AV_PIX_FMT_UYVY422:
      {
        const uint8_t* p = f->data[0] + size_t(y) * f->linesize[0] + (x & ~1) * 2;
        yuv709(p[1], p[0], p[2], px);
        break;
      }
      case AV_PIX_FMT_RGBA:
      case AV_PIX_FMT_BGRA:
      case AV_PIX_FMT_ARGB:
      case AV_PIX_FMT_ABGR:
      {
        const uint8_t* p = f->data[0] + size_t(y) * f->linesize[0] + x * 4;
        if(f->format == AV_PIX_FMT_RGBA) { px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; }
        else if(f->format == AV_PIX_FMT_BGRA) { px[0] = p[2]; px[1] = p[1]; px[2] = p[0]; }
        else if(f->format == AV_PIX_FMT_ARGB) { px[0] = p[1]; px[1] = p[2]; px[2] = p[3]; }
        else { px[0] = p[3]; px[1] = p[2]; px[2] = p[1]; }
        break;
      }
      default:
        return -1;
    }
    sr += px[0]; sg += px[1]; sb += px[2]; ++n;
  }
  if(n == 0)
    return -1;
  return unlvl(uint8_t(sr / n)) | (unlvl(uint8_t(sg / n)) << 2) | (unlvl(uint8_t(sb / n)) << 4);
}

// Row step shared by frameToRgba and psnrGradient so the converter only
// touches the rows the metric reads (a full scalar UHD conversion costs
// 40-80 ms — enough to overflow the capture queue every verification).
inline int verifyRowStep(int h)
{
  return h >= 4320 ? 4 : h >= 2160 ? 2 : 1;
}

// PSNR over the gradient region (skip the band), comparing a converted-to-RGB
// received frame against the regenerated reference for its decoded index.
double psnrGradient(const uint8_t* recv, const uint8_t* ref, int w, int h)
{
  const int band = std::max(1, h / 8);
  // Row subsampling at ≥4K: the metric stays a dense per-pixel compare on
  // every sampled row; sampling half/quarter of the rows changes minPSNR by
  // <0.05 dB in practice but keeps the verifier off the receive hot path.
  // The grid starts at a multiple of ystep because frameToRgba only
  // converts those rows.
  const int ystep = verifyRowStep(h);
  double mse = 0;
  long n = 0;
  for(int y = ((band + ystep - 1) / ystep) * ystep; y < h; y += ystep)
    for(int x = 0; x < w; ++x)
      for(int c = 0; c < 3; ++c)
      {
        const double d = double(recv[(size_t(y) * w + x) * 4 + c]) - ref[(size_t(y) * w + x) * 4 + c];
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

// Convert an AVFrame to a tightly-packed RGBA8 buffer. Returns false for
// unsupported formats. Only every verifyRowStep(h)-th row is converted;
// psnrGradient walks the same grid, other rows stay unspecified.
bool frameToRgba(const AVFrame* f, std::vector<uint8_t>& rgba)
{
  const int w = f->width, h = f->height;
  const int ystep = verifyRowStep(h);
  rgba.resize(size_t(w) * h * 4);
  switch(f->format)
  {
    case AV_PIX_FMT_UYVY422:
      for(int y = 0; y < h; y += ystep)
      {
        const uint8_t* src = f->data[0] + size_t(y) * f->linesize[0];
        uint8_t* dst = rgba.data() + size_t(y) * w * 4;
        for(int x = 0; x < w; x += 2)
        {
          const int U = src[0], Y0 = src[1], V = src[2], Y1 = src[3];
          yuv709(Y0, U, V, dst + x * 4);
          if(x + 1 < w)
            yuv709(Y1, U, V, dst + (x + 1) * 4);
          src += 4;
        }
      }
      return true;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_ABGR:
      for(int y = 0; y < h; y += ystep)
      {
        const uint8_t* src = f->data[0] + size_t(y) * f->linesize[0];
        uint8_t* dst = rgba.data() + size_t(y) * w * 4;
        for(int x = 0; x < w; ++x)
        {
          const uint8_t* p = src + x * 4;
          // Normalize to RGBA.
          if(f->format == AV_PIX_FMT_RGBA) { dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; }
          else if(f->format == AV_PIX_FMT_BGRA) { dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; }
          else if(f->format == AV_PIX_FMT_ARGB) { dst[0] = p[1]; dst[1] = p[2]; dst[2] = p[3]; }
          else { dst[0] = p[3]; dst[1] = p[2]; dst[2] = p[1]; } // ABGR
          dst[3] = 255;
          dst += 4;
        }
      }
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Mode table: the addon's videoFormat-string vocabulary -> geometry + NTV2.
// Mirrors AJANode::parseVideoFormat so we can enumerate firmware support via
// NTV2DeviceCanDoVideoFormat and drive AJANode/AJAInputNode by the same string.
// ---------------------------------------------------------------------------
struct VFmt
{
  const char* name;
  NTV2VideoFormat fmt;
  int w, h;
  double rate;
  bool quad;          // 4x quad-link 4K (needs 4 cables)
  bool eightK{false}; // quad-quad 8K/UHD2 (4x 4K, needs 4x12G)
};

const std::vector<VFmt>& videoFormatTable()
{
  static const std::vector<VFmt> t = {
      {"1080p2997", NTV2_FORMAT_1080p_2997, 1920, 1080, 29.97, false},
      {"1080p30", NTV2_FORMAT_1080p_3000, 1920, 1080, 30.0, false},
      {"1080p25", NTV2_FORMAT_1080p_2500, 1920, 1080, 25.0, false},
      {"1080p50", NTV2_FORMAT_1080p_5000_A, 1920, 1080, 50.0, false},
      {"1080p5994", NTV2_FORMAT_1080p_5994_A, 1920, 1080, 59.94, false},
      {"1080p60", NTV2_FORMAT_1080p_6000_A, 1920, 1080, 60.0, false},
      {"720p50", NTV2_FORMAT_720p_5000, 1280, 720, 50.0, false},
      {"720p5994", NTV2_FORMAT_720p_5994, 1280, 720, 59.94, false},
      {"720p60", NTV2_FORMAT_720p_6000, 1280, 720, 60.0, false},
      {"UHD_SL_25", NTV2_FORMAT_3840x2160p_2500, 3840, 2160, 25.0, false},
      {"UHD_SL_2997", NTV2_FORMAT_3840x2160p_2997, 3840, 2160, 29.97, false},
      {"UHD_SL_30", NTV2_FORMAT_3840x2160p_3000, 3840, 2160, 30.0, false},
      {"UHD_SL_50", NTV2_FORMAT_3840x2160p_5000, 3840, 2160, 50.0, false},
      {"UHD_SL_5994", NTV2_FORMAT_3840x2160p_5994, 3840, 2160, 59.94, false},
      {"UHD_SL_60", NTV2_FORMAT_3840x2160p_6000, 3840, 2160, 60.0, false},
      {"UHD50", NTV2_FORMAT_4x1920x1080p_5000, 3840, 2160, 50.0, true},
      {"UHD5994", NTV2_FORMAT_4x1920x1080p_5994, 3840, 2160, 59.94, true},
      {"UHD60", NTV2_FORMAT_4x1920x1080p_6000, 3840, 2160, 60.0, true},
  };
  return t;
}

// Exhaustive: enumerate EVERY NTV2VideoFormat both cards' firmware supports,
// deriving geometry from the SDK (no hand-maintained table). 8K/UHD2 formats
// are excluded here — they need the mode8K routing path, handled separately.
const std::vector<VFmt>& enumerateFirmwareFormats(NTV2DeviceID idOut, NTV2DeviceID idIn)
{
  static std::vector<std::string> nameStore;
  static std::vector<VFmt> out;
  out.clear();
  nameStore.clear();
  nameStore.reserve(NTV2_MAX_NUM_VIDEO_FORMATS); // keep c_str() pointers stable
  for(int i = 1; i < NTV2_MAX_NUM_VIDEO_FORMATS; ++i)
  {
    const auto fmt = static_cast<NTV2VideoFormat>(i);
    if(!::NTV2DeviceCanDoVideoFormat(idOut, fmt)
       || !::NTV2DeviceCanDoVideoFormat(idIn, fmt))
      continue;
    NTV2FormatDescriptor fd(fmt, NTV2_FBF_8BIT_YCBCR);
    const int w = static_cast<int>(fd.GetRasterWidth());
    const int h = static_cast<int>(fd.GetVisibleRasterHeight());
    if(w <= 0 || h <= 0)
      continue;
    nameStore.push_back(::NTV2VideoFormatToString(fmt));
    VFmt v;
    v.name = nameStore.back().c_str();
    v.fmt = fmt;
    v.w = w;
    v.h = h;
    v.rate = ::GetFramesPerSecond(::GetNTV2FrameRateFromVideoFormat(fmt));
    v.eightK = NTV2_IS_QUAD_QUAD_FORMAT(fmt);     // 8K/UHD2 (4x 4K)
    v.quad = !v.eightK && NTV2_IS_QUAD_FRAME_FORMAT(fmt); // 4K quad-link
    out.push_back(v);
  }
  return out;
}

// Pixel formats to sweep. CPU capture supports YCbCr8 / RGB8(ARGB); v210 is
// GPU-direct only (no AVFrame staging) and is flagged for the gpu receiver.
struct PFmt
{
  const char* outName;            // AJAOutputSettings::pixelFormat string
  NTV2FrameBufferFormat fbf;      // NTV2 framebuffer format (for card-cap probe)
  AJAInputPixelFormat in;         // matching input enum
  bool cpuCapture;                // supported by the CPU-staging receiver
  double psnrThreshold;           // pass bar for the gradient PSNR
};

const std::vector<PFmt>& pixelFormatTable()
{
  static const std::vector<PFmt> t = {
      {"YCbCr8", NTV2_FBF_8BIT_YCBCR, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // RGB framebuffer OUTPUT (ARGB) is CSC'd to YCbCr on the SDI wire, so we
      // capture it as UYVY — this validates the RGB->CSC->SDI output path end
      // to end. (Native RGB *capture* into an ARGB framestore is a separate
      // input-side CSC routing gap.)
      {"RGB8", NTV2_FBF_ARGB, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // Other 8-bit RGB byte orders the card stores. Like ARGB, these go
      // through the RGB->CSC->SDI path (firmware-black on Kona5-8K) — included
      // to confirm whether the black bug is ARGB-specific or all-RGB.
      {"RGBA8", NTV2_FBF_RGBA, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"ABGR8", NTV2_FBF_ABGR, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // v210 (10-bit YUV) OUTPUT validated by capturing the 10-bit SDI wire as
      // 8-bit UYVY (the card down-converts 10->8 on capture) — exercises the
      // GPU V210Encoder. Full 10-bit capture needs the GPU receiver.
      {"YCbCr10", NTV2_FBF_10BIT_YCBCR, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // Planar 4:2:2 10-bit (3-plane) output via the multi-plane DMA path +
      // YUV422P10 encoder. Wire is 10-bit YCbCr, captured as 8-bit UYVY.
      {"YCbCr10_422P", NTV2_FBF_10BIT_YCBCR_422PL3_LE, AJAInputPixelFormat::YCbCr8,
       true, 24.0},
      // 8-bit 4:2:2 YUY2 byte order (YUY2Encoder), captured as UYVY.
      {"YUY2", NTV2_FBF_8BIT_YCBCR_YUY2, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // High-bit-depth / packed RGB families (12bit firmware). All are RGB
      // framebuffers CSC'd to YCbCr on the wire, captured as 8-bit UYVY — this
      // validates the new PackedRGBEncoder byte layouts end-to-end (a wrong
      // layout shows up as scrambled colour / black, a correct one as ~44 dB
      // like RGB8). True >8-bit fidelity needs a GPU 10/12-bit receiver.
      {"RGB10", NTV2_FBF_10BIT_RGB, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"ARGB10", NTV2_FBF_10BIT_ARGB, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"RGB10DPX", NTV2_FBF_10BIT_DPX, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"RGB10DPXLE", NTV2_FBF_10BIT_DPX_LE, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"RGB12", NTV2_FBF_48BIT_RGB, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"RGB12P", NTV2_FBF_12BIT_RGB_PACKED, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"RGB24", NTV2_FBF_24BIT_RGB, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"BGR24", NTV2_FBF_24BIT_BGR, AJAInputPixelFormat::YCbCr8, true, 24.0},
      // Additional planar YCbCr (3-plane) output formats.
      {"YCbCr8_422P", NTV2_FBF_8BIT_YCBCR_422PL3, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"YCbCr8_420P", NTV2_FBF_8BIT_YCBCR_420PL3, AJAInputPixelFormat::YCbCr8, true, 24.0},
      {"YCbCr10_420P", NTV2_FBF_10BIT_YCBCR_420PL3_LE, AJAInputPixelFormat::YCbCr8, true, 24.0},
  };
  return t;
}

// ---------------------------------------------------------------------------
// Per-cell result.
// ---------------------------------------------------------------------------
struct Result
{
  std::string videoFormat, pixelFormat, interop, strategy, status;
  int sent = 0, recv = 0, drops = 0, gaps = 0, repeats = 0;
  double fps = 0, targetFps = 0, meanLatencyMs = 0, minPsnr = 0, meanPsnr = 0;
  // Performance detail.
  double latP95Ms = 0, latMaxMs = 0;       // latency distribution
  double jitterMs = 0, maxIntervalMs = 0;  // receiver pacing (inter-arrival)
  double renderMeanMs = 0, renderP95Ms = 0, renderMaxMs = 0; // producer cost
  uint64_t txGood = 0, txDrops = 0, txUnderruns = 0;
};

struct Options
{
  int outDevice = 0, inDevice = 1;
  int outChannel = 0, inChannel = 0; // SDI/FrameStore base (connector diag)
  double seconds = 5.0;
  std::vector<std::string> formats;  // empty => default curated set
  std::vector<std::string> pixfmts;  // empty => all in table
  std::vector<std::string> interops; // empty => {cpu,dvp,rdma}
  std::string dumpPrefix;            // if set, dump first verified frame/cell
  std::string rxMode = "cpu";        // cpu (AVFrame capture) | gpu (readback)
  std::string texgen = "gpu";        // gpu (shader pattern) | cpu (legacy paint)
  std::string eightKMode = "tsi";    // 8K routing: tsi | squares (SQD)
  score::gfx::GraphicsApi api = score::gfx::GraphicsApi::OpenGL; // render backend
  bool allFormats = false; // enumerate EVERY firmware-supported video format
  AJAHDRMode hdr = AJAHDRMode::Off; // HDR output transfer (off/hdr10/hlg)
  bool listOnly = false;
  bool benchUpload = false; // card-free upload microbenchmark, then exit
  bool vkInteropProbe = false; // Vulkan<->CUDA external-memory probe, then exit
};

AJAHDRMode parseHdr(const std::string& s)
{
  if(s == "hdr10")
    return AJAHDRMode::HDR10;
  if(s == "hlg")
    return AJAHDRMode::HLG;
  return AJAHDRMode::Off;
}
// Expected AVFrame color_trc the receiver should detect for an HDR mode.
int expectedTrc(AJAHDRMode m)
{
  return m == AJAHDRMode::HDR10  ? AVCOL_TRC_SMPTE2084
         : m == AJAHDRMode::HLG  ? AVCOL_TRC_ARIB_STD_B67
                                 : AVCOL_TRC_BT709;
}

score::gfx::GraphicsApi parseApi(const std::string& s)
{
  if(s == "vulkan" || s == "vk")
    return score::gfx::GraphicsApi::Vulkan;
  if(s == "d3d11" || s == "d3d" || s == "dx11")
    return score::gfx::GraphicsApi::D3D11;
  if(s == "d3d12" || s == "dx12")
    return score::gfx::GraphicsApi::D3D12;
  if(s == "metal" || s == "mtl")
    return score::gfx::GraphicsApi::Metal;
  return score::gfx::GraphicsApi::OpenGL;
}
const char* apiName(score::gfx::GraphicsApi a)
{
  switch(a)
  {
    case score::gfx::GraphicsApi::Vulkan:
      return "vulkan";
    case score::gfx::GraphicsApi::D3D11:
      return "d3d11";
    case score::gfx::GraphicsApi::D3D12:
      return "d3d12";
    case score::gfx::GraphicsApi::Metal:
      return "metal";
    case score::gfx::GraphicsApi::OpenGL:
      return "opengl";
    default:
      return "unknown";
  }
}

// Whether the OUT card can route single-link >=6G UHD (12G crosspoints).
// Filled by probeDevice; gates the honest SKIP for HFR single-link UHD.
static bool g_canDo12gRouting = false;

// Capability probe: open each card briefly, return device ID, close.
bool probeDevice(
    int idx, NTV2DeviceID& outId,
    std::set<NTV2FrameBufferFormat>* outFbfs = nullptr)
{
  CNTV2Card card;
  if(!card.Open(static_cast<UWord>(idx)))
    return false;
  outId = card.GetDeviceID();
  // Capture the framebuffer formats the LIVE firmware advertises — the same
  // runtime check (features().CanDoFrameBufferFormat -> GetSupportedItems) the
  // output node enforces in initializeAJADevice. This is the authoritative
  // "what the loaded bitfile can store"; the static NTV2DeviceCanDoFrameBufferFormat
  // table disagrees for some formats (e.g. it lists no device at all for the
  // planar PL2/PL3 layouts even where the register is writable).
  if(outFbfs)
    for(int i = 0; i < NTV2_FBF_NUMFRAMEBUFFERFORMATS; ++i)
    {
      const auto fbf = static_cast<NTV2FrameBufferFormat>(i);
      if(card.features().CanDoFrameBufferFormat(fbf))
        outFbfs->insert(fbf);
    }
  if(outFbfs) // only the OUT-card probe passes outFbfs
    g_canDo12gRouting = card.features().CanDo12gRouting();
  // Read-only firmware info (informs whether a different bitfile personality
  // might enable other modes).
  UWord fwRev = 0;
  ULWord pkgRev = 0;
  std::string fwDate, fwTime;
  card.GetRunningFirmwareRevision(fwRev);
  card.GetRunningFirmwareDate(fwDate, fwTime);
  card.GetRunningFirmwarePackageRevision(pkgRev);
  std::printf(
      "  card %d firmware: rev %u  pkg %u  built %s %s\n", idx, fwRev, pkgRev,
      fwDate.c_str(), fwTime.c_str());
  card.Close();
  return true;
}

// Index from the header band of a packed RGBA8 frame (GPU readback path).
int idxFromRgba(const uint8_t* rgba, int w, int h)
{
  const int band = std::max(1, h / 8);
  const int y = band / 2;
  long sr = 0, sg = 0, sb = 0, n = 0;
  for(int x = w / 4; x < 3 * w / 4; x += w / 32 + 1)
  {
    const uint8_t* p = rgba + (size_t(y) * w + x) * 4;
    sr += p[0]; sg += p[1]; sb += p[2]; ++n;
  }
  if(n == 0)
    return -1;
  return unlvl(uint8_t(sr / n)) | (unlvl(uint8_t(sg / n)) << 2)
         | (unlvl(uint8_t(sb / n)) << 4);
}

// SCORE_AJA_NO_VERIFY=1: the consumer only dequeues + counts, skipping the
// per-frame band decode and 1-in-8 CPU swscale RGBA + PSNR. Isolates the true
// consumer dequeue cadence (what a real GPU-decode renderer would sustain)
// from the harness's CPU verification cost.
inline bool noVerifyMode()
{
  static const bool v = std::getenv("SCORE_AJA_NO_VERIFY") != nullptr;
  return v;
}

// ---------------------------------------------------------------------------
// Shared per-cell verification: continuity / drops / latency + sampled PSNR.
// ---------------------------------------------------------------------------
struct VerifyMetrics
{
  std::atomic<int> frames{0};
  std::atomic<int> gaps{0};     // frames lost in transit (index step > 1)
  std::atomic<int> repeats{0};  // duplicate frame (index step == 0 => stall)
  std::atomic<int> psnrCount{0};
  std::atomic<double> psnrSum{0};
  std::atomic<double> psnrMin{99.0};
  std::atomic<bool> dumped{false};
  int lastIdx{-1};   // single consumer
  std::atomic<int> lastTrc{-1}; // detected AVFrame color_trc (HDR signaling)
  int64_t startNs{0};
  int64_t lastRecvNs{0};  // single consumer: previous frame arrival
  // Single-consumer perf sample buffers (read after the receiver thread stops).
  Stat latency;   // end-to-end send->receive latency (ms)
  Stat interval;  // inter-arrival spacing (ms): pacing / jitter
  std::string dumpPrefix;
  static constexpr int64_t kWarmupNs = 800'000'000; // ignore SDI lock transient

  // Pre-size the sample buffers so no allocation happens on the hot path.
  void reserveSamples(int n)
  {
    latency.reserve(n);
    interval.reserve(n);
  }

  // Records ordering/latency; returns true if this frame should also be PSNR'd
  // (1-in-8 sample; PSNR is heavy). Skips the warm-up settling transient.
  bool recordIndex(int idx, int64_t recvNs)
  {
    const int n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool warm = (recvNs - startNs) < kWarmupNs;
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
    // Pacing: spacing between successive arrivals (jitter = its stddev).
    if(!warm && lastRecvNs > 0)
      interval.add((recvNs - lastRecvNs) / 1e6);
    lastRecvNs = recvNs;
    // End-to-end latency for this index (send time stamped by the painter).
    const int64_t sent = g_sendNs[idx].load(std::memory_order_relaxed);
    if(!warm && sent > 0 && recvNs >= sent)
      latency.add((recvNs - sent) / 1e6);
    return !warm && (n % 8) == 0;
  }

  void recordPsnr(const uint8_t* rgba, int w, int h, int idx)
  {
    // The gradient region compared by psnrGradient is index-independent
    // (only the band varies, and psnrGradient skips it) — build the
    // reference once per geometry instead of repainting 33-132 MB per
    // verification (that repaint was starving the receive loop at ≥4K).
    if(refW != w || refH != h)
    {
      refCache.resize(size_t(w) * h * 4);
      paint(refCache.data(), w, h, idx);
      refW = w;
      refH = h;
    }
    const double p = psnrGradient(rgba, refCache.data(), w, h);
    psnrSum = psnrSum.load() + p;
    psnrCount.fetch_add(1, std::memory_order_relaxed);
    double cur = psnrMin.load();
    while(p < cur && !psnrMin.compare_exchange_weak(cur, p)) { }
    if(!dumpPrefix.empty() && !dumped.exchange(true))
    {
      QImage(rgba, w, h, QImage::Format_RGBA8888)
          .save(QString::fromStdString(dumpPrefix + "_recv.png"));
      QImage(refCache.data(), w, h, QImage::Format_RGBA8888)
          .save(QString::fromStdString(dumpPrefix + "_ref.png"));
    }
  }

  // recordPsnr is single-consumer per receiver; cached reference frame.
  std::vector<uint8_t> refCache;
  int refW{0}, refH{0};
};

// ---------------------------------------------------------------------------
// CPU-staging capture receiver: real aja_input_capture -> AVFrame, verified on
// a worker thread.
// ---------------------------------------------------------------------------
struct Receiver
{
  std::shared_ptr<::Video::ExternalInput> input;
  std::thread thr;
  std::atomic<bool> run{false};
  VerifyMetrics m;
  bool unsupportedFmt = false;

  bool open(const AJAInputSettings& s)
  {
    input = makeAJACapture(s);
    return input != nullptr;
  }

  void start()
  {
    if(!input)
      return;
    input->start();
    m.startNs = nowNs();
    run = true;
    thr = std::thread([this] { loop(); });
  }

  void loop()
  {
    std::vector<uint8_t> rgba;
    while(run.load(std::memory_order_relaxed))
    {
      AVFrame* f = input->dequeue_frame();
      if(!f)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      const int64_t recvNs = nowNs();
      m.lastTrc.store(f->color_trc, std::memory_order_relaxed); // HDR detect
      if(noVerifyMode())
      {
        m.frames.fetch_add(1, std::memory_order_relaxed); // dequeue cadence only
        input->release_frame(f);
        continue;
      }
      const int idx = recoverIndexRaw(f); // cheap per-frame band decode
      if(idx < 0)
        unsupportedFmt = true;
      if(m.recordIndex(idx, recvNs) && frameToRgba(f, rgba))
        m.recordPsnr(rgba.data(), f->width, f->height, idx);
      input->release_frame(f);
    }
  }

  void stop()
  {
    run = false;
    if(thr.joinable())
      thr.join();
    if(input)
      input->stop();
  }
};

// ---------------------------------------------------------------------------
// GPU-direct capture receiver: AJAInputNode (DVP/RDMA capture) -> BackgroundNode
// readback. Driven from the render thread; reads the captured texture back to
// the CPU each tick for verification. On hardware without DVP/RDMA the input
// node produces no frames and the cell reports SKIP(no-lock).
// ---------------------------------------------------------------------------
struct GpuReceiver
{
  std::unique_ptr<score::gfx::Graph> graph;
  AJAInputNode* in{};
  score::gfx::BackgroundNode* bg{};
  VerifyMetrics m;
  std::vector<uint8_t> rgba;

  bool open(const AJAInputSettings& s, int w, int h, score::gfx::GraphicsApi api)
  {
    in = new AJAInputNode(s);
    bg = new score::gfx::BackgroundNode();
    bg->shared_readback = std::make_shared<QRhiReadbackResult>();
    bg->setSize(QSize{w, h});
    graph = std::make_unique<score::gfx::Graph>();
    graph->addNode(in);
    graph->addNode(bg);
    graph->addEdge(
        in->output[0], bg->input[0], Process::CableType::ImmediateGlutton);
    graph->createAllRenderLists(api);
    m.startNs = nowNs();
    return bg->canRender();
  }

  // Called from the main render loop each tick (same thread as the sender).
  void renderTick()
  {
    if(!bg || !bg->canRender())
      return;
    bg->render();
    auto& rb = *bg->shared_readback;
    if(rb.data.isEmpty() || rb.pixelSize.isEmpty())
      return;
    const int w = rb.pixelSize.width(), h = rb.pixelSize.height();
    const auto* px = reinterpret_cast<const uint8_t*>(rb.data.constData());
    const int idx = idxFromRgba(px, w, h);
    if(m.recordIndex(idx, nowNs()))
      m.recordPsnr(px, w, h, idx);
  }

  void stop()
  {
    graph.reset();
    delete bg;
    bg = nullptr;
    delete in;
    in = nullptr;
  }
};

// ---------------------------------------------------------------------------
// One round-trip cell.
// ---------------------------------------------------------------------------
Result runCell(const Options& opt, const VFmt& vf, const PFmt& pf,
               const std::string& interop)
{
  Result r;
  r.videoFormat = vf.name;
  r.pixelFormat = pf.outName;
  r.interop = interop;

  // Output interop selection (validated per fallback). "cpu" forces the
  // encoder + CPU-staging path; "dvp"/"rdma" force that GPU-direct path
  // (falling back to CPU staging if it can't init — the strategy column
  // reports what actually engaged).
  if(interop == "cpu")
    qunsetenv("SCORE_AJA_FORCE_INTEROP");
  else
    qputenv("SCORE_AJA_FORCE_INTEROP", QByteArray::fromStdString(interop));

  // --- Receiver (card B) ---
  AJAInputSettings inS;
  inS.deviceIndex = opt.inDevice;
  inS.channelIndex = opt.inChannel;
  inS.videoFormat = vf.name;
  inS.videoFormatEnum = vf.fmt; // configured-format fallback for any NTV2 format
  inS.pixelFormat = pf.in;
  inS.resolutionMode = vf.eightK ? AJAInputResolutionMode::Quad8K
                       : vf.quad ? AJAInputResolutionMode::Quad4K
                                 : AJAInputResolutionMode::SingleLink;
  // Match the output's 8K routing topology. TSI = single-raster over 2x12G;
  // squares (SQD) = 4 independent 4K quadrants over 4x12G.
  const bool eightKSquares = (opt.eightKMode == "squares" || opt.eightKMode == "sqd");
  if(vf.eightK)
    inS.routingMode
        = eightKSquares ? AJAInputRoutingMode::SQD : AJAInputRoutingMode::TSI;
  // "none" = TX-only: no receiver graph at all, so the render loop drives the
  // sender alone. Isolates the product's true output cadence (measure the
  // card's own acFramesProcessed via SCORE_AJA_ACSTATS) from the harness's
  // single-thread render+readback+verify coupling.
  const bool txOnly = (opt.rxMode == "none");
  const bool gpuRx = (opt.rxMode == "gpu");
  inS.useRDMA = gpuRx; // GPU-direct capture node vs CPU-staging capture

  Receiver cpuRcv;
  GpuReceiver gpuRcv;

  // --- Sender (card A): TexgenNode -> AJANode ---
  AJAOutputSettings outS;
  outS.deviceIndex = opt.outDevice;
  outS.channelIndex = opt.outChannel;
  outS.width = vf.w;
  outS.height = vf.h;
  outS.rate = vf.rate;
  outS.videoFormat = vf.name;
  outS.videoFormatEnum = vf.fmt; // drive the format directly by NTV2 enum
  outS.pixelFormat = pf.outName;
  outS.useRDMA = (interop != "cpu");
  // 8K/UHD2 routing: TSI (2-sample interleave, single 8K raster matching the
  // encoder's single readback) or Squares (2x2 quadrant grid). Input routing
  // is matched above.
  outS.mode8K = vf.eightK ? (eightKSquares ? AJA8KMode::Squares : AJA8KMode::TSI)
                          : AJA8KMode::Disabled;
  outS.hdrMode = opt.hdr;

  // Test source. Default: GPU-generated pattern (fragment pass driven by
  // the frame-index uniform) — the producer stays shader-based at every
  // resolution. --texgen cpu keeps the legacy CPU paint+upload path for
  // A/B comparison; both produce bit-identical pixels.
  score::gfx::NodeModel* src{};
  score::gfx::Port* srcOut{};
  if(opt.texgen == "cpu")
  {
    auto* n = new score::gfx::TexgenNode;
    n->function = &g_paint;
    src = n;
    srcOut = n->output[0];
  }
  else
  {
    auto* n = new score::gfx::ShaderTexgenNode;
    n->onFrame
        = [](int idx) { g_sendNs[idx].store(nowNs(), std::memory_order_relaxed); };
    src = n;
    srcOut = n->output[0];
  }
  auto* out = new AJANode(outS);

  auto graph = std::make_unique<score::gfx::Graph>();
  graph->addNode(src);
  graph->addNode(out);
  graph->addEdge(srcOut, out->input[0], Process::CableType::ImmediateGlutton);
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

  // Open the receiver only after the sender is emitting, so the SDI signal is
  // present when capture probes the link.
  const std::string dump
      = opt.dumpPrefix.empty()
            ? std::string{}
            : opt.dumpPrefix + "_" + vf.name + "_" + pf.outName + "_" + interop;
  // Pre-size the receiver's perf sample buffers BEFORE its thread starts, so
  // recordIndex() never reallocates (and never races a reserve) on the hot path.
  const int estFrames = int(vf.rate * opt.seconds) + 64;
  if(txOnly)
  {
    // no receiver: the render loop feeds the output card alone
  }
  else if(gpuRx)
  {
    if(!gpuRcv.open(inS, vf.w, vf.h, opt.api))
      r.status = "SKIP(in-open)";
    else
    {
      gpuRcv.m.dumpPrefix = dump;
      gpuRcv.m.reserveSamples(estFrames);
    }
  }
  else if(!cpuRcv.open(inS))
  {
    r.status = "SKIP(in-open)";
  }
  else
  {
    cpuRcv.m.dumpPrefix = dump;
    cpuRcv.m.reserveSamples(estFrames);
    cpuRcv.start();
  }

  // Render must be driven FROM the Qt event loop (so QRhi frame-retirement /
  // deleteLater run between frames); calling render() in a tight loop stalls
  // beginOffscreenFrame. So tick render on a QTimer and pump events until the
  // duration elapses. The consumer thread paces the actual SDI wire on VBI.
  // Producer-side per-frame CPU cost: time each out->render() call. This is
  // the direct measure of the output encode + staging path (what the perf
  // fixes target). Pre-sized so timing never allocates on the tick.
  Stat renderMs;
  renderMs.reserve(int(vf.rate * opt.seconds) + 64);

  // Absolute-deadline pacing: a naive `start(int(1000/rate))` truncates the
  // period (59.94 -> 16 ms = 62.5 Hz ceiling) and drifts under event-loop
  // load (measured 53..63 effective fps at a 59.94/60 target). Poll at 2 ms
  // against a steady_clock schedule instead: self-correcting, no bursts
  // (resync when >2 periods behind), and the pump re-paces the wire on VBI
  // regardless.
  const double periodNs = 1e9 / vf.rate;
  int64_t nextDeadline = nowNs() + int64_t(periodNs);
  QTimer render;
  render.setTimerType(Qt::PreciseTimer);
  QObject::connect(&render, &QTimer::timeout, [&] {
    const int64_t now = nowNs();
    if(now < nextDeadline)
      return;
    if(now - nextDeadline > 2 * int64_t(periodNs))
      nextDeadline = now; // fell far behind (init hiccup): resync, don't burst
    nextDeadline += int64_t(periodNs);
    const int64_t t0 = nowNs();
    out->render();
    renderMs.add((nowNs() - t0) / 1e6);
    if(gpuRx)
      gpuRcv.renderTick(); // capture readback + verify (main thread)
  });
  render.start(2);

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
  if(txOnly)
    ; // no receiver to stop
  else if(gpuRx)
    gpuRcv.stop();
  else
    cpuRcv.stop();

  // --- Collect metrics ---
  VerifyMetrics& M = gpuRx ? gpuRcv.m : cpuRcv.m;
  r.recv = M.frames.load();
  r.gaps = M.gaps.load();
  r.repeats = M.repeats.load();
  r.fps = r.recv / opt.seconds;
  r.targetFps = vf.rate;
  const Summary lat = M.latency.summarize();
  const Summary itv = M.interval.summarize();
  const Summary rnd = renderMs.summarize();
  r.meanLatencyMs = lat.mean;
  r.latP95Ms = lat.p95;
  r.latMaxMs = lat.max;
  r.jitterMs = itv.stddev;       // pacing jitter = inter-arrival stddev
  r.maxIntervalMs = itv.max;     // worst pacing hiccup
  r.renderMeanMs = rnd.mean;
  r.renderP95Ms = rnd.p95;
  r.renderMaxMs = rnd.max;
  r.meanPsnr = M.psnrCount.load() > 0 ? M.psnrSum.load() / M.psnrCount.load() : 0;
  r.minPsnr = M.psnrCount.load() > 0 ? M.psnrMin.load() : 0;
  r.txGood = out->pacingGoodXfers();
  r.txDrops = out->pacingDrops();
  r.txUnderruns = out->pacingUnderruns();
  r.sent = int(r.txGood);

  if(r.status.empty() && txOnly)
  {
    // TX-only: the deliverable is the sender's own cadence (r.txGood +
    // SCORE_AJA_ACSTATS output counters); no receiver, so no PSNR.
    r.status = "TXONLY";
  }
  if(r.status.empty())
  {
    if(opt.hdr != AJAHDRMode::Off)
    {
      // HDR: PSNR doesn't apply (BT.2020/PQ vs the SDR reference). Validate
      // that frames flowed AND the input detected the HDR transfer the output
      // signaled over VPID.
      const int trc = M.lastTrc.load();
      std::printf(
          "    HDR: detected color_trc=%d, expected=%d\n", trc,
          expectedTrc(opt.hdr));
      if(M.frames.load() == 0)
        r.status = "SKIP(no-lock)";
      else if(trc == expectedTrc(opt.hdr))
        r.status = "PASS(hdr)";
      else
        r.status = "FAIL(hdr-trc)";
    }
    else if(!gpuRx && cpuRcv.unsupportedFmt && M.psnrCount.load() == 0)
      r.status = "FAIL(decode)";
    else if(M.psnrCount.load() == 0)
      r.status = "SKIP(no-lock)";
    else if(r.minPsnr < pf.psnrThreshold)
      r.status = "FAIL(psnr)";
    else
      r.status = "PASS";
  }

  // Teardown: graph dtor releases render lists (which reference the nodes)
  // before we delete the nodes.
  graph.reset();
  delete out;
  delete src;
  return r;
}

void printMatrix(const std::vector<Result>& rows)
{
  std::printf("\n%-12s %-8s %-5s %-13s %6s %6s %6s %5s %5s %5s %8s %8s %-14s\n",
              "format", "pixfmt", "iop", "strategy", "sent", "recv", "fps",
              "txdrp", "lost", "rep", "lat(ms)", "minPSNR", "status");
  std::printf("%s\n", std::string(120, '-').c_str());
  for(const auto& r : rows)
    std::printf(
        "%-12s %-8s %-5s %-13s %6d %6d %6.1f %5llu %5d %5d %8.2f %8.2f %-14s\n",
        r.videoFormat.c_str(), r.pixelFormat.c_str(), r.interop.c_str(),
        r.strategy.c_str(), r.sent, r.recv, r.fps, (unsigned long long)r.txDrops,
        r.gaps, r.repeats, r.meanLatencyMs, r.minPsnr, r.status.c_str());
}

// Performance detail: producer render() cost, end-to-end latency distribution,
// and receiver pacing. render(ms) is the per-frame CPU time of the output
// encode+staging path; jitter is the stddev of inter-arrival spacing.
void printPerf(const std::vector<Result>& rows)
{
  std::printf(
      "\nPerformance (render = producer per-frame CPU; lat = end-to-end; "
      "jit = pacing stddev)\n");
  std::printf("%-12s %-8s %-5s %6s %6s | %7s %7s %7s | %7s %7s %7s | %7s %7s\n",
              "format", "pixfmt", "iop", "fps", "tgtfps", "rndMean", "rndP95",
              "rndMax", "latMean", "latP95", "latMax", "jit", "maxItv");
  std::printf("%s\n", std::string(110, '-').c_str());
  for(const auto& r : rows)
  {
    // Skip rows that never ran (SKIP before any frame).
    if(r.recv == 0 && r.renderMeanMs == 0)
      continue;
    std::printf(
        "%-12s %-8s %-5s %6.1f %6.1f | %7.3f %7.3f %7.3f | %7.2f %7.2f %7.2f | "
        "%7.3f %7.2f\n",
        r.videoFormat.c_str(), r.pixelFormat.c_str(), r.interop.c_str(), r.fps,
        r.targetFps, r.renderMeanMs, r.renderP95Ms, r.renderMaxMs,
        r.meanLatencyMs, r.latP95Ms, r.latMaxMs, r.jitterMs, r.maxIntervalMs);
  }
}

int runSweep(const Options& opt)
{
  NTV2DeviceID idOut{}, idIn{};
  std::set<NTV2FrameBufferFormat> outFbfs; // live firmware-advertised FBFs
  if(!probeDevice(opt.outDevice, idOut, &outFbfs))
  {
    std::printf("ERROR: cannot open output card %d\n", opt.outDevice);
    return 2;
  }
  if(!probeDevice(opt.inDevice, idIn))
  {
    std::printf("ERROR: cannot open input card %d\n", opt.inDevice);
    return 2;
  }
  std::printf("Output card %d: %s\n", opt.outDevice, ::NTV2DeviceIDToString(idOut).c_str());
  std::printf("Input  card %d: %s\n", opt.inDevice, ::NTV2DeviceIDToString(idIn).c_str());
  std::printf("Render backend: %s\n", apiName(opt.api));

  // Firmware-supported formats = table ∩ CanDoVideoFormat(both cards).
  std::vector<VFmt> formats;
  // --all-formats: every firmware-supported NTV2 format (already filtered);
  // otherwise the curated table filtered by per-card firmware support.
  const std::vector<VFmt>& table
      = opt.allFormats ? enumerateFirmwareFormats(idOut, idIn) : videoFormatTable();
  for(const auto& vf : table)
  {
    const bool ok = opt.allFormats
                    || (::NTV2DeviceCanDoVideoFormat(idOut, vf.fmt)
                        && ::NTV2DeviceCanDoVideoFormat(idIn, vf.fmt));
    const bool selected
        = opt.formats.empty()
          || std::find(opt.formats.begin(), opt.formats.end(), vf.name) != opt.formats.end();
    if(ok && selected)
      formats.push_back(vf);
  }

  if(opt.listOnly)
  {
    std::printf("\nFirmware-supported formats (both cards):\n");
    for(const auto& vf : formats)
      std::printf("  %-12s %dx%d @ %.2f%s\n", vf.name, vf.w, vf.h, vf.rate,
                  vf.quad ? " (quad-link)" : "");
    std::printf("\nPixel formats:\n");
    for(const auto& pf : pixelFormatTable())
      std::printf("  %-8s %s\n", pf.outName, pf.cpuCapture ? "(cpu+gpu rx)" : "(gpu rx only)");
    std::printf("\nFramebuffer formats the OUTPUT card's firmware advertises:\n");
    for(const auto fbf : outFbfs)
      std::printf("  %s\n", ::NTV2FrameBufferFormatToString(fbf, true).c_str());
    return 0;
  }

  std::vector<Result> rows;
  for(const auto& vf : formats)
    for(const auto& pf : pixelFormatTable())
    {
      if(!opt.pixfmts.empty()
         && std::find(opt.pixfmts.begin(), opt.pixfmts.end(), pf.outName) == opt.pixfmts.end())
        continue;
      // Card-capability gate: a framebuffer format the firmware doesn't
      // advertise would be rejected by the output node (or silently emit a
      // frozen/garbage SDI signal), so skip it cleanly. Uses the live
      // firmware capability — what the node actually enforces — not the static
      // SDK table (which e.g. lists no device for the planar PL2/PL3 formats).
      if(outFbfs.find(pf.fbf) == outFbfs.end())
      {
        Result r;
        r.videoFormat = vf.name; r.pixelFormat = pf.outName;
        r.interop = "-";
        r.status = "SKIP(fbf-unsupported)";
        rows.push_back(r);
        continue;
      }
      // Single-link >=6G UHD needs 12G crosspoints. Without CanDo12gRouting
      // the TSI quad fallback mislabels its links (ST425-1 VPIDs) and the
      // link-grouped 12G mux never engages from user space (PHY stays 3G
      // while inserting an ST2081-10 VPID) — driver/FW 18.0.0, see
      // aja-vpid-regression-report.md. Not roundtrippable; skip honestly.
      if(NTV2_IS_4K_VIDEO_FORMAT(vf.fmt) && !NTV2_IS_QUAD_QUAD_FORMAT(vf.fmt)
         && vf.rate > 31.0 && !g_canDo12gRouting)
      {
        Result r;
        r.videoFormat = vf.name; r.pixelFormat = pf.outName;
        r.interop = "-";
        r.status = "SKIP(no-12g-routing)";
        rows.push_back(r);
        continue;
      }
      // The framestore may accept the layout while the playout path can't
      // raster it (plays black) — capture/codec-only formats.
      if(!Gfx::AJA::ajaFbfPlayoutCapable(pf.fbf))
      {
        Result r;
        r.videoFormat = vf.name; r.pixelFormat = pf.outName;
        r.interop = "-";
        r.status = "SKIP(fbf-no-playout)";
        rows.push_back(r);
        continue;
      }
      if(!pf.cpuCapture)
      {
        Result r;
        r.videoFormat = vf.name; r.pixelFormat = pf.outName;
        r.interop = "-";
        r.status = "SKIP(gpu-rx-only)";
        rows.push_back(r);
        continue;
      }
      for(const auto& interop : opt.interops)
      {
        std::printf("[ %-12s %-8s %-4s ] running %.1fs ...\n", vf.name,
                    pf.outName, interop.c_str(), opt.seconds);
        std::fflush(stdout);
        rows.push_back(runCell(opt, vf, pf, interop));
      }
    }

  printMatrix(rows);
  printPerf(rows);
  const bool anyFail = std::any_of(rows.begin(), rows.end(),
                                   [](const Result& r) { return r.status.rfind("FAIL", 0) == 0; });
  return anyFail ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Card-free upload microbenchmark: per-frame latency to land one captured UYVY
// frame fully into the decoder's input texture (to GPU completion), for the
// raw-GL path (glTexSubImage2D) vs the portable QRhi path (uploadTexture), at
// 1080p / 4K / 8K. Both paths complete to the GPU each iteration (raw:
// glFinish; portable: endOffscreenFrame), so the comparison is apples-to-apples
// and the delta isolates the portable path's overhead — chiefly its extra
// full-frame memcpy (client -> QRhi staging -> texture, vs the raw path's
// single driver copy), which scales with frame size. Since score's capture
// pipeline reads back every frame (a per-frame sync point), this latency is a
// good proxy for achievable fps.
// ---------------------------------------------------------------------------
int runUploadBench(score::gfx::GraphicsApi api)
{
  struct Res { const char* name; int w, h; };
  // UYVY 4:2:2 -> packed RGBA8 texture is (w/2 x h); frame bytes = w*h*2.
  const Res reses[] = {{"1080p", 1920, 1080}, {"2160p/4K", 3840, 2160},
                       {"4320p/8K", 7680, 4320}};
  constexpr int kWarm = 30, kIters = 300;

  std::printf(
      "\nUpload microbenchmark — per-frame latency to GPU completion, %d "
      "iters\n", kIters);
  std::printf(
      "%-10s %9s | %12s %14s %12s\n", "res", "frameMB", "raw-GL us",
      "portable us", "delta us");
  std::printf("%s\n", std::string(64, '-').c_str());

  int rc = 0;
  for(const auto& r : reses)
  {
    const int texW = r.w / 2, texH = r.h;
    const std::uint32_t frameBytes = std::uint32_t(r.w) * r.h * 2u;
    const double frameMB = frameBytes / (1024.0 * 1024.0);

    auto rs = score::gfx::createRenderState(api, QSize(texW, texH), nullptr);
    if(!rs || !rs->rhi)
    {
      std::printf("%-10s   (no QRhi)\n", r.name);
      rc = 1;
      continue;
    }
    auto& rhi = *rs->rhi;
    auto* tex = rhi.newTexture(QRhiTexture::RGBA8, {texW, texH}, 1, {});
    if(!tex->create())
    {
      std::printf("%-10s   (texture create failed)\n", r.name);
      delete tex;
      rc = 1;
      continue;
    }
    std::vector<uint8_t> buf(frameBytes, 0x80);

    // --- Portable QRhi path: uploadTexture into a batch, applied in an
    //     offscreen frame (endOffscreenFrame flushes; no GPU wait beyond that).
    auto portableOnce = [&] {
      auto* batch = rhi.nextResourceUpdateBatch();
      QRhiTextureSubresourceUploadDescription sub(buf.data(), frameBytes);
      sub.setDataStride(static_cast<quint32>(texW) * 4u);
      batch->uploadTexture(tex, QRhiTextureUploadDescription{{0, 0, sub}});
      QRhiCommandBuffer* cb = nullptr;
      if(rhi.beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess)
      {
        cb->resourceUpdate(batch);
        rhi.endOffscreenFrame();
      }
    };
    for(int i = 0; i < kWarm; ++i) portableOnce();
    QElapsedTimer t;
    t.start();
    for(int i = 0; i < kIters; ++i) portableOnce();
    const double portUs = t.nsecsElapsed() / 1000.0 / kIters;

    // --- Raw-GL path: glTexSubImage2D straight from client memory (one driver
    //     copy). Only meaningful on the GL backend.
    double rawUs = -1.0;
#if QT_CONFIG(opengl)
    if(rhi.backend() == QRhi::OpenGLES2)
    {
      auto* native
          = static_cast<const QRhiGles2NativeHandles*>(rhi.nativeHandles());
      if(native && native->context && rs->surface)
      {
        auto* ctx = native->context;
        ctx->makeCurrent(rs->surface);
        auto* f = ctx->extraFunctions();
        const auto nt = tex->nativeTexture();
        const GLuint glTex = static_cast<GLuint>(nt.object);
        auto rawOnce = [&] {
          f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
          f->glBindTexture(GL_TEXTURE_2D, glTex);
          f->glTexSubImage2D(
              GL_TEXTURE_2D, 0, 0, 0, texW, texH, GL_RGBA, GL_UNSIGNED_BYTE,
              buf.data());
          f->glBindTexture(GL_TEXTURE_2D, 0);
          f->glFinish(); // complete to GPU, matching endOffscreenFrame
        };
        for(int i = 0; i < kWarm; ++i) rawOnce();
        QElapsedTimer t2;
        t2.start();
        for(int i = 0; i < kIters; ++i) rawOnce();
        rawUs = t2.nsecsElapsed() / 1000.0 / kIters;
        ctx->doneCurrent();
      }
    }
#endif

    if(rawUs >= 0)
      std::printf(
          "%-10s %9.2f | %12.1f %14.1f %12.1f\n", r.name, frameMB, rawUs,
          portUs, portUs - rawUs);
    else
      std::printf(
          "%-10s %9.2f | %12s %14.1f %12s\n", r.name, frameMB, "n/a (non-GL)",
          portUs, "—");

    tex->destroy();
    delete tex;
  }
  std::fflush(stdout);
  return rc;
}

// ---------------------------------------------------------------------------
// Vulkan zero-copy capture foundation probe. Validates the export-allocate ->
// FD -> CUDA-import chain that CaptureInteropVulkanTier3 needs, on this exact
// driver/Qt, WITHOUT any AJA card or nvidia-peermem (only the final AJA
// DMABufferLock(inRDMA=true) pin needs peermem; everything up to it is pure
// Vulkan<->CUDA interop). If this passes, the strategy's machinery is sound;
// the remaining step is hardware-gated.
// ---------------------------------------------------------------------------
#if defined(AJA_HAS_VK_INTEROP_PROBE)
// Design-B core validation: map an exportable VkImage as a CUDA array and run
// the per-frame buffer->array copy the strategy uses, seeding the source buffer
// with a gradient. Validates createExportableImage + cuda_p2p_import_vulkan_image
// + cuda_p2p_upload_buffer + cuda_p2p_copy_buffer_to_array on the real driver
// (no AJA / peermem needed). Returns 0 on success.
int runVkImageCopyPhase(
    score::gfx::vkinterop::VulkanCtx& vk, CudaP2PContextHandle cudaCtx,
    void* srcBufferPtr, std::uint32_t texW, std::uint32_t texH)
{
  namespace vki = score::gfx::vkinterop;
  auto fail = [](const char* s) {
    std::printf("  [FAIL] %s\n", s);
    return 1;
  };

  vki::ExternalImageDesc desc{};
  desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  desc.extent = {texW, texH, 1};
  desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT;
  desc.tiling = VK_IMAGE_TILING_OPTIMAL;
  desc.handleType = vki::kOpaqueHandleType;
  desc.dedicated = true;
  auto img = vki::createExportableImage(vk, desc);
  if(!img)
    return fail("createExportableImage");
  std::printf("  [ ok ] createExportableImage (%ux%u RGBA8)\n", texW, texH);

  int rc = 0;
  auto imgHandle = vki::exportMemoryHandle(vk, img->memory, vki::kOpaqueHandleType);
  if(!imgHandle || !imgHandle->isValid())
  {
    rc = fail("exportMemoryHandle(image)");
  }
  else
  {
    CudaP2PImageDesc cdesc{texW, texH, 1, 4, CUDA_P2P_FORMAT_UNSIGNED_INT8, 0};
    void* cuArray = nullptr;
    CudaP2PImageHandle imgH{};
    const auto ie = cuda_p2p_import_vulkan_image(
        cudaCtx, imgHandle->osHandle(), img->size, &cdesc, 0, &cuArray, &imgH);
    if(ie != CUDA_P2P_SUCCESS || !cuArray)
    {
      std::printf(
          "  [FAIL] cuda_p2p_import_vulkan_image: %s\n",
          cuda_p2p_get_error_string(cudaCtx));
      rc = 1;
    }
    else
    {
      std::printf("  [ ok ] cuda_p2p_import_vulkan_image -> CUarray\n");
      // Seed the source buffer with an RGBA gradient, then run the copy.
      std::vector<uint8_t> grad(std::size_t(texW) * texH * 4);
      for(std::uint32_t y = 0; y < texH; ++y)
        for(std::uint32_t x = 0; x < texW; ++x)
        {
          uint8_t* p = &grad[(std::size_t(y) * texW + x) * 4];
          p[0] = uint8_t(x * 255 / texW);
          p[1] = uint8_t(y * 255 / texH);
          p[2] = 0x40;
          p[3] = 0xFF;
        }
      if(cuda_p2p_upload_buffer(cudaCtx, srcBufferPtr, grad.data(), grad.size())
         != CUDA_P2P_SUCCESS)
        rc = fail("cuda_p2p_upload_buffer");
      else if(cuda_p2p_copy_buffer_to_array(
                  cudaCtx, srcBufferPtr, cuArray, texW * 4, texH, texW * 4)
              != CUDA_P2P_SUCCESS)
        rc = fail("cuda_p2p_copy_buffer_to_array");
      else
        std::printf(
            "  [ ok ] cuda_p2p_copy_buffer_to_array (buffer -> image)\n");
      cuda_p2p_release_image(cudaCtx, imgH);
    }
  }
  vki::destroyExternal(vk, *img);
  return rc;
}
#endif

int runVkInteropProbe()
{
#if defined(AJA_HAS_VK_INTEROP_PROBE)
  using namespace score::gfx;
  std::printf(
      "\nVulkan<->CUDA external-memory probe (zero-copy capture foundation)\n");

  const std::uint32_t frameBytes = 1920u * 1080u * 2u; // 1080p UYVY
  auto fail = [](const char* step) {
    std::printf("  [FAIL] %s\n", step);
    return 1;
  };

  auto rs = createRenderState(GraphicsApi::Vulkan, QSize(960, 1080), nullptr);
  if(!rs || !rs->rhi)
    return fail("createRenderState(Vulkan)");
  auto& rhi = *rs->rhi;
  if(rhi.backend() != QRhi::Vulkan)
    return fail("backend is not Vulkan");

  auto* h = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
  if(!h || !h->dev || !h->physDev)
    return fail("no Vulkan native device handles");
  // score always builds its Vulkan QRhi from staticVulkanInstance().
  QVulkanInstance* qInst = staticVulkanInstance(false);
  if(!qInst)
    return fail("no QVulkanInstance");
  std::printf("  [ ok ] Vulkan device + instance\n");

  vkinterop::VulkanCtx vk{qInst->vkInstance(), h->physDev, h->dev, qInst};

  // 1. Allocate an exportable device-local VkBuffer (the AJA P2P DMA target).
  vkinterop::ExternalBufferDesc desc{
      frameBytes,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      vkinterop::kOpaqueHandleType, /*dedicated=*/true};
  auto buf = vkinterop::createExportableBuffer(vk, desc);
  if(!buf)
    return fail("createExportableBuffer");
  std::printf("  [ ok ] createExportableBuffer (%u bytes)\n", frameBytes);

  int rc = 0;
  // 2. Export the memory as an opaque FD.
  auto handle
      = vkinterop::exportMemoryHandle(vk, buf->memory, vkinterop::kOpaqueHandleType);
  if(!handle || !handle->isValid())
  {
    rc = fail("exportMemoryHandle");
  }
  else
  {
    std::printf("  [ ok ] exportMemoryHandle (handle=%p)\n", handle->osHandle());

    // 3. Import the FD into CUDA -> flat device pointer (what AJA would pin).
    if(!cuda_p2p_available())
    {
      std::printf("  [SKIP] CUDA not available on this host\n");
    }
    else
    {
      CudaP2PContextHandle cudaCtx{};
      if(cuda_p2p_init(&cudaCtx) != CUDA_P2P_SUCCESS || !cudaCtx)
      {
        rc = fail("cuda_p2p_init");
      }
      else
      {
        void* gpuPtr = nullptr;
        CudaP2PResourceHandle res{};
        const auto ie = cuda_p2p_import_vulkan_buffer(
            cudaCtx, handle->osHandle(), frameBytes, &gpuPtr, &res);
        if(ie != CUDA_P2P_SUCCESS || !gpuPtr)
        {
          std::printf(
              "  [FAIL] cuda_p2p_import_vulkan_buffer: %s\n",
              cuda_p2p_get_error_string(cudaCtx));
          rc = 1;
        }
        else
        {
          std::printf(
              "  [ ok ] cuda_p2p_import_vulkan_buffer -> CUdeviceptr %p\n",
              gpuPtr);

          // --- Design B core: map a VkImage as a CUDA array and run the
          //     per-frame buffer->array copy (the strategy's hot path). The
          //     packed UYVY texture is 960x1080 RGBA8 = the same byte count as
          //     the buffer (1920x1080x2), so we reuse the same buffer.
          const std::uint32_t texW = 960, texH = 1080;
          rc = runVkImageCopyPhase(vk, cudaCtx, gpuPtr, texW, texH);
          if(rc == 0)
            std::printf(
                "\n  RESULT: Vulkan<->CUDA zero-copy capture path (export +"
                " image-map + buffer->texture copy) WORKS on this host.\n  The"
                " only unverified step (AJA DMABufferLock inRDMA=true) needs"
                "\n  nvidia-peermem + a GPUDirect-RDMA-capable setup.\n");

          cuda_p2p_release_buffer(cudaCtx, res); // also closes the imported fd
        }
        cuda_p2p_shutdown(cudaCtx);
      }
    }
  }

  vkinterop::destroyExternal(vk, *buf);
  std::fflush(stdout);
  return rc;
#else
  std::printf("vk-interop probe: requires Vulkan + SCORE_HAS_AJA_CUDA_BRIDGE\n");
  return 1;
#endif
}

Options parseOptions()
{
  Options o;
  QCommandLineParser p;
  p.setApplicationDescription("AJA card-to-card round-trip test");
  p.addHelpOption();
  QCommandLineOption outDev("out-device", "Output card index", "n", "0");
  QCommandLineOption inDev("in-device", "Input card index", "n", "1");
  QCommandLineOption outCh(
      "out-channel", "Output SDI/FrameStore base channel (0-based)", "n", "0");
  QCommandLineOption inCh(
      "in-channel", "Input SDI/FrameStore base channel (0-based)", "n", "0");
  QCommandLineOption secs("seconds", "Seconds per cell", "s", "5");
  QCommandLineOption fmts("formats", "Comma-separated videoFormat names", "list");
  QCommandLineOption pfs("pixfmt", "Comma-separated pixelFormat names", "list");
  QCommandLineOption iop(
      "interop", "Comma list of output interop modes: cpu,dvp,rdma", "list");
  QCommandLineOption dump(
      "dump", "Save first verified frame per cell to <prefix>_*.png", "prefix");
  QCommandLineOption rx(
      "rx",
      "Receiver: cpu (AVFrame capture) | gpu (AJAInputNode readback) | none "
      "(TX-only, measures pure sender cadence)",
      "mode", "cpu");
  QCommandLineOption texgenOpt(
      "texgen", "Test source: gpu (shader pattern) | cpu (legacy CPU paint)",
      "mode", "gpu");
  QCommandLineOption apiOpt(
      "api", "Render backend: opengl | vulkan | d3d11 | d3d12 | metal", "api",
      "opengl");
  QCommandLineOption eightKModeOpt(
      "8k-mode", "8K routing topology: tsi | squares", "mode", "tsi");
  QCommandLineOption allFmt(
      "all-formats", "Sweep EVERY firmware-supported NTV2 video format");
  QCommandLineOption hdrOpt(
      "hdr", "HDR output transfer: off | hdr10 | hlg", "mode", "off");
  QCommandLineOption list("list", "Print supported matrix and exit");
  QCommandLineOption benchUp(
      "bench-upload",
      "Card-free upload microbenchmark (raw-GL vs portable QRhi) at "
      "1080p/4K/8K, then exit");
  QCommandLineOption vkProbe(
      "vk-interop-probe",
      "Card-free Vulkan<->CUDA external-memory probe (zero-copy capture "
      "foundation), then exit");
  p.addOptions(
      {outDev, inDev, outCh, inCh, secs, fmts, pfs, iop, dump, rx, texgenOpt,
       apiOpt, eightKModeOpt, allFmt, hdrOpt, list, benchUp, vkProbe});
  p.process(*qApp);

  o.outDevice = p.value(outDev).toInt();
  o.inDevice = p.value(inDev).toInt();
  o.outChannel = p.value(outCh).toInt();
  o.inChannel = p.value(inCh).toInt();
  o.seconds = p.value(secs).toDouble();
  o.listOnly = p.isSet(list);
  auto split = [](const QString& s) {
    std::vector<std::string> v;
    for(const auto& part : s.split(',', Qt::SkipEmptyParts))
      v.push_back(part.trimmed().toStdString());
    return v;
  };
  if(p.isSet(fmts))
    o.formats = split(p.value(fmts));
  if(p.isSet(pfs))
    o.pixfmts = split(p.value(pfs));
  if(p.isSet(iop))
    o.interops = split(p.value(iop));
  if(o.interops.empty())
    o.interops = {"cpu", "dvp", "rdma"};
  if(p.isSet(dump))
    o.dumpPrefix = p.value(dump).toStdString();
  o.rxMode = p.value(rx).toStdString();
  o.texgen = p.value(texgenOpt).toStdString();
  o.eightKMode = p.value(eightKModeOpt).toStdString();
  o.api = parseApi(p.value(apiOpt).toStdString());
  o.allFormats = p.isSet(allFmt);
  o.hdr = parseHdr(p.value(hdrOpt).toStdString());
  o.benchUpload = p.isSet(benchUp);
  o.vkInteropProbe = p.isSet(vkProbe);
  return o;
}

} // namespace

int main(int argc, char** argv)
{
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");

  // MinimalGUIApplication (unlike the full score::Application) never calls
  // loadResources(), so in a static build the Qt resources are never
  // registered — and the linker strips the .qrc objects since nothing
  // references them. That leaves the skin/icons/fonts missing and startup
  // aborts right after "could not open :/skin/DefaultSkin.json". Register the
  // resources that live in score_lib_base, mirroring score/app/Application.cpp.
#if defined(SCORE_STATIC_PLUGINS)
  Q_INIT_RESOURCE(score);
  Q_INIT_RESOURCE(fonts);
#endif

  // Most-minimal app: no audio/CLAP plugins (avoids the CLAP teardown crash),
  // dummy audio backend.
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_SANITIZE_SKIP_CHECKS", "1"); // no first-run modal in headless runs
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

  score::MinimalGUIApplication app(argc, argv);

  // Non-interactive: auto-dismiss any modal dialog (e.g. the package-manager
  // first-run "Download the user library?" prompt) so its nested QDialog::exec
  // can't block the sweep.
  QTimer dialogKiller;
  QObject::connect(&dialogKiller, &QTimer::timeout, [] {
    if(auto* w = QApplication::activeModalWidget())
      w->close();
  });
  dialogKiller.start(100);

  int rc = 0;
  QMetaObject::invokeMethod(
      &app,
      [&] {
        const Options opt = parseOptions();
        rc = opt.vkInteropProbe ? runVkInteropProbe()
             : opt.benchUpload  ? runUploadBench(opt.api)
                                : runSweep(opt);
        std::fflush(stdout);
        // Cards are closed by now. score's global teardown still segfaults
        // even with audio plugins disabled, which would mask the pass/fail
        // exit code — so exit cleanly with the real result code.
        std::_Exit(rc);
      },
      Qt::QueuedConnection);

  return app.exec();
}
