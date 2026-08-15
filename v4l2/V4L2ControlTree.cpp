#include "V4L2ControlTree.hpp"

#include <Gfx/ControlTree.hpp>

#include <ossia/network/common/complex_type.hpp>
#include <ossia/network/domain/domain.hpp>

#include <QSocketNotifier>

#include <algorithm>

namespace Gfx::V4L2
{
namespace
{
/// What the tree publishes for a control holding the driver-side value @p raw.
///
/// The two menu kinds differ: V4L2 stores an *index* for both, but a plain
/// menu's meaning is its label while an integer menu's is its payload, so a
/// tree that published indices for the latter would show 0..n instead of the
/// frame rates or bit depths the user actually picks between.
ossia::value publishedValue(const ControlDesc& c, std::int64_t raw)
{
  switch(c.kind)
  {
    case ControlKind::Boolean:
      return bool(raw != 0);

    case ControlKind::Menu:
    {
      auto it = std::find_if(
          c.menu.begin(), c.menu.end(),
          [raw](const ControlMenuEntry& e) { return std::int64_t(e.index) == raw; });
      if(it != c.menu.end())
        return it->label;
      return std::string{};
    }

    case ControlKind::IntegerMenu:
    {
      auto it = std::find_if(
          c.menu.begin(), c.menu.end(),
          [raw](const ControlMenuEntry& e) { return std::int64_t(e.index) == raw; });
      if(it != c.menu.end())
        return int(it->value);
      return int(raw);
    }

    case ControlKind::Integer:
      if(c.exceedsInt32())
        return float(raw);
      return int(raw);

    case ControlKind::Bitmask:
      return int(raw);

    case ControlKind::Button:
    case ControlKind::String:
      break;
  }
  return {};
}

/// The driver-side value for something written to the tree. Empty when the
/// value cannot be interpreted for this control.
std::optional<std::int64_t> driverValue(const ControlDesc& c, const ossia::value& v)
{
  switch(c.kind)
  {
    case ControlKind::Boolean:
      return ossia::convert<bool>(v) ? 1 : 0;

    case ControlKind::Menu:
    {
      // A label is what the explorer offers, but an index is what OSC and
      // scripts tend to send, so accept either -- the Window device's `screen`
      // parameter takes the same two forms for the same reason.
      if(auto s = v.target<std::string>())
      {
        auto it = std::find_if(
            c.menu.begin(), c.menu.end(),
            [&s](const ControlMenuEntry& e) { return e.label == *s; });
        if(it != c.menu.end())
          return std::int64_t(it->index);
        return std::nullopt;
      }
      const auto idx = ossia::convert<int>(v);
      auto it = std::find_if(
          c.menu.begin(), c.menu.end(),
          [idx](const ControlMenuEntry& e) { return int(e.index) == idx; });
      return it != c.menu.end() ? std::optional<std::int64_t>{idx} : std::nullopt;
    }

    case ControlKind::IntegerMenu:
    {
      const auto want = std::int64_t(ossia::convert<int>(v));
      auto it = std::find_if(
          c.menu.begin(), c.menu.end(),
          [want](const ControlMenuEntry& e) { return e.value == want; });
      if(it != c.menu.end())
        return std::int64_t(it->index);
      return std::nullopt;
    }

    case ControlKind::Integer:
    case ControlKind::Bitmask:
      return std::int64_t(ossia::convert<float>(v));

    case ControlKind::Button:
      return 0;

    case ControlKind::String:
      break;
  }
  return std::nullopt;
}

ossia::val_type typeFor(const ControlDesc& c)
{
  switch(c.kind)
  {
    case ControlKind::Boolean:
      return ossia::val_type::BOOL;
    case ControlKind::Menu:
    case ControlKind::String:
      return ossia::val_type::STRING;
    case ControlKind::Button:
      return ossia::val_type::IMPULSE;
    case ControlKind::Integer:
      return c.exceedsInt32() ? ossia::val_type::FLOAT : ossia::val_type::INT;
    case ControlKind::IntegerMenu:
    case ControlKind::Bitmask:
      return ossia::val_type::INT;
  }
  return ossia::val_type::INT;
}

ossia::domain domainFor(const ControlDesc& c)
{
  switch(c.kind)
  {
    case ControlKind::Menu:
    {
      std::vector<std::string> labels;
      labels.reserve(c.menu.size());
      for(const auto& e : c.menu)
        labels.push_back(e.label);
      return ossia::make_domain(std::move(labels));
    }
    case ControlKind::IntegerMenu:
    {
      std::vector<ossia::value> vals;
      vals.reserve(c.menu.size());
      for(const auto& e : c.menu)
        vals.push_back(int(e.value));
      return ossia::make_domain(vals);
    }
    case ControlKind::Integer:
      if(c.exceedsInt32())
        return ossia::make_domain(float(c.min), float(c.max));
      return ossia::make_domain(int(c.min), int(c.max));
    case ControlKind::Boolean:
    case ControlKind::Bitmask:
    case ControlKind::Button:
    case ControlKind::String:
      break;
  }
  return {};
}

ossia::access_mode accessFor(const ControlDesc& c)
{
  if(c.readOnly)
    return ossia::access_mode::GET;
  if(c.writeOnly)
    return ossia::access_mode::SET;
  return ossia::access_mode::BI;
}

std::string descriptionFor(const ControlDesc& c)
{
  std::string d = c.name;
  if(!c.group.empty())
    d += " (" + c.group + ")";
  if(c.step > 1)
    d += ", step " + std::to_string(c.step);
  if(c.inactive)
    d += ", currently inactive";
  return d;
}
} // namespace

ControlTree::ControlTree(
    const std::string& devicePath, ossia::net::device_base& dev,
    ossia::net::node_base& parent, std::string group)
    : m_echo{std::make_shared<bool>(false)}
{
  if(!m_set.open(devicePath))
    return;

  build(dev, parent, group);

  if(m_set.subscribeEvents())
  {
    // V4L2 signals control changes as a priority condition on the fd, which is
    // what QSocketNotifier calls an Exception. Watching the fd rather than
    // polling means an external `v4l2-ctl` shows up immediately and an idle
    // camera costs nothing.
    m_notifier
        = std::make_unique<QSocketNotifier>(m_set.fd(), QSocketNotifier::Exception);
    QObject::connect(
        m_notifier.get(), &QSocketNotifier::activated, m_notifier.get(),
        [this] { m_set.pollEvents([this](const ControlSet::Event& e) { onDriverEvent(e); }); });
    m_notifier->setEnabled(true);

    // Drain the initial burst: subscribing asks for one event per control, and
    // those carry the values the tree was just built from.
    m_set.pollEvents([](const ControlSet::Event&) { });
  }
}

ControlTree::~ControlTree() = default;

void ControlTree::build(
    ossia::net::device_base& dev, ossia::net::node_base& parent,
    const std::string& group)
{
  const auto& ctrls = m_set.controls();

  std::vector<Gfx::TreeControl> tcs;
  tcs.reserve(ctrls.size());

  for(std::size_t i = 0; i < ctrls.size(); ++i)
  {
    const auto& c = ctrls[i];

    Gfx::TreeControl t;
    t.name = c.slug;
    t.description = descriptionFor(c);
    t.type = typeFor(c);
    t.domain = domainFor(c);
    t.access = accessFor(c);

    if(c.kind == ControlKind::String)
    {
      if(auto s = m_set.getString(c.id))
        t.initial = *s;
    }
    else if(c.kind != ControlKind::Button)
    {
      if(auto v = m_set.get(c.id))
        t.initial = publishedValue(c, *v);
    }

    if(!c.readOnly)
    {
      const auto id = c.id;
      t.onSet = [this, id](const ossia::value& v) {
        if(*m_echo)
          return;
        const auto* d = m_set.find(id);
        if(!d)
          return;

        if(d->kind == ControlKind::String)
        {
          if(auto s = v.target<std::string>())
            m_set.setString(id, *s);
          return;
        }

        const auto raw = driverValue(*d, v);
        if(!raw)
          return;

        const auto r = m_set.set(id, *raw);
        if(!r.ok)
          return;

        // The driver rounds to `step` and clamps; publish what it took rather
        // than what was asked, or the tree and the hardware disagree silently.
        if(r.value != *raw)
          if(auto* p = paramFor(id))
          {
            *m_echo = true;
            p->push_value(publishedValue(*d, r.value));
            *m_echo = false;
          }
      };
    }

    tcs.push_back(std::move(t));
  }

  m_params = Gfx::addControlGroup(dev, parent, group, tcs);
}

ossia::net::parameter_base* ControlTree::paramFor(std::uint32_t id) const noexcept
{
  const auto& ctrls = m_set.controls();
  for(std::size_t i = 0; i < ctrls.size() && i < m_params.size(); ++i)
    if(ctrls[i].id == id)
      return m_params[i];
  return nullptr;
}

void ControlTree::onDriverEvent(const ControlSet::Event& e)
{
  const auto* d = m_set.find(e.id);
  auto* p = paramFor(e.id);
  if(!d || !p)
    return;

  // An inactive control is one the hardware will not accept writes for until
  // something else changes -- auto-white-balance holding its temperature, for
  // instance. Reflecting that as read-only is the closest the tree can say.
  p->set_access(d->inactive || d->readOnly ? ossia::access_mode::GET
                                           : ossia::access_mode::BI);

  if(e.rangeChanged)
    p->set_domain(domainFor(*d));

  *m_echo = true;
  p->push_value(publishedValue(*d, e.value));
  *m_echo = false;
}

} // namespace Gfx::V4L2
