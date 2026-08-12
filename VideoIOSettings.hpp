#pragma once
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <QString>

#include <verdigris>

namespace Gfx::VideoIO
{

/// Compiled-in capture-card vendors. Hardcoded dispatch (no registry): the
/// unified Direct Video I/O device branches on this tag to build the right
/// vendor node/backend. Stored as an int so the settings header stays free of
/// any vendor SDK type.
enum class Vendor : int
{
  AJA = 0,
  DeckLink = 1,
  Deltacast = 2,
  Bluefish = 3,
  Magewell = 4,
  V4L2 = 5,
  /// NVIDIA Argus (Tegra camera ISP). Appended, never renumbered: these
  /// values are serialised into saved documents.
  Argus = 6
};

/// Vendor-neutral playout settings. Vendor-specific knobs are carried as plain
/// ints/strings here and translated to the vendor SDK enums at device-build time
/// (see VideoOutput.cpp), so this header never pulls in ntv2 / DeckLink headers.
struct VideoOutputSettings
{
  Vendor vendor{Vendor::AJA};
  QString deviceName;
  int deviceIndex{0};
  int channelIndex{0};
  int width{1920};
  int height{1080};
  double rate{59.94};
  QString videoFormat{"1080p5994"};
  QString pixelFormat{"YCbCr10"};
  bool useRDMA{true};
  int mode8K{0};  ///< AJA8KMode (AJA only)
  int hdrMode{0}; ///< AJAHDRMode (AJA only)
};

/// Vendor-neutral capture settings. Pixel format is a canonical token
/// (YCbCr8 / YCbCr10 / ARGB / RGBA) translated per vendor.
struct VideoInputSettings
{
  Vendor vendor{Vendor::AJA};
  QString deviceName;
  int deviceIndex{0};
  int channelIndex{0};
  QString videoFormat{"1080p5994"};
  QString pixelFormat{"YCbCr8"};
  int resolutionMode{0}; ///< AJAInputResolutionMode (AJA only)
  int routingMode{0};    ///< AJAInputRoutingMode (AJA only)
  bool useRDMA{true};
};

} // namespace Gfx::VideoIO

SCORE_SERIALIZE_DATASTREAM_DECLARE(, Gfx::VideoIO::VideoOutputSettings);
Q_DECLARE_METATYPE(Gfx::VideoIO::VideoOutputSettings)
W_REGISTER_ARGTYPE(Gfx::VideoIO::VideoOutputSettings)

SCORE_SERIALIZE_DATASTREAM_DECLARE(, Gfx::VideoIO::VideoInputSettings);
Q_DECLARE_METATYPE(Gfx::VideoIO::VideoInputSettings)
W_REGISTER_ARGTYPE(Gfx::VideoIO::VideoInputSettings)
