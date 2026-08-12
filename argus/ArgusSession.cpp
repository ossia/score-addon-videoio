#include "ArgusSession.hpp"

#include <QDebug>
#include <QFile>

#include <atomic>
#include <ctime>
#include <thread>

#if defined(SCORE_HAS_ARGUS)
#include <Argus/Argus.h>
#if __has_include(<Argus/Ext/SensorTimestampTsc.h>)
#include <Argus/Ext/SensorTimestampTsc.h>
#define SCORE_ARGUS_HAS_TSC_TIMESTAMP 1
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <argus/ArgusRuntimeInternal.hpp>
#include <nvbufsurface.h>
#endif

namespace Gfx::Argus
{

#if !defined(SCORE_HAS_ARGUS)

struct ArgusSession::Impl
{
};
ArgusSession::ArgusSession() = default;
ArgusSession::~ArgusSession() = default;
bool ArgusSession::open(const ArgusSettings&)
{
  return false;
}
void ArgusSession::close() { }
bool ArgusSession::isOpen() const noexcept
{
  return false;
}
std::uint32_t ArgusSession::width() const noexcept
{
  return 0;
}
std::uint32_t ArgusSession::height() const noexcept
{
  return 0;
}
std::uint32_t ArgusSession::frameByteSize() const noexcept
{
  return 0;
}
std::int32_t ArgusSession::resolvedSensorMode() const noexcept
{
  return -1;
}
double ArgusSession::resolvedFrameRate() const noexcept
{
  return 0.0;
}
const std::vector<ArgusSlot>& ArgusSession::slots() const noexcept
{
  static const std::vector<ArgusSlot> empty;
  return empty;
}
const std::vector<ArgusSlot>& ArgusSession::slots(std::size_t) const noexcept
{
  static const std::vector<ArgusSlot> empty;
  return empty;
}
std::size_t ArgusSession::streamCount() const noexcept
{
  return 0;
}
bool ArgusSession::mapHost()
{
  return false;
}
bool ArgusSession::start(
    std::function<void(const std::size_t*, const std::uint64_t*, std::size_t)>,
    std::function<std::uint32_t(std::size_t)>)
{
  return false;
}
void ArgusSession::stop() { }
std::uint64_t ArgusSession::capturedFrames() const noexcept
{
  return 0;
}
ArgusSession::LatencyStats ArgusSession::latency() const noexcept
{
  return {};
}
void ArgusSession::resetLatency() noexcept { }

#else

using namespace ::Argus;

namespace
{
/// One EGLDisplay for the process. Argus needs it purely to accept our
/// EGLImages; nothing renders through it, and it is required even when QRhi is
/// on Vulkan because there is no non-EGL way to hand Argus a buffer.
EGLDisplay eglDisplayForArgus()
{
  static EGLDisplay dpy = [] {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(d == EGL_NO_DISPLAY)
    {
      qWarning() << "Argus: no EGLDisplay; buffers cannot be registered";
      return EGL_NO_DISPLAY;
    }
    if(!eglInitialize(d, nullptr, nullptr))
    {
      qWarning() << "Argus: eglInitialize failed";
      return EGL_NO_DISPLAY;
    }
    return d;
  }();
  return dpy;
}

/// Write one NV12 frame to disk, plus a sidecar describing its layout.
///
/// Gated on SCORE_ARGUS_DUMP because it costs a full-frame write. It exists
/// because the render-side grab does not work on this board, so without it
/// there is no way to look at what the ISP actually produced -- and "the rung
/// engaged" is not evidence that the pixels are right.
void dumpFrame(
    const QByteArray& prefix, std::uint64_t n, void* const* planeHost,
    std::uint32_t bytes, std::uint32_t w, std::uint32_t h,
    const std::uint32_t* offsets, const std::uint32_t* pitches,
    std::uint32_t planeCount)
{
  if(!planeHost || !planeHost[0] || bytes == 0)
    return;
  const QString base
      = QString::fromUtf8(prefix) + QStringLiteral("-%1").arg(n, 3, 10, QChar('0'));
  QFile f(base + ".nv12");
  if(!f.open(QIODevice::WriteOnly))
  {
    qWarning() << "Argus: cannot write" << f.fileName();
    return;
  }
  // Each plane from its own mapping, contiguously into the file, so the result
  // is a plain NV12 image regardless of how the allocator spaced the planes.
  for(std::uint32_t i = 0; i < planeCount && i < 3; ++i)
  {
    if(!planeHost[i])
      break;
    const std::uint32_t rows = (i == 0) ? h : (h + 1) / 2;
    f.write(static_cast<const char*>(planeHost[i]), qint64(pitches[i]) * rows);
  }
  f.close();

  QFile m(base + ".txt");
  if(m.open(QIODevice::WriteOnly))
  {
    QString t = QStringLiteral("NV12 %1x%2 bytes=%3 planes=%4\n")
                    .arg(w).arg(h).arg(bytes).arg(planeCount);
    for(std::uint32_t i = 0; i < planeCount && i < 3; ++i)
      t += QStringLiteral("plane%1 offset=%2 pitch=%3\n")
               .arg(i).arg(offsets[i]).arg(pitches[i]);
    m.write(t.toUtf8());
    m.close();
  }
  qDebug() << "Argus: wrote" << f.fileName() << bytes << "bytes";
}

AeAntibandingMode toArgus(AeAntibanding v)
{
  switch(v)
  {
    case AeAntibanding::Off:  return AE_ANTIBANDING_MODE_OFF;
    case AeAntibanding::Hz50: return AE_ANTIBANDING_MODE_50HZ;
    case AeAntibanding::Hz60: return AE_ANTIBANDING_MODE_60HZ;
    case AeAntibanding::Auto:
    default:                  return AE_ANTIBANDING_MODE_AUTO;
  }
}

::Argus::AwbMode toArgus(Gfx::Argus::AwbMode v)
{
  switch(v)
  {
    case Gfx::Argus::AwbMode::Off:             return AWB_MODE_OFF;
    case Gfx::Argus::AwbMode::Incandescent:    return AWB_MODE_INCANDESCENT;
    case Gfx::Argus::AwbMode::Fluorescent:     return AWB_MODE_FLUORESCENT;
    case Gfx::Argus::AwbMode::WarmFluorescent: return AWB_MODE_WARM_FLUORESCENT;
    case Gfx::Argus::AwbMode::Daylight:        return AWB_MODE_DAYLIGHT;
    case Gfx::Argus::AwbMode::CloudyDaylight:  return AWB_MODE_CLOUDY_DAYLIGHT;
    case Gfx::Argus::AwbMode::Twilight:        return AWB_MODE_TWILIGHT;
    case Gfx::Argus::AwbMode::Shade:           return AWB_MODE_SHADE;
    case Gfx::Argus::AwbMode::Manual:          return AWB_MODE_MANUAL;
    case Gfx::Argus::AwbMode::Auto:
    default:                                   return AWB_MODE_AUTO;
  }
}

DenoiseMode toDenoise(Quality v)
{
  switch(v)
  {
    case Quality::Off:         return DENOISE_MODE_OFF;
    case Quality::HighQuality: return DENOISE_MODE_HIGH_QUALITY;
    case Quality::Fast:
    default:                   return DENOISE_MODE_FAST;
  }
}

EdgeEnhanceMode toEdge(Quality v)
{
  switch(v)
  {
    case Quality::Off:         return EDGE_ENHANCE_MODE_OFF;
    case Quality::HighQuality: return EDGE_ENHANCE_MODE_HIGH_QUALITY;
    case Quality::Fast:
    default:                   return EDGE_ENHANCE_MODE_FAST;
  }
}
}

struct ArgusSession::Impl
{
  ArgusSettings set;

