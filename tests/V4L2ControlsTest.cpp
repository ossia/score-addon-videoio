// Validates the V4L2 control layer against whatever cameras the machine has.
// No GPU, no display, no streaming -- so it runs on a build box, a Jetson over
// ssh, and a laptop with a webcam, which is the coverage the tree needs since
// the same code has to serve every V4L2 driver.
//
//   V4L2ControlsTest                 # enumerate every /dev/video*, report
//   V4L2ControlsTest --device /dev/video0
//   V4L2ControlsTest --write         # also exercise set() round-trips
//   V4L2ControlsTest --events        # wait for a control change and report it
//
// Exit code is nonzero if any check fails.

#include <v4l2/V4L2Controls.hpp>

#include <dirent.h>
#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Gfx::V4L2;

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what)
{
  ++g_checks;
  if(!cond)
  {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

const char* kindName(ControlKind k)
{
  switch(k)
  {
    case ControlKind::Integer: return "int";
    case ControlKind::Boolean: return "bool";
    case ControlKind::Menu: return "menu";
    case ControlKind::IntegerMenu: return "intmenu";
    case ControlKind::Bitmask: return "bitmask";
    case ControlKind::Button: return "button";
    case ControlKind::String: return "string";
  }
  return "?";
}

std::vector<std::string> videoDevices()
{
  std::vector<std::string> out;
  if(DIR* dir = ::opendir("/dev"))
  {
    while(dirent* e = ::readdir(dir))
      if(std::strncmp(e->d_name, "video", 5) == 0)
        out.push_back(std::string{"/dev/"} + e->d_name);
    ::closedir(dir);
  }
  std::sort(out.begin(), out.end());
  return out;
}

/// Every property the tree relies on, asserted for one control.
void checkDescriptor(const ControlDesc& c, const std::vector<ControlDesc>& all)
{
  check(!c.slug.empty(), c.name + ": slug is empty");

  for(char ch : c.slug)
    check(
        (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_',
        c.name + ": slug '" + c.slug + "' has a character an address cannot carry");

  const auto dupes = std::count_if(
      all.begin(), all.end(), [&](const ControlDesc& o) { return o.slug == c.slug; });
  check(dupes == 1, c.name + ": slug '" + c.slug + "' is not unique");

  check(c.step > 0, c.name + ": step must be positive");

  if(c.kind != ControlKind::Button && c.kind != ControlKind::String)
    check(c.min <= c.max, c.name + ": inverted range");

  if(c.kind == ControlKind::Menu || c.kind == ControlKind::IntegerMenu)
  {
    check(!c.menu.empty(), c.name + ": a menu with no entries");
    for(const auto& e : c.menu)
      check(!e.label.empty(), c.name + ": a menu entry with no label");
  }

  // A control cannot be both directions at once, and a read-only control that
  // claims write-only would make the tree's access mode meaningless.
  check(!(c.readOnly && c.writeOnly), c.name + ": both read-only and write-only");
}

/// Writes a control, then verifies the readback obeys the driver's own rules.
void checkWrite(ControlSet& set, const ControlDesc& c)
{
  if(c.readOnly || c.inactive || c.executeOnWrite)
    return;
  if(c.kind != ControlKind::Integer && c.kind != ControlKind::Boolean)
    return;

  const auto before = set.get(c.id);
  if(!before)
    return;

  // Aim at a value inside the range that is deliberately NOT step-aligned when
  // step > 1, because the interesting case is what the driver does with it.
  const std::int64_t target
      = std::clamp<std::int64_t>(c.min + (c.max - c.min) / 2 + 1, c.min, c.max);

  const auto r = set.set(c.id, target);
  if(!r.ok)
  {
    // A refusal is a legitimate outcome (EBUSY while streaming, EACCES); what
    // must not happen is a silent claim of success.
    std::printf(
        "    %-34s write refused (errno %d) -- reported, not hidden\n", c.slug.c_str(),
        r.error);
    return;
  }

  check(
      r.value >= c.min && r.value <= c.max,
      c.slug + ": readback " + std::to_string(r.value) + " outside ["
          + std::to_string(c.min) + "," + std::to_string(c.max) + "]");

  if(c.step > 1)
    check(
        ((r.value - c.min) % c.step) == 0,
        c.slug + ": readback " + std::to_string(r.value) + " is not aligned to step "
            + std::to_string(c.step));

  if(r.value != target)
    std::printf(
        "    %-34s asked %lld, hardware took %lld (step %lld) -- readback matters\n",
        c.slug.c_str(), (long long)target, (long long)r.value, (long long)c.step);

  // Leave the camera as we found it.
  set.set(c.id, *before);
}

int runDevice(const std::string& path, bool doWrite)
{
  ControlSet set;
  if(!set.open(path))
  {
    std::printf("%s: cannot open (skipped)\n", path.c_str());
    return 0;
  }

  const auto& ctrls = set.controls();
  std::printf("\n=== %s: %zu controls ===\n", path.c_str(), ctrls.size());

  for(const auto& c : ctrls)
  {
    checkDescriptor(c, ctrls);

    std::string flags;
    if(c.readOnly)
      flags += " ro";
    if(c.writeOnly)
      flags += " wo";
    if(c.inactive)
      flags += " inactive";
    if(c.volatileCtrl)
      flags += " volatile";
    if(c.executeOnWrite)
      flags += " exec-on-write";

    const auto v = set.get(c.id);
    std::printf(
        "  %-34s %-8s [%lld..%lld step %lld] = %s%s\n", c.slug.c_str(),
        kindName(c.kind), (long long)c.min, (long long)c.max, (long long)c.step,
        v ? std::to_string(*v).c_str() : "-", flags.c_str());

    if(!c.menu.empty())
    {
      std::printf("      menu:");
      for(const auto& e : c.menu)
        std::printf(" %u=%s", e.index, e.label.c_str());
      std::printf("\n");
    }
  }

  if(doWrite)
  {
    std::printf("  -- write round-trips --\n");
    for(const auto& c : ctrls)
      checkWrite(set, c);
  }

  return 0;
}

/// Proves the tree can track a change it did not make: subscribe, then wait for
/// an external writer (another process, or the camera itself).
int runEvents(const std::string& path, int seconds)
{
  ControlSet set;
  if(!set.open(path))
  {
    std::printf("%s: cannot open\n", path.c_str());
    return 1;
  }
  if(!set.subscribeEvents())
  {
    std::printf("%s: no control events available on this driver\n", path.c_str());
    return 1;
  }
  std::printf(
      "%s: subscribed, waiting %ds for a control change "
      "(try: v4l2-ctl -d %s -c <name>=<value>)\n",
      path.c_str(), seconds, path.c_str());

  int seen = 0;
  // The initial burst is the SEND_INITIAL echo of every control; drain it so
  // what follows is genuinely a change.
  set.pollEvents([](const ControlSet::Event&) {});

  for(int i = 0; i < seconds * 10; ++i)
  {
    pollfd pfd{};
    pfd.fd = set.fd();
    pfd.events = POLLPRI;
    ::poll(&pfd, 1, 100);
    if(pfd.revents & POLLPRI)
    {
      set.pollEvents([&](const ControlSet::Event& e) {
        const auto* d = set.find(e.id);
        std::printf(
            "  event: %-30s = %lld%s%s\n", d ? d->slug.c_str() : "?",
            (long long)e.value, e.inactive ? " (now inactive)" : "",
            e.rangeChanged ? " (range changed)" : "");
        ++seen;
      });
    }
  }

  check(seen > 0, "no control event was received");
  return 0;
}
} // namespace

int main(int argc, char** argv)
{
  std::string device;
  bool doWrite = false, doEvents = false;
  int seconds = 10;
  for(int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if(a == "--device" && i + 1 < argc)
      device = argv[++i];
    else if(a == "--write")
      doWrite = true;
    else if(a == "--events")
      doEvents = true;
    else if(a == "--seconds" && i + 1 < argc)
      seconds = std::atoi(argv[++i]);
  }

  // Slugging is pure and must hold regardless of hardware.
  check(controlSlug("White Balance, Automatic") == "white_balance_automatic",
        "slug of 'White Balance, Automatic'");
  check(controlSlug("Exposure Time, Absolute") == "exposure_time_absolute",
        "slug of 'Exposure Time, Absolute'");
  check(controlSlug("gain") == "gain", "slug of 'gain'");
  check(controlSlug("  ") == "control", "slug of a nameless control");

  if(doEvents)
  {
    if(device.empty())
      device = "/dev/video0";
    runEvents(device, seconds);
  }
  else if(!device.empty())
  {
    runDevice(device, doWrite);
  }
  else
  {
    const auto devs = videoDevices();
    if(devs.empty())
      std::printf("no /dev/video* on this machine\n");
    for(const auto& d : devs)
      runDevice(d, doWrite);
  }

  std::printf(
      "\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
