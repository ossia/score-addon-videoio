// Kona5 dynamic-personality inspector / switcher.
//
// Kona5 boards are "dynamic devices": the FPGA can be reloaded at runtime with
// a different bitfile, changing which video formats the card advertises. This
// is NOT a PROM flash — LoadDynamicDevice reconfigures the running FPGA and the
// board reverts to its base personality on the next power cycle, so a wrong
// choice costs a reboot, not a board.
//
// Motivation: both Kona5-8K boards here advertise only single-link UHD
// (3840x2160, up to 60p) — no quad-link 4K and no quad-quad 8K — so the
// harness's 8K routing paths have never had a format to run against. Printing
// the base ID, the current ID and the loadable list says whether 8K is a
// personality away or a hardware limit.
//
//   --list                 inspect every board, change nothing
//   --load <id> [--device N]  switch personality (default: all boards)
//
// <id> is a Kona5 variant name: 8k, 8kmk, 2x4k, retail, 3dlut.

#include <ajantv2/includes/ntv2card.h>
#include <ajantv2/includes/ntv2devicefeatures.h>
#include <ajantv2/includes/ntv2devicescanner.h>
#include <ajantv2/includes/ntv2utils.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct Variant
{
  const char* token;
  NTV2DeviceID id;
};

// The Kona5 personalities that carry different format capability. The OE1..OE8
// variants are fixed customer builds and are deliberately not offered here.
constexpr Variant kVariants[] = {
    {"retail", DEVICE_ID_KONA5},
    {"8kmk", DEVICE_ID_KONA5_8KMK},
    {"8k", DEVICE_ID_KONA5_8K},
    {"2x4k", DEVICE_ID_KONA5_2X4K},
    {"3dlut", DEVICE_ID_KONA5_3DLUT},
};

/// How many formats of each class the device advertises. This is the number
/// that actually decides whether an 8K test can run at all.
void reportFormatClasses(NTV2DeviceID id)
{
  int hd = 0, uhdSingle = 0, quad = 0, quadQuad = 0;
  for(int i = 1; i < NTV2_MAX_NUM_VIDEO_FORMATS; ++i)
  {
    const auto fmt = static_cast<NTV2VideoFormat>(i);
    if(!::NTV2DeviceCanDoVideoFormat(id, fmt))
      continue;
    if(NTV2_IS_QUAD_QUAD_FORMAT(fmt))
      ++quadQuad;
    else if(NTV2_IS_QUAD_FRAME_FORMAT(fmt))
      ++quad;
    else if(NTV2_IS_4K_VIDEO_FORMAT(fmt))
      ++uhdSingle;
    else
      ++hd;
  }
  std::printf(
      "    formats: %d SD/HD, %d UHD-single-link, %d quad-link(4K), "
      "%d quad-quad(8K)\n",
      hd, uhdSingle, quad, quadQuad);
}

/// Every quad-quad format with the geometry NTV2FormatDescriptor reports for
/// it. AJARoundtrip drops any format whose descriptor yields w<=0 or h<=0, so
/// if the descriptor is empty here that is why 8K never reaches the matrix.
void dumpQuadQuad(NTV2DeviceID id)
{
  std::printf("    quad-quad(8K) formats and their descriptor geometry:\n");
  for(int i = 1; i < NTV2_MAX_NUM_VIDEO_FORMATS; ++i)
  {
    const auto fmt = static_cast<NTV2VideoFormat>(i);
    if(!::NTV2DeviceCanDoVideoFormat(id, fmt) || !NTV2_IS_QUAD_QUAD_FORMAT(fmt))
      continue;
    NTV2FormatDescriptor fd(fmt, NTV2_FBF_8BIT_YCBCR);
    std::printf(
        "      %-28s raster %ux%u  visible-h %u  %s\n",
        ::NTV2VideoFormatToString(fmt).c_str(), fd.GetRasterWidth(),
        fd.GetFullRasterHeight(), fd.GetVisibleRasterHeight(),
        (fd.GetRasterWidth() == 0 || fd.GetVisibleRasterHeight() == 0)
            ? "<- DROPPED by the harness" : "");
  }
}