  UniqueObj<CaptureSession> session;
  ICaptureSession* iSession{};
  UniqueObj<Request> request;

  struct Slot
  {
    NvBufSurface* surf{};
    void* eglImage{};
    Buffer* buffer{};
    int fd{-1};
    std::size_t index{};
    bool mapped{false};
  };

  /// One sensor's output stream and its buffer pool. A synchronised multi-sensor
  /// session has several of these under one CaptureSession and one Request, which
  /// is what makes their frames belong to the same capture; a single-sensor
  /// session is the same code with one entry.
  struct Stream
  {
    UniqueObj<OutputStream> stream;
    IBufferOutputStream* iStream{};
    std::vector<Slot> pool;
    std::vector<ArgusSlot> pub;
  };
  // Held by pointer: Argus's UniqueObj is neither copyable nor movable, so a
  // vector of Streams cannot reallocate.
  std::vector<std::unique_ptr<Stream>> streams;

  std::uint32_t w{}, h{}, frameBytes{};
  std::int32_t mode{-1};
  double rate{};

  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> frames{0};

  // Latency accumulators. Written only by the capture thread, read by anyone;
  // relaxed is enough since they are statistics, not a synchronisation signal.
  QByteArray dumpPrefix;
  std::uint64_t dumpCount{3};

