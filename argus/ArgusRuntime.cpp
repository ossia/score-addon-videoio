#include "ArgusRuntime.hpp"

#include <QDebug>

#include <algorithm>
#include <mutex>

#if defined(SCORE_HAS_ARGUS)
#include <Argus/Argus.h>
#endif

namespace Gfx::Argus
{

bool argusCompiledIn() noexcept
{
#if defined(SCORE_HAS_ARGUS)
  return true;
#else
  return false;
#endif
}

#if !defined(SCORE_HAS_ARGUS)

bool argusAvailable() noexcept
{
  return false;
}
const std::vector<CameraInfo>& argusCameras()
{
  static const std::vector<CameraInfo> empty;
  return empty;
}
void argusRefreshCameras() { }

#else

namespace
{
struct Provider
{
  ::Argus::UniqueObj<::Argus::CameraProvider> obj;
  ::Argus::ICameraProvider* iface{};
};

Provider& provider()
{
  static Provider p = [] {
    Provider r;
    r.obj = ::Argus::UniqueObj<::Argus::CameraProvider>(
        ::Argus::CameraProvider::create());
    r.iface = ::Argus::interface_cast<::Argus::ICameraProvider>(r.obj);
    if(!r.iface)
      qDebug() << "Argus: CameraProvider unavailable (is nvargus-daemon "
                  "running?)";
    return r;
  }();
  return p;
}

const char* modeTypeName(::Argus::SensorModeType t)
{
  if(t == ::Argus::SENSOR_MODE_TYPE_DEPTH)
    return "depth";
  if(t == ::Argus::SENSOR_MODE_TYPE_YUV)
    return "YUV";
  if(t == ::Argus::SENSOR_MODE_TYPE_RGB)
    return "RGB";
  if(t == ::Argus::SENSOR_MODE_TYPE_BAYER)
    return "Bayer";
  return "unknown";
}

std::vector<CameraInfo> g_cameras;
bool g_scanned{false};
std::mutex g_mutex;

void scanLocked()
{
  g_cameras.clear();
  g_scanned = true;

  auto* prov = provider().iface;
  if(!prov)
    return;

  std::vector<::Argus::CameraDevice*> devices;
  if(prov->getCameraDevices(&devices) != ::Argus::STATUS_OK)
  {
    qDebug() << "Argus: getCameraDevices failed";
    return;
  }

  for(std::size_t i = 0; i < devices.size(); ++i)
  {
    auto* props = ::Argus::interface_cast<::Argus::ICameraProperties>(devices[i]);
    if(!props)
      continue;

    CameraInfo cam;
    cam.index = static_cast<std::uint32_t>(i);
    cam.model = props->getModelName();

    std::vector<::Argus::SensorMode*> modes;
    // getAllSensorModes, not getBasicSensorModes: the extended list is where
    // the DOL/PWL entries live, and a sensor can expose a mode we want there.
    props->getAllSensorModes(&modes);
    for(std::size_t m = 0; m < modes.size(); ++m)
    {
      auto* sm = ::Argus::interface_cast<::Argus::ISensorMode>(modes[m]);
      if(!sm)
        continue;
      SensorModeInfo info;
      info.index = static_cast<std::uint32_t>(m);
      info.width = sm->getResolution().width();
      info.height = sm->getResolution().height();
      const auto dur = sm->getFrameDurationRange();
      info.minFrameDurationNs = dur.min();
      info.maxFrameDurationNs = dur.max();
      info.maxFrameRate = dur.min() > 0 ? 1e9 / double(dur.min()) : 0.0;
      const auto exp = sm->getExposureTimeRange();
      info.minExposureNs = exp.min();
      info.maxExposureNs = exp.max();
      const auto gain = sm->getAnalogGainRange();
      info.minGain = gain.min();
      info.maxGain = gain.max();
      info.type = modeTypeName(sm->getSensorModeType());
      cam.modes.push_back(std::move(info));
    }
    g_cameras.push_back(std::move(cam));
  }
}
}

bool argusAvailable() noexcept
{
  return provider().iface != nullptr;
}

const std::vector<CameraInfo>& argusCameras()
{
  std::lock_guard lock{g_mutex};
  if(!g_scanned)
    scanLocked();
  return g_cameras;
}

void argusRefreshCameras()
{
  std::lock_guard lock{g_mutex};
  scanLocked();
}

#endif

std::int32_t resolveSensorMode(
    const CameraInfo& cam, std::int32_t requested, std::uint32_t reqWidth,
    std::uint32_t reqHeight, double reqRate) noexcept
{
  const auto n = static_cast<std::int32_t>(cam.modes.size());
  if(n == 0)
    return -1;

  if(requested >= 0)
    return requested < n ? requested : -1;

  // Compare rates, not durations. 1e9/60 truncates to 16666666 ns while the
  // sensor reports its 60 fps mode as 16666667, so a duration comparison
  // rejects an exactly-matching mode by one nanosecond -- and the fallback is
  // a mode of the same resolution at half the rate, which looks like a
  // permanent performance bug rather than a selection error. The tolerance
  // also absorbs 59.94 vs 60 style requests.
  constexpr double kRateTolerance = 1e-3;

  std::int32_t best = -1;
  for(std::int32_t i = 0; i < n; ++i)
  {
    const auto& m = cam.modes[i];
    // The mode must cover the requested geometry and be able to run at least
    // as fast as the requested rate.
    if(m.width < reqWidth || m.height < reqHeight)
      continue;
    if(reqRate > 0.0 && m.maxFrameRate < reqRate * (1.0 - kRateTolerance))
      continue;

    if(best < 0)
    {
      best = i;
      continue;
    }

    const auto& b = cam.modes[best];
    const bool exact = (m.width == reqWidth && m.height == reqHeight);
    const bool bestExact = (b.width == reqWidth && b.height == reqHeight);

    // Three separate rules, kept separate: conflating "prefers an exact
    // resolution" with "prefers the faster mode" makes an exact match lose to
    // an oversized one purely because the oversized one was faster.
    if(exact != bestExact)
    {
      // An exact resolution always beats an oversized one.
      if(exact)
        best = i;
    }
    else if(exact)
    {
      // Both exact: take the faster, which is what the plugin does (it
      // compares minimum frame durations).
      if(m.maxFrameRate > b.maxFrameRate)
        best = i;
    }
    else
    {
      // Neither exact: the smallest qualifying resolution wins.
      if(m.width < b.width || (m.width == b.width && m.height < b.height))
        best = i;
    }
  }
  return best;
}

}
