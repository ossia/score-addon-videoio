#include "ArgusSyncRig.hpp"

#include <QDebug>
#include <QString>

#include <map>

namespace Gfx::Argus
{

ArgusRigMember::~ArgusRigMember() = default;

ArgusRig::ArgusRig(std::string name, std::size_t memberCount)
    : m_name{std::move(name)}
    , m_members{
          memberCount < score::gfx::interop::CaptureFrameSet::kMaxMembers
              ? memberCount
              : score::gfx::interop::CaptureFrameSet::kMaxMembers}
    , m_correlator{
          memberCount < score::gfx::interop::CaptureFrameSet::kMaxMembers
              ? memberCount
              : score::gfx::interop::CaptureFrameSet::kMaxMembers}
{
}

ArgusRig::~ArgusRig()
{
  if(m_shared)
    m_shared->stop();
  for(auto& s : m_solo)
    if(s)
      s->stop();
}

bool ArgusRig::open(const ArgusSettings& settings)
{
  std::lock_guard lock{m_lifecycleLock};
  if(m_opened)
    return m_openOk;
  m_opened = true;

  std::vector<std::uint32_t> ids = settings.sensorIds;
  if(ids.empty())
    ids.push_back(settings.sensorId);
  if(ids.size() > m_members)
    ids.resize(m_members);

  // One session for every sensor is the only shape that synchronises the
  // capture itself, so it is what we ask for. Some driver stacks refuse it and
  // there is no way to ask beforehand; the override exists because on a board
  // where it is known to fail, attempting it costs a session teardown before
  // every run.
  const bool trySharedSession
      = ids.size() > 1 && qgetenv("SCORE_ARGUS_SHARED_SESSION") != "0";

  if(trySharedSession)
  {
    auto session = std::make_unique<ArgusSession>();
    ArgusSettings shared = settings;
    shared.sensorIds = ids;
    if(session->open(shared) && session->streamCount() >= ids.size())
    {
      qDebug() << "Argus rig" << m_name.c_str() << ": one session drives"
               << int(ids.size())
               << "sensors; they are exposed together, not merely paired";
      m_shared = std::move(session);
      m_openOk = true;
      return true;
    }
    qWarning() << "Argus rig" << m_name.c_str()
               << ": the driver refused a single session for" << int(ids.size())
               << "sensors. Falling back to one session per sensor: the eyes "
                  "will be paired by arrival rather than exposed together, and "
                  "each runs its own AE/AWB loop.";
  }

  m_solo.resize(ids.size());
  for(std::size_t i = 0; i < ids.size(); ++i)
  {
    ArgusSettings solo = settings;
    solo.sensorIds.clear();
    solo.sensorId = ids[i];
    solo.syncMember = i;

    auto session = std::make_unique<ArgusSession>();
    if(!session->open(solo))
    {
      qWarning() << "Argus rig" << m_name.c_str() << ": sensor" << ids[i]
                 << "would not open; the rig cannot run";
      m_solo.clear();
      return false;
    }
    m_solo[i] = std::move(session);
  }

  m_openOk = !m_solo.empty();
  return m_openOk;
}

ArgusSession* ArgusRig::sessionFor(std::size_t member) noexcept
{
  if(m_shared)
    return m_shared.get();
  return member < m_solo.size() ? m_solo[member].get() : nullptr;
}

std::size_t ArgusRig::streamFor(std::size_t member) const noexcept
{
  // A shared session gives each sensor its own stream; a solo one has exactly
  // the sensor it was opened for. Getting this wrong is the failure worth
  // guarding against hardest: two members both reading stream 0 look perfectly
  // synchronised, because they are the same sensor twice.
  return m_shared ? member : 0;
}

void ArgusRig::arm(std::size_t member, ArgusRigMember* self)
{
  if(member >= m_members || !self)
    return;

  {
    std::lock_guard lock{m_memberLock};
    m_member[member] = self;
  }

  std::lock_guard lock{m_lifecycleLock};
  ++m_armed;

  if(!self->grouped() && !m_warnedUngrouped)
  {
    m_warnedUngrouped = true;
    qWarning() << "Argus rig" << m_name.c_str() << ": member" << int(member)
               << "declined the sync group, so it renders on its own cadence "
                  "and the members that did engage will hold, waiting for a "
                  "capture it never contributes to.";
  }

  if(m_shared)
  {
    if(m_startedShared)
      return;
    m_startedShared = m_shared->start(
        [this](
            const std::size_t* slots, const std::uint64_t* stamps,
            std::size_t n) { onSharedCapture(slots, stamps, n); },
        [this](std::size_t stream) { return takeReturnedFor(stream); });
    if(!m_startedShared)
      qWarning() << "Argus rig" << m_name.c_str() << ": the session would not "
                                                     "start";
    return;
  }

  if(member >= m_solo.size() || !m_solo[member] || m_startedSolo[member])
    return;
  m_startedSolo[member] = m_solo[member]->start(
      [this, member](
          const std::size_t* slots, const std::uint64_t* stamps, std::size_t n) {
        if(n >= 1)
          onSoloCapture(member, slots[0], stamps[0]);
      },
      [this, member](std::size_t) { return takeReturnedFor(member); });
  if(!m_startedSolo[member])
    qWarning() << "Argus rig" << m_name.c_str() << ": sensor for member"
               << int(member) << "would not start";
}

void ArgusRig::disarm(std::size_t member)
{
  if(member >= m_members)
    return;

  // Cleared before anything is stopped, and under the lock the capture thread
  // takes: once this returns, no callback can still be holding the member.
  {
    std::lock_guard lock{m_memberLock};
    if(!m_member[member])
      return;
    m_member[member] = nullptr;
  }

  std::lock_guard lock{m_lifecycleLock};
  if(m_armed > 0)
    --m_armed;

  if(m_shared)
  {
    // The shared session feeds every member, so it only stops with the last of
    // them. A member leaving simply stops completing sets.
    if(m_armed == 0 && m_startedShared)
    {
      m_shared->stop();
      m_startedShared = false;
    }
    return;
  }

  if(member < m_solo.size() && m_solo[member] && m_startedSolo[member])
  {
    m_solo[member]->stop();
    m_startedSolo[member] = false;
  }

  // Whatever this member was still holding can never complete a row now, so
  // give it up rather than leave its partners waiting on a member that is gone.
  int held[score::gfx::interop::CaptureFrameSet::kMaxMembers];
  m_correlator.drain(held);
  for(std::size_t i = 0; i < m_members; ++i)
    if(held[i] >= 0)
      returnUnused(i, std::size_t(held[i]));
}

void ArgusRig::onSharedCapture(
    const std::size_t* slots, const std::uint64_t* stamps, std::size_t n)
{
  int setSlots[score::gfx::interop::CaptureFrameSet::kMaxMembers];
  std::uint64_t setStamps[score::gfx::interop::CaptureFrameSet::kMaxMembers];
  bool complete = true;

  {
    std::lock_guard lock{m_memberLock};
    for(std::size_t m = 0; m < m_members; ++m)
    {
      setSlots[m] = -1;
      setStamps[m] = 0;

      auto* member = m < n ? m_member[m] : nullptr;
      if(!member)
      {
        // A member whose renderer has not settled yet, or has gone. Its buffer
        // goes straight back or the ISP runs out of memory to write into.
        complete = false;
        if(m < n)
          returnUnused(m, slots[m]);
        continue;
      }

      if(member->grouped())
      {
        setSlots[m] = int(slots[m]);
        setStamps[m] = stamps[m];
      }
      else
      {
        // Its renderer reads its own ring and never asks the group, so it
        // cannot be part of the set -- and the set is then unpublishable.
        complete = false;
        member->publishUngrouped(slots[m]);
      }
    }
  }

  if(complete)
  {
    m_correlator.group().publish(setSlots, setStamps);
    m_incompleteRun.store(0, std::memory_order_relaxed);
    return;
  }

  // Publishing a set with a hole in it would be worse than not publishing:
  // take() only ever serves complete sets, so nothing would bind it, and the
  // slots it names would be lent to the group with nothing left to release
  // them. Give them back instead.
  for(std::size_t m = 0; m < m_members; ++m)
    if(setSlots[m] >= 0)
      returnUnused(m, std::size_t(setSlots[m]));

  warnIncomplete(setSlots, n);
}

void ArgusRig::warnIncomplete(const int* setSlots, std::size_t n)
{
  // Only complete sets are handed out, so one member missing holds the whole
  // rig on its last frame. At startup that is just the other renderers not
  // having settled yet and it clears itself within a second; sustained, it is a
  // child nobody connected -- which otherwise presents as a picture that froze
  // for no visible reason.
  const auto run = m_incompleteRun.fetch_add(1, std::memory_order_relaxed) + 1;
  if(run != kIncompleteRunWarn)
    return;

  QString missing;
  for(std::size_t m = 0; m < m_members; ++m)
    if(m >= n || setSlots[m] < 0)
      missing += (missing.isEmpty() ? "" : ", ") + QString("cam%1").arg(int(m));
  qWarning() << "Argus rig" << m_name.c_str() << ": holding on"
             << missing.toUtf8().constData()
             << "-- nothing is rendering them, so the rig has no complete "
                "capture to hand out. Connect every child, or use one device "
                "per sensor if they do not need to be synchronised.";
}

void ArgusRig::onSoloCapture(
    std::size_t member, std::size_t slot, std::uint64_t stamp)
{
  std::lock_guard lock{m_memberLock};
  auto* self = m_member[member];
  if(!self)
  {
    returnUnused(member, slot);
    return;
  }

  if(!self->grouped())
  {
    self->publishUngrouped(slot);
    return;
  }

  // Separate sessions arrive on separate threads, so the set is assembled from
  // arrivals; the offer that completes a row publishes it.
  const int displaced = m_correlator.offer(member, int(slot), stamp);
  if(displaced < 0)
  {
    m_incompleteRun.store(0, std::memory_order_relaxed);
    return;
  }

  // Displacing our own previous offer means it never found a partner. Once in a
  // while that is a dropped frame; every capture in a row is a member nobody is
  // rendering, and the rig then holds every other member on its last frame.
  returnUnused(member, std::size_t(displaced));
  if(m_incompleteRun.fetch_add(1, std::memory_order_relaxed) + 1
     == kIncompleteRunWarn)
  {
    qWarning() << "Argus rig" << m_name.c_str() << ": member" << int(member)
               << "has displaced its own offer" << int(kIncompleteRunWarn)
               << "times running -- its partners are not delivering, so the "
                  "rig has no complete capture to hand out. Connect every "
                  "child, or use one device per sensor if they do not need to "
                  "be synchronised.";
  }
}

std::uint32_t ArgusRig::takeReturnedFor(std::size_t member)
{
  if(member >= m_members)
    return 0;

  std::uint32_t mask = m_unused[member].exchange(0, std::memory_order_acquire);

  std::lock_guard lock{m_memberLock};
  auto* self = m_member[member];
  if(self && !self->grouped())
    mask |= self->takeReturnedUngrouped();
  else
    mask |= m_correlator.group().takeReturned(member);
  return mask;
}

void ArgusRig::returnUnused(std::size_t member, std::size_t slot)
{
  if(member >= m_members || slot >= 32u)
    return;
  m_unused[member].fetch_or(1u << unsigned(slot), std::memory_order_release);
}

std::shared_ptr<ArgusRig>
acquireArgusRig(const std::string& name, std::size_t memberCount)
{
  if(name.empty())
    return {};

  static std::mutex mutex;
  static std::map<std::string, std::weak_ptr<ArgusRig>> rigs;

  std::lock_guard lock{mutex};

  // Weak, so the rig's lifetime is the union of its members' and a rig that
  // stops leaves nothing for the next one under the same name to inherit --
  // including a capture session that would then refuse to reopen.
  auto& entry = rigs[name];
  if(auto existing = entry.lock())
    return existing;

  auto fresh = std::make_shared<ArgusRig>(name, memberCount);
  entry = fresh;
  return fresh;
}

} // namespace Gfx::Argus