void inspect(CNTV2Card& card, unsigned index)
{
  const NTV2DeviceID cur = card.GetDeviceID();
  std::printf(
      "card %u: %s (id 0x%08x)\n", index, ::NTV2DeviceIDToString(cur).c_str(),
      unsigned(cur));
  std::printf("    dynamic device : %s\n", card.IsDynamicDevice() ? "yes" : "no");
  const NTV2DeviceID base = card.GetBaseDeviceID();
  std::printf(
      "    base id        : %s (0x%08x)\n", ::NTV2DeviceIDToString(base).c_str(),
      unsigned(base));
  reportFormatClasses(cur);
  dumpQuadQuad(cur);

  const NTV2DeviceIDList loadable = card.GetDynamicDeviceList();
  std::printf("    loadable       :");
  if(loadable.empty())
    std::printf(" (none)");
  for(const auto id : loadable)
    std::printf(" %s", ::NTV2DeviceIDToString(id).c_str());
  std::printf("\n");
}
} // namespace

int main(int argc, char** argv)
{
  bool doList = (argc <= 1);
  const char* loadTok = nullptr;
  int onlyDevice = -1;
  for(int i = 1; i < argc; ++i)
  {
    if(!std::strcmp(argv[i], "--list"))
      doList = true;
    else if(!std::strcmp(argv[i], "--load") && i + 1 < argc)
      loadTok = argv[++i];
    else if(!std::strcmp(argv[i], "--device") && i + 1 < argc)
      onlyDevice = std::atoi(argv[++i]);
  }

  CNTV2DeviceScanner scanner;
  const auto count = scanner.GetNumDevices();
  if(count == 0)
  {
    std::printf("no AJA devices\n");
    return 2;
  }

  NTV2DeviceID want = DEVICE_ID_INVALID;
  if(loadTok)
  {
    for(const auto& v : kVariants)
      if(!std::strcmp(v.token, loadTok))
        want = v.id;
    if(want == DEVICE_ID_INVALID)
    {
      std::printf("unknown personality '%s'; known:", loadTok);
      for(const auto& v : kVariants)
        std::printf(" %s", v.token);
      std::printf("\n");
      return 2;
    }
  }

  int rc = 0;
  for(unsigned i = 0; i < count; ++i)
  {
    if(onlyDevice >= 0 && int(i) != onlyDevice)
      continue;
    CNTV2Card card;
    if(!CNTV2DeviceScanner::GetDeviceAtIndex(i, card))
    {
      std::printf("card %u: open failed\n", i);
      rc = 2;
      continue;
    }
    if(doList || !loadTok)
    {
      inspect(card, i);
      continue;
    }

    if(!card.IsDynamicDevice())
    {
      std::printf("card %u: not a dynamic device; cannot switch personality\n", i);
      rc = 1;
      continue;
    }
    if(card.GetDeviceID() == want)
    {
      std::printf(
          "card %u: already %s\n", i, ::NTV2DeviceIDToString(want).c_str());
      reportFormatClasses(card.GetDeviceID());
      continue;
    }
    if(!card.CanLoadDynamicDevice(want))
    {
      std::printf(
          "card %u: %s is not loadable on this board\n", i,
          ::NTV2DeviceIDToString(want).c_str());
      rc = 1;
      continue;
    }
    std::printf(
        "card %u: loading %s ...\n", i, ::NTV2DeviceIDToString(want).c_str());
    std::fflush(stdout);
    if(!card.LoadDynamicDevice(want))
    {
      std::printf("card %u: LoadDynamicDevice FAILED\n", i);
      rc = 1;
      continue;
    }
    // Re-open: the device identity changed underneath the handle.
    CNTV2Card reopened;
    if(CNTV2DeviceScanner::GetDeviceAtIndex(i, reopened))
      inspect(reopened, i);
  }
  return rc;
}
