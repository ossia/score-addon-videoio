#include "V4L2Controls.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>

namespace Gfx::V4L2
{
namespace
{
int xioctl(int fd, unsigned long req, void* arg) noexcept
{
  int r;
  do
  {
    r = ::ioctl(fd, req, arg);
  } while(r == -1 && errno == EINTR);
  return r;
}

std::optional<ControlKind> kindOf(std::uint32_t type) noexcept
{
  switch(type)
  {
    case V4L2_CTRL_TYPE_INTEGER:
    case V4L2_CTRL_TYPE_INTEGER64:
      return ControlKind::Integer;
    case V4L2_CTRL_TYPE_BOOLEAN:
      return ControlKind::Boolean;
    case V4L2_CTRL_TYPE_MENU:
      return ControlKind::Menu;
    case V4L2_CTRL_TYPE_INTEGER_MENU:
      return ControlKind::IntegerMenu;
    case V4L2_CTRL_TYPE_BITMASK:
      return ControlKind::Bitmask;
    case V4L2_CTRL_TYPE_BUTTON:
      return ControlKind::Button;
    case V4L2_CTRL_TYPE_STRING:
      return ControlKind::String;
    default:
      // V4L2_CTRL_TYPE_CTRL_CLASS is a section marker rather than a control,
      // and the compound U8/U16/U32 types are read-only blobs -- an Orin NX
      // publishes its whole sensor mode table that way. Neither belongs in a
      // tree of settable values.
      return std::nullopt;
  }
}

/// Whether the control carries a payload rather than a scalar, which
/// VIDIOC_G_CTRL cannot read.
bool hasPayload(const v4l2_query_ext_ctrl& q) noexcept
{
  return q.flags & V4L2_CTRL_FLAG_HAS_PAYLOAD;
}

} // namespace

bool ControlDesc::exceedsInt32() const noexcept
{
  constexpr std::int64_t lo = -2147483648LL;
  constexpr std::int64_t hi = 2147483647LL;
  return min < lo || max > hi;
}

std::string controlSlug(const std::string& name)
{
  std::string out;
  out.reserve(name.size());
  bool lastUnderscore = false;
  for(unsigned char c : name)
  {
    if(std::isalnum(c))
    {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastUnderscore = false;
    }
    else if(!lastUnderscore && !out.empty())
    {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while(!out.empty() && out.back() == '_')
    out.pop_back();
  if(out.empty())
    out = "control";
  return out;
}

ControlSet::~ControlSet()
{
  close();
}

ControlSet::ControlSet(ControlSet&& other) noexcept
    : m_fd{other.m_fd}
    , m_controls{std::move(other.m_controls)}
{
  other.m_fd = -1;
}

ControlSet& ControlSet::operator=(ControlSet&& other) noexcept
{
  if(this != &other)
  {
    close();
    m_fd = other.m_fd;
    m_controls = std::move(other.m_controls);
    other.m_fd = -1;
  }
  return *this;
}

bool ControlSet::open(const std::string& path)
{
  close();
  m_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if(m_fd < 0)
    return false;
  enumerate();
  return true;
}

void ControlSet::close()
{
  if(m_fd >= 0)
  {
    ::close(m_fd);
    m_fd = -1;
  }
  m_controls.clear();
}

void ControlSet::enumerate()
{
  m_controls.clear();
  if(m_fd < 0)
    return;

  std::string currentGroup;

  std::uint32_t id = 0;
  for(;;)
  {
    v4l2_query_ext_ctrl q{};
    q.id = id | V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    if(xioctl(m_fd, VIDIOC_QUERY_EXT_CTRL, &q) != 0)
      break;
    id = q.id;

    if(q.type == V4L2_CTRL_TYPE_CTRL_CLASS)
    {
      currentGroup = q.name;
      continue;
    }

    // A disabled control cannot be read or written and has no useful range;
    // publishing it would be publishing a lie.
    if(q.flags & V4L2_CTRL_FLAG_DISABLED)
      continue;

    const auto kind = kindOf(q.type);
    if(!kind || hasPayload(q))
      continue;

    ControlDesc d;
    d.id = q.id;
    d.kind = *kind;
    d.name = q.name;
    d.group = currentGroup;
    d.min = q.minimum;
    d.max = q.maximum;
    d.step = static_cast<std::int64_t>(q.step);
    d.defaultValue = q.default_value;
    d.readOnly = q.flags & V4L2_CTRL_FLAG_READ_ONLY;
    d.writeOnly = q.flags & V4L2_CTRL_FLAG_WRITE_ONLY;
    d.inactive = q.flags & V4L2_CTRL_FLAG_INACTIVE;
    d.volatileCtrl = q.flags & V4L2_CTRL_FLAG_VOLATILE;
    d.executeOnWrite = q.flags & V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;

    if(d.step <= 0)
      d.step = 1;

    if(d.kind == ControlKind::Menu || d.kind == ControlKind::IntegerMenu)
    {
      // Menus are not required to be contiguous: querying an absent index
      // returns EINVAL and the walk simply skips it.
      for(std::int64_t i = q.minimum; i <= q.maximum; ++i)
      {
        v4l2_querymenu m{};
        m.id = q.id;
        m.index = static_cast<std::uint32_t>(i);
        if(xioctl(m_fd, VIDIOC_QUERYMENU, &m) != 0)
          continue;

        ControlMenuEntry e;
        e.index = m.index;
        if(d.kind == ControlKind::IntegerMenu)
        {
          e.value = m.value;
          e.label = std::to_string(m.value);
        }
        else
        {
          e.label = reinterpret_cast<const char*>(m.name);
          e.value = m.index;
        }
        d.menu.push_back(std::move(e));
      }
    }

    m_controls.push_back(std::move(d));
  }

  // Slugs are the tree addresses, so they have to be unique even when two
  // drivers -- or one driver's private and standard halves -- name two
  // controls the same. The id disambiguates and is stable across runs.
  for(auto& c : m_controls)
    c.slug = controlSlug(c.name);

  for(std::size_t i = 0; i < m_controls.size(); ++i)
  {
    std::size_t dupes = 0;
    for(std::size_t j = 0; j < i; ++j)
      if(m_controls[j].slug == m_controls[i].slug)
        ++dupes;
    if(dupes > 0)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "_%08x", m_controls[i].id);
      m_controls[i].slug += buf;
    }
  }
}

const ControlDesc* ControlSet::find(std::uint32_t id) const noexcept
{
  auto it = std::find_if(
      m_controls.begin(), m_controls.end(),
      [id](const ControlDesc& c) { return c.id == id; });
  return it == m_controls.end() ? nullptr : &*it;
}

const ControlDesc* ControlSet::findBySlug(const std::string& slug) const noexcept
{
  auto it = std::find_if(
      m_controls.begin(), m_controls.end(),
      [&slug](const ControlDesc& c) { return c.slug == slug; });
  return it == m_controls.end() ? nullptr : &*it;
}

std::optional<std::int64_t> ControlSet::get(std::uint32_t id) const
{
  if(m_fd < 0)
    return std::nullopt;
  const auto* d = find(id);
  if(!d || d->writeOnly || d->kind == ControlKind::Button
     || d->kind == ControlKind::String)
    return std::nullopt;

  v4l2_ext_control c{};
  c.id = id;
  v4l2_ext_controls cs{};
  cs.count = 1;
  cs.controls = &c;
#ifdef V4L2_CTRL_WHICH_CUR_VAL
  cs.which = V4L2_CTRL_WHICH_CUR_VAL;
#endif
  if(xioctl(m_fd, VIDIOC_G_EXT_CTRLS, &cs) != 0)
    return std::nullopt;

  return d->exceedsInt32() ? c.value64 : static_cast<std::int64_t>(c.value);
}

std::optional<std::string> ControlSet::getString(std::uint32_t id) const
{
  if(m_fd < 0)
    return std::nullopt;
  const auto* d = find(id);
  if(!d || d->kind != ControlKind::String)
    return std::nullopt;

  std::string buf;
  buf.resize(static_cast<std::size_t>(d->max) + 1u);

  v4l2_ext_control c{};
  c.id = id;
  c.size = static_cast<std::uint32_t>(buf.size());
  c.string = buf.data();
  v4l2_ext_controls cs{};
  cs.count = 1;
  cs.controls = &c;
  if(xioctl(m_fd, VIDIOC_G_EXT_CTRLS, &cs) != 0)
    return std::nullopt;

  buf.resize(std::strlen(buf.c_str()));
  return buf;
}

ControlWriteResult ControlSet::set(std::uint32_t id, std::int64_t value)
{
  ControlWriteResult r;
  if(m_fd < 0)
  {
    r.error = EBADF;
    return r;
  }
  const auto* d = find(id);
  if(!d || d->readOnly)
  {
    r.error = EACCES;
    return r;
  }

  // Clamp before writing: a driver is allowed to reject an out-of-range value
  // outright rather than saturate, which would turn a slider drag into an
  // error instead of a move to the endpoint.
  if(d->kind != ControlKind::Button && d->kind != ControlKind::Bitmask)
    value = std::clamp(value, d->min, d->max);

  v4l2_ext_control c{};
  c.id = id;
  if(d->exceedsInt32())
    c.value64 = value;
  else
    c.value = static_cast<std::int32_t>(value);

  v4l2_ext_controls cs{};
  cs.count = 1;
  cs.controls = &c;
  if(xioctl(m_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0)
  {
    r.error = errno;
    return r;
  }

  r.ok = true;
  // Read back rather than echo: the driver rounds to `step` and clamps to a
  // range that may itself have moved. Measured on an Orin NX, whose `gain`
  // has step 3: a request of 500 lands on 501.
  if(auto v = get(id))
    r.value = *v;
  else
    r.value = value;
  return r;
}

ControlWriteResult ControlSet::setString(std::uint32_t id, const std::string& value)
{
  ControlWriteResult r;
  if(m_fd < 0)
  {
    r.error = EBADF;
    return r;
  }
  const auto* d = find(id);
  if(!d || d->kind != ControlKind::String || d->readOnly)
  {
    r.error = EACCES;
    return r;
  }

  std::string buf = value;
  buf.resize(std::max<std::size_t>(buf.size(), static_cast<std::size_t>(d->min)));
  if(buf.size() > static_cast<std::size_t>(d->max))
    buf.resize(static_cast<std::size_t>(d->max));
  buf.push_back('\0');

  v4l2_ext_control c{};
  c.id = id;
  c.size = static_cast<std::uint32_t>(buf.size());
  c.string = buf.data();
  v4l2_ext_controls cs{};
  cs.count = 1;
  cs.controls = &c;
  if(xioctl(m_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0)
  {
    r.error = errno;
    return r;
  }
  r.ok = true;
  return r;
}

void ControlSet::refresh()
{
  if(m_fd < 0)
    return;
  for(auto& c : m_controls)
  {
    v4l2_query_ext_ctrl q{};
    q.id = c.id;
    if(xioctl(m_fd, VIDIOC_QUERY_EXT_CTRL, &q) != 0)
      continue;
    c.min = q.minimum;
    c.max = q.maximum;
    c.step = q.step > 0 ? static_cast<std::int64_t>(q.step) : 1;
    c.defaultValue = q.default_value;
    c.readOnly = q.flags & V4L2_CTRL_FLAG_READ_ONLY;
    c.inactive = q.flags & V4L2_CTRL_FLAG_INACTIVE;
  }
}

bool ControlSet::subscribeEvents()
{
  if(m_fd < 0)
    return false;
  bool any = false;
  for(const auto& c : m_controls)
  {
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_CTRL;
    sub.id = c.id;
    // Without V4L2_EVENT_SUB_FL_SEND_INITIAL the tree would stay at whatever
    // the enumeration saw until something moved; with it, every control
    // reports itself once on subscription and the tree starts truthful.
    sub.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
    if(xioctl(m_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0)
      any = true;
  }
  return any;
}

bool ControlSet::pollEvents(const std::function<void(const Event&)>& onEvent)
{
  if(m_fd < 0)
    return false;

  for(;;)
  {
    v4l2_event ev{};
    if(xioctl(m_fd, VIDIOC_DQEVENT, &ev) != 0)
    {
      // ENOENT means the queue is empty; anything else and the device is gone.
      return errno == EAGAIN || errno == ENOENT;
    }
    if(ev.type != V4L2_EVENT_CTRL)
      continue;

    const auto& c = ev.u.ctrl;
    Event out;
    out.id = ev.id;
    out.value = (c.type == V4L2_CTRL_TYPE_INTEGER64) ? c.value64 : c.value;
    out.inactive = c.flags & V4L2_CTRL_FLAG_INACTIVE;
    out.readOnly = c.flags & V4L2_CTRL_FLAG_READ_ONLY;
    out.rangeChanged = ev.u.ctrl.changes & V4L2_EVENT_CTRL_CH_RANGE;
    out.min = c.minimum;
    out.max = c.maximum;
    out.step = c.step;
    out.defaultValue = c.default_value;

    if(auto* d = const_cast<ControlDesc*>(find(ev.id)))
    {
      d->inactive = out.inactive;
      d->readOnly = out.readOnly;
      if(out.rangeChanged)
      {
        d->min = out.min;
        d->max = out.max;
        d->step = out.step > 0 ? out.step : 1;
        d->defaultValue = out.defaultValue;
      }
    }

    onEvent(out);
  }
}

} // namespace Gfx::V4L2
