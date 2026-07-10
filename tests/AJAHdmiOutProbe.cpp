// HDMI monitor-output validation probe for AJA Kona 5.
//
// The Kona 5 (all three personalities: retail / 8K / 12bit) has ONE HDMI 2.0
// output and NO HDMI input, so a true card-to-card HDMI pixel round-trip is
// impossible on this rig. This probe validates the deepest thing the hardware
// allows: for every HDMI-capable video format the current firmware supports,
// route framestore -> HDMI out, program the HDMI TX, and read back the live
// TX status (NTV2HDMIOutputStatus) to confirm the transmitter is enabled and
// locked to the requested standard/rate.
//
// Usage: hdmiprobe [card-index] (default: probes card 0 and card 1)

#include <ntv2card.h>
#include <ntv2devicefeatures.h>
#include <ntv2enums.h>
#include <ntv2formatdescriptor.h>
#include <ntv2utils.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
struct HFmt
{
  const char* name;
  NTV2VideoFormat fmt;
};

// HDMI 2.0-capable candidate formats (progressive + interlaced HD, UHD).
const std::vector<HFmt>& hdmiFormats()
{
  static const std::vector<HFmt> t = {
      {"625i50", NTV2_FORMAT_625_5000},
      {"525i5994", NTV2_FORMAT_525_5994},
      {"720p50", NTV2_FORMAT_720p_5000},
      {"720p5994", NTV2_FORMAT_720p_5994},
      {"720p60", NTV2_FORMAT_720p_6000},
      {"1080i25", NTV2_FORMAT_1080i_5000},
      {"1080i2997", NTV2_FORMAT_1080i_5994},
      {"1080i30", NTV2_FORMAT_1080i_6000},
      {"1080p24", NTV2_FORMAT_1080p_2400},
      {"1080p25", NTV2_FORMAT_1080p_2500},
      {"1080p2997", NTV2_FORMAT_1080p_2997},
      {"1080p30", NTV2_FORMAT_1080p_3000},
      {"1080p50", NTV2_FORMAT_1080p_5000_A},
      {"1080p5994", NTV2_FORMAT_1080p_5994_A},
      {"1080p60", NTV2_FORMAT_1080p_6000_A},
      {"2160p24", NTV2_FORMAT_3840x2160p_2400},
      {"2160p25", NTV2_FORMAT_3840x2160p_2500},
      {"2160p2997", NTV2_FORMAT_3840x2160p_2997},
      {"2160p30", NTV2_FORMAT_3840x2160p_3000},
      {"2160p50", NTV2_FORMAT_3840x2160p_5000},
      {"2160p5994", NTV2_FORMAT_3840x2160p_5994},
      {"2160p60", NTV2_FORMAT_3840x2160p_6000},
  };
  return t;
}

// Fill frame 0 with a UYVY colorbar-ish gradient so the TX carries real video.
void fillPattern(CNTV2Card& card, NTV2VideoFormat fmt)
{
  NTV2FormatDescriptor fd(fmt, NTV2_FBF_8BIT_YCBCR);
  const ULWord bytes = fd.GetTotalBytes();
  std::vector<uint8_t> buf(bytes);
  const ULWord pitch = fd.GetBytesPerRow();
  for(ULWord y = 0; y < fd.GetFullRasterHeight(); ++y)
  {
    uint8_t* row = buf.data() + size_t(y) * pitch;
    for(ULWord x = 0; x + 3 < pitch; x += 4)
    {
      row[x + 0] = uint8_t(64 + (x * 128) / pitch);  // U
      row[x + 1] = uint8_t(16 + (x * 219) / pitch);  // Y0
      row[x + 2] = uint8_t(192 - (x * 128) / pitch); // V
      row[x + 3] = uint8_t(16 + (y * 219) / fd.GetFullRasterHeight()); // Y1
    }
  }
  card.DMAWriteFrame(0, reinterpret_cast<ULWord*>(buf.data()), bytes);
}

const char* colorSpaceStr(NTV2HDMIColorSpace cs)
{
  switch(cs)
  {
    case NTV2_HDMIColorSpaceRGB: return "RGB";
    case NTV2_HDMIColorSpaceYCbCr: return "YCbCr";
    default: return "?";
  }
}

// hdmiout4 core status register (Kona5): bit29 = sink present (HPD),
// bit27 = tx lock state. From driver/ntv2hout4reg.h.
constexpr ULWord kRegHOut4VideoControl = 0x1d40;
// Global HDMI control register: bit25 = force HPD (driver treats sink as
// present, brings the TX up without EDID — for analyzers / dumb cables).
constexpr ULWord kRegHdmiControl = 127;