  std::atomic<std::uint64_t> latCount{0}, latUnusable{0};
  std::atomic<std::uint64_t> latMin{~0ull}, latMax{0}, latSum{0};

  void destroyPool()
  {
    for(auto& stp : streams)
    {
      auto& st = *stp;
      for(auto& s : st.pool)
      {
        if(s.mapped && s.surf)
          NvBufSurfaceUnMap(s.surf, 0, 0);
        if(s.eglImage && s.surf)
          NvBufSurfaceUnMapEglImage(s.surf, 0);
        // The Argus Buffer is owned by the stream and dies with it; the surface
        // is ours.
        if(s.surf)
          NvBufSurfaceDestroy(s.surf);
        s = {};
      }
      st.pool.clear();
      st.pub.clear();
    }
    streams.clear();
  }
};

ArgusSession::ArgusSession()
    : d{std::make_unique<Impl>()}
{
}

ArgusSession::~ArgusSession()
{
  close();
}

bool ArgusSession::isOpen() const noexcept
{
  return d && !d->streams.empty() && d->streams[0]->iStream != nullptr;
}
std::uint32_t ArgusSession::width() const noexcept
{
  return d->w;
}
std::uint32_t ArgusSession::height() const noexcept
{
  return d->h;
}
std::uint32_t ArgusSession::frameByteSize() const noexcept
{
  return d->frameBytes;
}
std::int32_t ArgusSession::resolvedSensorMode() const noexcept
{
  return d->mode;
}
double ArgusSession::resolvedFrameRate() const noexcept
{
  return d->rate;
}
const std::vector<ArgusSlot>& ArgusSession::slots() const noexcept
{
  return slots(0);
}

const std::vector<ArgusSlot>& ArgusSession::slots(std::size_t stream) const noexcept
{
  static const std::vector<ArgusSlot> empty;
  return stream < d->streams.size() ? d->streams[stream]->pub : empty;
}

std::size_t ArgusSession::streamCount() const noexcept
{
  return d ? d->streams.size() : 0;
}
std::uint64_t ArgusSession::capturedFrames() const noexcept
{
  return d->frames.load(std::memory_order_acquire);
}

ArgusSession::LatencyStats ArgusSession::latency() const noexcept
{
  LatencyStats r;
  r.count = d->latCount.load(std::memory_order_relaxed);
  r.unusable = d->latUnusable.load(std::memory_order_relaxed);
  if(r.count == 0)
    return r;
  r.minNs = d->latMin.load(std::memory_order_relaxed);
  r.maxNs = d->latMax.load(std::memory_order_relaxed);
  r.meanNs = double(d->latSum.load(std::memory_order_relaxed)) / double(r.count);
  return r;
}

void ArgusSession::resetLatency() noexcept
{
  d->latCount.store(0, std::memory_order_relaxed);
  d->latUnusable.store(0, std::memory_order_relaxed);
  d->latMin.store(~0ull, std::memory_order_relaxed);
  d->latMax.store(0, std::memory_order_relaxed);
  d->latSum.store(0, std::memory_order_relaxed);
}

bool ArgusSession::open(const ArgusSettings& settings)
{
  close();
  d->set = settings;

  if(!argusAvailable())
  {
    qWarning() << "Argus: unavailable (is nvargus-daemon running?)";
    return false;
  }

  const auto& cams = argusCameras();
  if(settings.sensorId >= cams.size())
  {
    qWarning() << "Argus: sensor-id" << settings.sensorId << "but only"
               << cams.size() << "camera(s)";
    return false;
  }
  const auto& cam = cams[settings.sensorId];

  const auto modeIdx = resolveSensorMode(
      cam, settings.sensorMode, settings.width, settings.height,
      settings.frameRate);
  if(modeIdx < 0)
  {
    qWarning() << "Argus: no sensor mode satisfies" << settings.width << "x"
               << settings.height << "@" << settings.frameRate;
    return false;
  }
  const auto& mi = cam.modes[std::size_t(modeIdx)];
  d->mode = modeIdx;
  d->w = mi.width;
  d->h = mi.height;

  // Log it always. The difference between two same-resolution modes on this
  // sensor is 60 vs 30 fps, so a silent pick is indistinguishable from a
  // permanent performance bug.
  if(settings.verbose)
  {
    qDebug() << "Argus: camera" << settings.sensorId << cam.model.c_str()
             << "mode" << modeIdx << mi.width << "x" << mi.height << "@"
             << mi.maxFrameRate << "fps max," << mi.type.c_str();
  }

  // --- session + stream -----------------------------------------------------
  // Borrow the runtime's provider and device handles: only one CameraProvider
  // may exist per process, and its device order matches argusCameras().
  auto* prov = argusProviderHandle();
  const auto& devices = argusDeviceHandles();
  if(!prov || settings.sensorId >= devices.size())
  {
    qWarning() << "Argus: no device handle for sensor-id" << settings.sensorId;
    return false;
  }

  // The sensors of one rig share a single CaptureSession. That is what makes
  // their frames one capture: one AE/AWB loop, one Request, one repeat clock.
  // Two sessions would give two of each, and no amount of correlation downstream
  // can put back a synchronisation the capture never had.
  std::vector<std::uint32_t> ids = settings.sensorIds;
  if(ids.empty())
    ids.push_back(settings.sensorId);
  for(auto id : ids)
  {
    if(id >= devices.size())
    {
      qWarning() << "Argus: no device handle for sensor-id" << id;
      return false;
    }
  }

  if(ids.size() > 1)
  {
    // Must be declared before the session is created, and counts SESSIONS of
    // each kind rather than sensors.
    if(prov->setSyncSensorSessionsCount(1, 0) != STATUS_OK)
      qWarning() << "Argus: setSyncSensorSessionsCount refused; the sensors may "
                    "not be hardware-synchronised";
    std::vector<CameraDevice*> devs;
    devs.reserve(ids.size());
    for(auto id : ids)
      devs.push_back(devices[id]);
    d->session = UniqueObj<CaptureSession>(prov->createCaptureSession(devs));
  }
  else
  {
    d->session = UniqueObj<CaptureSession>(
        prov->createCaptureSession(devices[ids[0]]));
  }
  d->iSession = interface_cast<ICaptureSession>(d->session);
  if(!d->iSession)
  {
    qWarning() << "Argus: createCaptureSession failed for" << ids.size()
               << "sensor(s)";
    return false;
  }

  d->streams.reserve(ids.size());
  for(std::size_t i = 0; i < ids.size(); ++i)
    d->streams.push_back(std::make_unique<Impl::Stream>());
  for(std::size_t si = 0; si < ids.size(); ++si)
  {
    UniqueObj<OutputStreamSettings> streamSettings(
        d->iSession->createOutputStreamSettings(STREAM_TYPE_BUFFER));
    auto* iStreamSettings
        = interface_cast<IBufferOutputStreamSettings>(streamSettings);
    if(!iStreamSettings)
    {
      qWarning() << "Argus: no IBufferOutputStreamSettings";
      return false;
    }
    // BUFFER_TYPE_EGL_IMAGE is the only buffer type libargus defines, so this is
    // not a choice: it is the sole way to give it memory we own.
    iStreamSettings->setBufferType(BUFFER_TYPE_EGL_IMAGE);
    iStreamSettings->setMetadataEnable(true);
    // Which sensor feeds this stream. Without it every stream defaults to the
    // first device in the session, so a two-sensor rig would silently deliver
    // the same eye twice.
    if(auto* iOut = interface_cast<IOutputStreamSettings>(streamSettings))
      iOut->setCameraDevice(devices[ids[si]]);

    d->streams[si]->stream = UniqueObj<OutputStream>(
        d->iSession->createOutputStream(streamSettings.get()));
    d->streams[si]->iStream
        = interface_cast<IBufferOutputStream>(d->streams[si]->stream);
    if(!d->streams[si]->iStream)
    {
      qWarning() << "Argus: createOutputStream failed for sensor" << ids[si];
      return false;
    }
  }

  // --- buffer pool ----------------------------------------------------------
  EGLDisplay dpy = eglDisplayForArgus();
  if(dpy == EGL_NO_DISPLAY)
    return false;

  auto want = settings.bufferCount;
  // The borrowed contract keeps a slot from the ISP until the renderer has
  // finished with it, so the pool depth bounds throughput, not just memory.
  // Overridable so the depth can be swept without a rebuild.
  if(const auto env = qgetenv("SCORE_ARGUS_BUFFERS").toULongLong(); env >= 4)
    want = env;
  const auto n = std::max<std::size_t>(want, 4);
  // Each sensor gets its own pool: the ISP writes one frame per sensor per
  // capture, and they must land in separate memory to be sampled together.
  for(auto& stp : d->streams)
  {
    auto& st = *stp;
    st.pool.resize(n);
    st.pub.resize(n);

    for(std::size_t i = 0; i < n; ++i)
    {
      NvBufSurfaceAllocateParams p{};
      p.params.width = d->w;
      p.params.height = d->h;
      p.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
      // Pitch-linear, not block-linear: a block-linear surface is only importable
      // with the matching DRM modifier, and the zero-copy rungs take the plain
      // linear path. Cheaper to let the ISP write linear than to teach every
      // importer a vendor modifier.
      p.params.layout = NVBUF_LAYOUT_PITCH;
      p.params.memType = NVBUF_MEM_SURFACE_ARRAY;
      p.memtag = NvBufSurfaceTag_CAMERA;
      // Tight rows: the planar importers derive plane offsets assuming no row
      // padding and refuse a padded pitch, so ask for none rather than be
      // declined.
      p.disablePitchPadding = true;

      NvBufSurface* surf{};
      if(NvBufSurfaceAllocate(&surf, 1, &p) != 0 || !surf)
      {
        qWarning() << "Argus: NvBufSurfaceAllocate failed for slot" << i;
        d->destroyPool();
        return false;
      }
      surf->numFilled = 1;
      st.pool[i].surf = surf;
      st.pool[i].index = i;

      if(NvBufSurfaceMapEglImage(surf, 0) != 0
         || !surf->surfaceList[0].mappedAddr.eglImage)
      {
        qWarning() << "Argus: NvBufSurfaceMapEglImage failed for slot" << i;
        d->destroyPool();
        return false;
      }
      st.pool[i].eglImage = surf->surfaceList[0].mappedAddr.eglImage;
      st.pool[i].fd = int(surf->surfaceList[0].bufferDesc);

      UniqueObj<BufferSettings> bufSettings(st.iStream->createBufferSettings());
      auto* iBufSettings = interface_cast<IEGLImageBufferSettings>(bufSettings);
      if(!iBufSettings)
      {
        qWarning() << "Argus: no IEGLImageBufferSettings";
        d->destroyPool();
        return false;
      }
      iBufSettings->setEGLDisplay(dpy);
      iBufSettings->setEGLImage(st.pool[i].eglImage);

      Buffer* buf = st.iStream->createBuffer(bufSettings.get());
      auto* iBuf = interface_cast<IBuffer>(buf);
      if(!buf || !iBuf)
      {
        qWarning() << "Argus: createBuffer failed for slot" << i;
        d->destroyPool();
        return false;
      }
      // The slot index travels with the Buffer, so acquireBuffer can be mapped
      // back to our pool without a search.
      iBuf->setClientData(reinterpret_cast<const void*>(std::uintptr_t(i)));
      st.pool[i].buffer = buf;

      // Publish the geometry the GPU side needs, taken from NvBufSurface rather
      // than computed: it is the authority on where each plane starts.
      const auto& sp = surf->surfaceList[0].planeParams;
      auto& out = st.pub[i];
      out.dmabufFd = st.pool[i].fd;
      out.planeCount = std::min<std::uint32_t>(sp.num_planes, 3);
      std::uint32_t total = 0;
      for(std::uint32_t pl = 0; pl < out.planeCount; ++pl)
      {
        out.offset[pl] = sp.offset[pl];
        out.pitch[pl] = sp.pitch[pl];
        total = std::max(total, sp.offset[pl] + sp.psize[pl]);
      }
      out.totalBytes = total ? total : surf->surfaceList[0].dataSize;

      // Prime: a Buffer must be released before Argus may write into it.
      st.iStream->releaseBuffer(buf);
    }

  }

  static const std::vector<ArgusSlot> noSlots;
  const auto& pub0 = d->streams.empty() ? noSlots : d->streams[0]->pub;
  d->frameBytes = pub0.empty() ? 0 : pub0[0].totalBytes;

  // Report where the real layout differs from a tight one. The backend passes
  // these offsets down explicitly so the zero-copy rungs use them verbatim, so
  // this is information rather than a problem -- but it is worth saying, since
  // a producer whose planes are NOT tightly packed is exactly the case a
  // derivation gets wrong, and knowing which allocator does that is useful.
  if(d->set.verbose && !pub0.empty() && pub0[0].planeCount > 1)
  {
    const auto& s0 = pub0[0];
    std::uint32_t tight = 0;
    for(std::uint32_t pl = 0; pl < s0.planeCount; ++pl)
    {
      if(s0.offset[pl] != tight)
      {
        qDebug() << "Argus: plane" << pl << "starts at" << s0.offset[pl]
                 << "where a tight layout would put it at" << tight
                 << "-- using the reported layout";
        break;
      }
      const auto ph = (pl == 0) ? d->h : (d->h + 1) / 2;
      tight += s0.pitch[pl] * ph;
    }
  }

  // --- request --------------------------------------------------------------
  d->request = UniqueObj<Request>(d->iSession->createRequest());
  auto* iReq = interface_cast<IRequest>(d->request);
  if(!iReq)
  {
    qWarning() << "Argus: cannot build the capture request";
    d->destroyPool();
    return false;
  }
  // One Request driving every stream is the mechanism: the sensors expose
  // together because they are told to capture together, not because their
  // frames are matched up afterwards.
  for(auto& stp : d->streams)
  {
    if(iReq->enableOutputStream(stp->stream.get()) != STATUS_OK)
    {
      qWarning() << "Argus: enableOutputStream failed";
      d->destroyPool();
      return false;
    }
  }

  auto* src = interface_cast<ISourceSettings>(iReq->getSourceSettings());
  if(src)
  {
    std::vector<SensorMode*> modes;
    if(auto* props = interface_cast<ICameraProperties>(devices[settings.sensorId]))
    {
      props->getAllSensorModes(&modes);
      if(std::size_t(modeIdx) < modes.size())
        src->setSensorMode(modes[std::size_t(modeIdx)]);
    }

    // Frame duration drives the rate. Clamp to what the mode can do rather than
    // let libargus reject the whole request.
    double want = settings.frameRate > 0.0 ? settings.frameRate : mi.maxFrameRate;
    if(want > mi.maxFrameRate)
    {
      if(settings.verbose)
        qDebug() << "Argus: requested" << want << "fps clamped to the mode's"
                 << mi.maxFrameRate;
      want = mi.maxFrameRate;
    }
    d->rate = want;
    const auto dur = std::uint64_t(1e9 / want);
    src->setFrameDurationRange(::Argus::Range<std::uint64_t>(dur, dur));

    if(settings.exposureTimeNs.set)
      src->setExposureTimeRange(::Argus::Range<std::uint64_t>(
          std::uint64_t(settings.exposureTimeNs.min),
          std::uint64_t(settings.exposureTimeNs.max)));
    if(settings.gain.set)
      src->setGainRange(
          ::Argus::Range<float>(float(settings.gain.min), float(settings.gain.max)));
  }

  if(auto* ac = interface_cast<IAutoControlSettings>(iReq->getAutoControlSettings()))
  {
    ac->setAeAntibandingMode(toArgus(settings.aeAntibanding));
    ac->setAeLock(settings.aeLock);
    ac->setAwbLock(settings.awbLock);
    ac->setAwbMode(toArgus(settings.awbMode));
    ac->setExposureCompensation(settings.exposureCompensation);
    if(settings.ispDigitalGain.set)
      ac->setIspDigitalGainRange(::Argus::Range<float>(
          float(settings.ispDigitalGain.min), float(settings.ispDigitalGain.max)));
    if(settings.saturationSet)
    {
      ac->setColorSaturationEnable(true);
      ac->setColorSaturation(settings.saturation);
    }
    if(settings.aeRegion.set)
    {
      std::vector<AcRegion> r;
      r.emplace_back(
          std::uint32_t(settings.aeRegion.left), std::uint32_t(settings.aeRegion.top),
          std::uint32_t(settings.aeRegion.right),
          std::uint32_t(settings.aeRegion.bottom), settings.aeRegion.weight);
      ac->setAeRegions(r);
    }
  }

  if(auto* dn = interface_cast<IDenoiseSettings>(d->request))
  {
    dn->setDenoiseMode(toDenoise(settings.denoiseMode));
    if(settings.denoiseStrength >= 0.f)
      dn->setDenoiseStrength(settings.denoiseStrength);
  }
  if(auto* ee = interface_cast<IEdgeEnhanceSettings>(d->request))
  {
    ee->setEdgeEnhanceMode(toEdge(settings.edgeEnhanceMode));
    if(settings.edgeEnhanceStrength >= 0.f)
      ee->setEdgeEnhanceStrength(settings.edgeEnhanceStrength);
  }

  // SCORE_ARGUS_DUMP=<prefix> writes the first few frames to disk. It needs the
  // host mapping, which the zero-copy rungs otherwise never take, so it is
  // requested here rather than left to whichever rung wins.
  d->dumpPrefix = qgetenv("SCORE_ARGUS_DUMP");
  if(!d->dumpPrefix.isEmpty())
  {
    if(const auto c = qgetenv("SCORE_ARGUS_DUMP_COUNT").toULongLong(); c > 0)
      d->dumpCount = c;
    if(!mapHost())
      qWarning() << "Argus: dump requested but the surfaces would not map";
    else
      qDebug() << "Argus: dumping the first" << d->dumpCount << "frames to"
               << d->dumpPrefix;
  }

  if(settings.verbose)
    qDebug() << "Argus: opened" << d->w << "x" << d->h << "NV12," << n
             << "buffers," << d->frameBytes << "bytes/frame, mode" << d->mode
             << "@" << d->rate << "fps";
  return true;
}

bool ArgusSession::mapHost()
{
  bool all = true;
  for(auto& stp : d->streams)
  for(std::size_t i = 0; i < stp->pool.size(); ++i)
  {
    auto& st = *stp;
    auto& s = st.pool[i];
    if(!s.surf)
      continue;
    // plane = -1 maps every plane. Passing 0 maps only luma, and then
    // reading a whole frame from addr[0] runs off the end of that mapping --
    // which looks exactly like corrupt chroma.
    if(NvBufSurfaceMap(s.surf, 0, -1, NVBUF_MAP_READ) != 0)
    {
      qWarning() << "Argus: NvBufSurfaceMap failed for slot" << i;
      all = false;
      continue;
    }
    s.mapped = true;
    // Per plane: NvBufSurface maps each separately and they are not required to
    // be contiguous, so one base pointer plus an offset is not enough.
    const auto& sp = s.surf->surfaceList[0].planeParams;
    for(std::uint32_t pl = 0; pl < sp.num_planes && pl < 3; ++pl)
      st.pub[i].planeHost[pl] = s.surf->surfaceList[0].mappedAddr.addr[pl];
    st.pub[i].host = st.pub[i].planeHost[0];
  }
  return all;
}

bool ArgusSession::start(
    std::function<void(const std::size_t*, const std::uint64_t*, std::size_t)>
        onFrame,
    std::function<std::uint32_t(std::size_t)> takeReturned)
{
  if(!d->iSession || d->streams.empty() || !d->streams[0]->iStream
     || d->running.load())
    return false;

  if(d->iSession->repeat(d->request.get()) != STATUS_OK)
  {
    qWarning() << "Argus: repeat() failed";
    return false;
  }

  d->running.store(true, std::memory_order_release);
  d->thread = std::thread([this, onFrame = std::move(onFrame),
                           takeReturned = std::move(takeReturned)] {
    while(d->running.load(std::memory_order_acquire))
    {
      IBuffer* bufStream0 = nullptr;
      // One capture delivers one buffer per stream. Acquire them all before
      // publishing anything: half a capture must never be visible downstream,
      // which is the whole reason the renderer can trust the set it is given.
      const std::size_t ns = d->streams.size();
      std::size_t slotIdx[kMaxSyncSensors]{};
      std::uint64_t stamps[kMaxSyncSensors]{};
      Buffer* acquired[kMaxSyncSensors]{};
      std::size_t got = 0;
      bool endOfStream = false;
      for(std::size_t si = 0; si < ns && si < kMaxSyncSensors; ++si)
      {
        Status st = STATUS_OK;
        Buffer* b = d->streams[si]->iStream->acquireBuffer(
            d->set.acquireTimeoutNs, &st);
        if(!b || st != STATUS_OK)
        {
          // A timeout on a live sensor means the ISP stalled; keep looping so a
          // recovering sensor resumes, but do not spin on a dead stream.
          if(st == STATUS_END_OF_STREAM)
            endOfStream = true;
          break;
        }
        acquired[si] = b;
        ++got;
        auto* ib = interface_cast<IBuffer>(b);
        slotIdx[si] = ib ? std::size_t(reinterpret_cast<std::uintptr_t>(
                               ib->getClientData()))
                         : std::size_t(0);
        if(si == 0)
          bufStream0 = ib;
      }
      if(got != ns)
      {
        // Partial capture: give back what we took. Dropping them here would
        // drain the pools a few frames at a time and present as a sensor that
        // slowly stops delivering.
        for(std::size_t si = 0; si < got; ++si)
          d->streams[si]->iStream->releaseBuffer(acquired[si]);
        if(endOfStream)
          break;
        continue;
      }

      auto* iBuf = bufStream0;
      std::uint64_t sofTsc = 0;
      const auto slot = slotIdx[0];

      // Sensor start-of-frame -> here. Measured before publishing so it is the
      // same span nvarguscamerasrc's show-latency reports, and therefore
      // directly comparable to it.
      if(iBuf)
      {
        if(const auto* md = iBuf->getMetadata())
        {
          if(auto* iMd = interface_cast<const ICaptureMetadata>(md))
          {
            const auto sensorNs = iMd->getSensorTimestamp();
#if defined(SCORE_ARGUS_HAS_TSC_TIMESTAMP)
            // The VI hardware start-of-frame on the Tegra-wide TSC -- the same
            // counter the V4L2 buffer stamps and cntvct_el0 use, so capture,
            // render and any external reference compare without conversion.
            // Deliberately NOT falling back to sensorNs when this is
            // unavailable: they are different clock domains, and a skew computed
            // across the two would be a plausible-looking fiction. Zero means
            // "no stamp", which the sync group already skips.
            if(auto* iTsc = interface_cast<const Ext::ISensorTimestampTsc>(md))
              sofTsc = iTsc->getSensorSofTimestampTsc();
#endif
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const auto nowNs
                = std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
            // Argus stamps on the monotonic clock. If that ever stops being
            // true the subtraction yields nonsense, so implausible values are
            // counted rather than averaged into a confident wrong figure.
            if(sensorNs != 0 && nowNs > sensorNs && (nowNs - sensorNs) < 10'000'000'000ull)
            {
              const auto lat = nowNs - sensorNs;
              d->latSum.fetch_add(lat, std::memory_order_relaxed);
              d->latCount.fetch_add(1, std::memory_order_relaxed);
              auto prev = d->latMin.load(std::memory_order_relaxed);
              while(lat < prev
                    && !d->latMin.compare_exchange_weak(prev, lat, std::memory_order_relaxed))
                ;
              prev = d->latMax.load(std::memory_order_relaxed);
              while(lat > prev
                    && !d->latMax.compare_exchange_weak(prev, lat, std::memory_order_relaxed))
                ;
            }
            else
            {
              d->latUnusable.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }

      const auto n = d->frames.fetch_add(1, std::memory_order_release) + 1;

      // Dump before publishing: after it, the renderer owns the slot and the
      // ISP may already be refilling it.
      if(!d->dumpPrefix.isEmpty() && n <= d->dumpCount
         && slot < d->streams[0]->pub.size())
      {
        const auto& ps = d->streams[0]->pub[slot];
        dumpFrame(
            d->dumpPrefix, n, ps.planeHost, ps.totalBytes, d->w, d->h,
            ps.offset, ps.pitch, ps.planeCount);
      }

      if(onFrame)
      {
        stamps[0] = sofTsc;
        onFrame(slotIdx, stamps, ns);
      }

      // Periodic, not per-frame: at 60 fps a per-frame line is its own
      // performance problem, and the comparison against gst wants a stable
      // aggregate rather than a stream of instants.
      if(d->set.verbose && (n % 120) == 0)
      {
        const auto c = d->latCount.load(std::memory_order_relaxed);
        const auto u = d->latUnusable.load(std::memory_order_relaxed);
        if(c > 0)
        {
          qDebug(
              "Argus: latency over %llu frames: min %.2f ms  mean %.2f ms  max "
              "%.2f ms  (unusable %llu)",
              (unsigned long long)c,
              d->latMin.load(std::memory_order_relaxed) / 1e6,
              double(d->latSum.load(std::memory_order_relaxed)) / double(c) / 1e6,
              d->latMax.load(std::memory_order_relaxed) / 1e6,
              (unsigned long long)u);
        }
        else if(u > 0)
        {
          qDebug(
              "Argus: %llu frames had an unusable sensor timestamp; no latency "
              "figure",
              (unsigned long long)u);
        }
      }

      // Give back only what the renderer has finished with. Releasing the slot
      // we just published would let the ISP overwrite the frame being sampled.
      if(takeReturned)
      {
        for(std::size_t si = 0; si < ns; ++si)
        {
          auto& stm = *d->streams[si];
          std::uint32_t mask = takeReturned(si);
          for(std::size_t i = 0; mask && i < stm.pool.size(); ++i)
          {
            if(mask & (1u << i))
            {
              if(stm.pool[i].buffer)
                stm.iStream->releaseBuffer(stm.pool[i].buffer);
              mask &= ~(1u << i);
            }
          }
        }
      }
    }
  });
  return true;
}

void ArgusSession::stop()
{
  if(!d->running.exchange(false))
  {
    if(d->thread.joinable())
      d->thread.join();
    return;
  }
  for(auto& stp : d->streams)
    if(stp->iStream)
      stp->iStream->endOfStream();
  if(d->thread.joinable())
    d->thread.join();
  if(d->iSession)
  {
    d->iSession->stopRepeat();
    d->iSession->waitForIdle();
  }
}

void ArgusSession::close()
{
  if(!d)
    return;
  stop();
  d->destroyPool();
  d->request.reset();

  d->session.reset();
  d->iSession = nullptr;
  d->w = d->h = d->frameBytes = 0;
  d->mode = -1;
  d->rate = 0.0;
  d->frames.store(0);
}

#endif

}
