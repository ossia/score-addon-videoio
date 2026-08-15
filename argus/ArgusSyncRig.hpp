#pragma once

/**
 * @file ArgusSyncRig.hpp
 * @brief The capture shared by the Argus backends that make up one camera rig.
 *
 * A rig is several sensors on one carrier board that must be rendered from the
 * same instant -- the 360 head's two eyes. Unlike the V4L2 rig, where each
 * sensor is a separate file descriptor with its own thread and the only thing
 * to share is a correlator, Argus can drive several sensors from a single
 * CaptureSession: one AE/AWB loop, one Request, one repeat clock. That is the
 * synchronisation itself rather than an after-the-fact pairing, so it is what
 * this asks for first.
 *
 * It is not always granted. `createCaptureSession(vector<CameraDevice*>)`
 * returns STATUS_UNAVAILABLE on some driver stacks -- measured on the Orin NX
 * devkit, in every sensor order, with and without setSyncSensorSessionsCount --
 * and there is no way to ask beforehand. So the rig falls back to one session
 * per sensor plus the same arrival correlator the V4L2 rig uses: the eyes are
 * then paired rather than genuinely synchronised, which is worth saying out
 * loud but is still far better than two devices latching independently.
 *
 * Either way the members see one thing: a CaptureSyncGroup that hands every
 * renderer slots from the same capture. `shared()` is what says which of the
 * two guarantees is actually in force.
 *
 * Members find each other by rig name because they are constructed
 * independently, from separate graph nodes, in whatever order the document
 * happens to instantiate them. The registry hands the first caller a new rig
 * and every later caller the same one; when the last member drops its
 * reference the rig goes with it.
 */

#include <argus/ArgusSession.hpp>
#include <argus/ArgusSettings.hpp>

#include <Gfx/Graph/interop/CaptureCorrelator.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Gfx::Argus
{

/**
 * @brief What the rig needs from one member's backend, on the capture thread.
 *
 * Only a renderer that engaged the group takes its slots from it. One that
 * declined -- its rung cannot bind a caller-chosen slot -- still has to be fed,
 * from its own ring, and its slots are still its own strategy's to release.
 */
struct ArgusRigMember
{
  virtual ~ArgusRigMember();

  /// Whether this member's renderer takes its slots from the group.
  virtual bool grouped() const noexcept = 0;

  /// Publish a slot to this member's own ring, for a renderer that declined.
  virtual void publishUngrouped(std::size_t slot) = 0;

  /// Slots this member's own strategy has finished with, for the same case.
  virtual std::uint32_t takeReturnedUngrouped() = 0;
};

class ArgusRig
{
public:
  ArgusRig(std::string name, std::size_t memberCount);
  ~ArgusRig();
  ArgusRig(const ArgusRig&) = delete;
  ArgusRig& operator=(const ArgusRig&) = delete;

  /// Open the capture. The first member to call decides the settings -- every
  /// member is built from the same device, so they all pass the same ones --
  /// and later members get that first answer back without reopening.
  bool open(const ArgusSettings& settings);

  std::size_t memberCount() const noexcept { return m_members; }

  /// True when one session drives every sensor, i.e. the capture is
  /// synchronised rather than correlated after the fact.
  bool shared() const noexcept { return m_shared != nullptr; }

  score::gfx::interop::CaptureSyncGroup& group() noexcept
  {
    return m_correlator.group();
  }

  /// The session this member's slots come from, and its stream within it.
  ArgusSession* sessionFor(std::size_t member) noexcept;
  std::size_t streamFor(std::size_t member) const noexcept;

  /// Join the capture, once this member's renderer has settled. Starts the
  /// session on the first member; the rest simply begin completing sets.
  void arm(std::size_t member, ArgusRigMember* self);
  void disarm(std::size_t member);

private:
  /// Capture thread. One capture of the shared session: every sensor's slot.
  void onSharedCapture(
      const std::size_t* slots, const std::uint64_t* stamps, std::size_t n);
  /// Capture thread. One capture of one member's own session.
  void onSoloCapture(std::size_t member, std::size_t slot, std::uint64_t stamp);
  /// Capture thread. Slots of `member` that may go back to the ISP.
  std::uint32_t takeReturnedFor(std::size_t member);
  /// Hand a slot back to the ISP without the renderer ever seeing it.
  void returnUnused(std::size_t member, std::size_t slot);
  /// Capture thread. Say so, once, when the rig has been stuck on a member for
  /// long enough that it is not the other renderers still settling.
  void reportIfIncomplete(const int* setSlots, std::size_t n);

  /// Captures in a row that could not make a complete set. Four seconds at 30
  /// fps: long enough that a renderer still initialising has finished, short
  /// enough that nobody sits in front of a frozen picture wondering.
  static constexpr std::uint32_t kIncompleteRunWarn = 120;
  std::atomic<std::uint32_t> m_incompleteRun{0};

  const std::string m_name;
  const std::size_t m_members;

  /// Guards the member table against the capture thread. Deliberately not the
  /// lock the session lifecycle uses: stopping a session joins the capture
  /// thread, and that thread takes this one.
  std::mutex m_memberLock;
  ArgusRigMember* m_member[score::gfx::interop::CaptureFrameSet::kMaxMembers]{};

  std::mutex m_lifecycleLock;
  bool m_opened{false};
  bool m_openOk{false};
  std::size_t m_armed{0};
  bool m_startedShared{false};
  bool m_startedSolo[score::gfx::interop::CaptureFrameSet::kMaxMembers]{};
  bool m_warnedUngrouped{false};

  /// One session for every sensor, when the driver allows it.
  std::unique_ptr<ArgusSession> m_shared;
  /// One session per sensor otherwise; empty when m_shared is in use.
  std::vector<std::unique_ptr<ArgusSession>> m_solo;

  /// Publishes into the group. Used as a plain group in shared mode, where one
  /// capture already carries every sensor; used as a correlator in the fallback,
  /// where the sensors arrive on separate threads.
  score::gfx::interop::CaptureCorrelator m_correlator;

  /// Slots to release that the renderer never got: a member's own displaced
  /// offer, or a capture that arrived before its member armed. Merged into the
  /// mask the session polls, which is the only channel Argus takes them back on.
  std::atomic<std::uint32_t>
      m_unused[score::gfx::interop::CaptureFrameSet::kMaxMembers]{};
};

/// The rig named `name`, creating it if this is the first member. `memberCount`
/// is honoured only for the caller that creates it: a later member disagreeing
/// about the size would have to resize a group its partners already publish
/// into. Null for an empty name, which is how a camera says it stands alone.
std::shared_ptr<ArgusRig>
acquireArgusRig(const std::string& name, std::size_t memberCount);

} // namespace Gfx::Argus