int probeCard(int idx, bool forceHpd)
{
  CNTV2Card card;
  if(!card.Open(UWord(idx)))
  {
    std::printf("card %d: open failed\n", idx);
    return 2;
  }
  const NTV2DeviceID id = card.GetDeviceID();
  std::printf(
      "\ncard %d: %s  (HDMI outputs: %d)%s\n", idx,
      ::NTV2DeviceIDToString(id).c_str(),
      int(card.features().GetNumHDMIVideoOutputs()),
      forceHpd ? "  [force-HPD]" : "");
  if(card.features().GetNumHDMIVideoOutputs() < 1)
  {
    std::printf("  no HDMI output on this personality — nothing to probe\n");
    return 1;
  }

  ULWord vc = 0;
  card.ReadRegister(kRegHOut4VideoControl, vc);
  std::printf(
      "  hdmiout4 videocontrol=0x%08x  sinkPresent=%u txLock=%u\n", vc,
      (vc >> 29) & 1, (vc >> 27) & 1);
  if(forceHpd)
  {
    ULWord hc = 0;
    card.ReadRegister(kRegHdmiControl, hc);
    card.WriteRegister(kRegHdmiControl, hc | (1u << 25));
  }

  std::printf(
      "  %-10s | %-9s %-22s %-12s %-6s %-6s | %s\n", "requested", "enabled",
      "tx-standard", "tx-rate", "space", "depth", "verdict");
  std::printf("  %s\n", std::string(96, '-').c_str());

  int failures = 0, tested = 0;
  for(const auto& hf : hdmiFormats())
  {
    if(!::NTV2DeviceCanDoVideoFormat(id, hf.fmt))
      continue;

    // Framestore ch1 -> HDMI out routing, display mode, pattern in frame 0.
    card.SetEveryFrameServices(NTV2_OEM_TASKS);
    card.SetVideoFormat(hf.fmt, false, false, NTV2_CHANNEL1);
    card.SetFrameBufferFormat(NTV2_CHANNEL1, NTV2_FBF_8BIT_YCBCR);
    card.SetMode(NTV2_CHANNEL1, NTV2_MODE_DISPLAY);
    card.SetOutputFrame(NTV2_CHANNEL1, 0);
    fillPattern(card, hf.fmt);
    card.Connect(NTV2_XptHDMIOutInput, NTV2_XptFrameBuffer1YUV);

    // Program the HDMI TX explicitly.
    const NTV2Standard std = ::GetNTV2StandardFromVideoFormat(hf.fmt);
    const NTV2FrameRate rate = ::GetNTV2FrameRateFromVideoFormat(hf.fmt);
    card.SetHDMIOutVideoStandard(std);
    card.SetHDMIOutVideoFPS(rate);
    card.SetHDMIOutSampleStructure(NTV2_HDMI_422);
    card.SetHDMIOutBitDepth(NTV2_HDMI10Bit);

    // Let the TX retrain / driver HDMI agent settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    NTV2HDMIOutputStatus st;
    const bool got = card.GetHDMIOutStatus(st);
    ULWord vcNow = 0;
    card.ReadRegister(kRegHOut4VideoControl, vcNow);
    ++tested;

    const std::string txStd = got ? ::NTV2StandardToString(st.mVideoStandard, true) : "?";
    const std::string txRate = got ? ::NTV2FrameRateToString(st.mVideoRate, true) : "?";
    // The driver reports HFR UHD/4K as the plain standard + rate; normalize.
    auto normStd = [](NTV2Standard s) {
      if(s == NTV2_STANDARD_3840HFR) return NTV2_STANDARD_3840x2160p;
      if(s == NTV2_STANDARD_4096HFR) return NTV2_STANDARD_4096x2160p;
      return s;
    };
    const bool stdOk = got && normStd(st.mVideoStandard) == normStd(std);
    const bool rateOk = got && st.mVideoRate == rate;
    const bool pass = got && st.mEnabled && stdOk && rateOk;
    if(!pass)
      ++failures;
    std::printf(
        "  %-10s | %-9s %-22s %-12s %-6s %-6d sink=%u lock=%u | %s%s%s\n",
        hf.name, got ? (st.mEnabled ? "yes" : "NO") : "?", txStd.c_str(),
        txRate.c_str(), got ? colorSpaceStr(st.mColorSpace) : "?",
        got ? int(st.mVideoBitDepth) : -1, (vcNow >> 29) & 1, (vcNow >> 27) & 1,
        pass ? "PASS" : "FAIL",
        (!pass && got && !st.mEnabled) ? "(tx-disabled)" : "",
        (!pass && got && st.mEnabled && (!stdOk || !rateOk)) ? "(std/rate-mismatch)" : "");
    std::fflush(stdout);
  }
  std::printf("  => %d/%d modes OK\n", tested - failures, tested);
  if(forceHpd)
  {
    ULWord hc = 0;
    card.ReadRegister(kRegHdmiControl, hc);
    card.WriteRegister(kRegHdmiControl, hc & ~(1u << 25));
  }
  return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
  bool forceHpd = false;
  int cardArg = -1;
  for(int i = 1; i < argc; ++i)
  {
    if(!std::strcmp(argv[i], "--force-hpd"))
      forceHpd = true;
    else
      cardArg = std::atoi(argv[i]);
  }
  int rc = 0;
  if(cardArg >= 0)
    rc = probeCard(cardArg, forceHpd);
  else
  {
    rc |= probeCard(0, forceHpd);
    rc |= probeCard(1, forceHpd);
  }
  return rc;
}
