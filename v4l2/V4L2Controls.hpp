#pragma once

/**
 * @file V4L2Controls.hpp
 * @brief Enumerate, read and write a V4L2 device's controls.
 *
 * Deliberately free of any ossia or Qt dependency: this is the V4L2 half of
 * exposing a camera's controls in the device tree, and it is testable on its
 * own against whatever cameras a machine happens to have.
 *
 * The fd is separate from the capture session's. Control ioctls are
 * device-wide rather than per-filehandle -- measured: `v4l2-ctl -c gain=...`
 * from another process changes the picture while score streams on its own fd
 * -- so nothing has to be coordinated with the capture thread, and controls
 * work before streaming has ever started, which is when the device tree is
 * built.
 *
 * Enumeration walks with V4L2_CTRL_FLAG_NEXT_CTRL. That is the only way to
 * reach vendor-private controls: a Logitech BRIO puts `led1_mode` at
 * 0x0a046d05 and an Orin NX puts its whole sensor interface at 0x009a2xxx,
 * neither of which is reachable by iterating the documented ID ranges.
 */

#include <score_addon_videoio_export.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Gfx::V4L2
{

/// What a control accepts, once the V4L2 type has been reduced to the shapes
/// a device tree can actually represent.
enum class ControlKind
{
  Integer, ///< integer with min/max/step
  Boolean,
  Menu,        ///< named choices; `menu` holds them
  IntegerMenu, ///< numeric choices; `menu` holds them, labels are the numbers
  Bitmask,
  Button, ///< write-only trigger, no value
  String,
};

/// One selectable entry of a menu control. V4L2 menus are not required to be
/// contiguous, so the index is carried explicitly rather than implied by
/// position.
struct ControlMenuEntry
{
  std::uint32_t index{};
  std::string label;
  std::int64_t value{}; ///< IntegerMenu only; the numeric payload
};

/// A control as the driver describes it.
struct ControlDesc
{
  std::uint32_t id{};
  ControlKind kind{};
  std::string name;  ///< driver-provided, human readable ("White Balance, Automatic")
  std::string slug;  ///< tree-safe, unique within the device ("white_balance_automatic")
  std::string group; ///< control class name ("Camera Controls"), may be empty

  std::int64_t min{};
  std::int64_t max{};
  std::int64_t step{1};
  std::int64_t defaultValue{};

  /// The driver stores this control in the 64-bit half of the ext-control
  /// union. Determined by the V4L2 type, never by whether the range happens to
  /// fit 32 bits: on little-endian the two alias, so the wrong one appears to
  /// work, which is how it stays wrong.
  bool int64Type{};

  bool readOnly{};
  bool writeOnly{};
  bool inactive{};    ///< currently unsettable because another control says so
  bool volatileCtrl{};
  bool executeOnWrite{}; ///< writing performs an action; never write to probe

  std::vector<ControlMenuEntry> menu;

  /// True when the range does not fit a 32-bit signed integer, which the tree
  /// cannot represent exactly and must widen to a float.
  bool exceedsInt32() const noexcept;
};

/// Result of a write. A driver is entitled to clamp, round to `step`, or
/// refuse, so the caller must not assume the value it asked for.
struct ControlWriteResult
{
  bool ok{};
  std::int64_t value{}; ///< what the hardware actually holds afterwards
  int error{};          ///< errno when !ok
};

/**
 * @brief A control-only handle on a V4L2 device.
 *
 * Owns its own fd, independent of any capture session on the same device.
 */
class SCORE_ADDON_VIDEOIO_EXPORT ControlSet
{
public:
  ControlSet() noexcept = default;
  ~ControlSet();
  ControlSet(const ControlSet&) = delete;
  ControlSet& operator=(const ControlSet&) = delete;
  ControlSet(ControlSet&&) noexcept;
  ControlSet& operator=(ControlSet&&) noexcept;

  /// Opens @p path and enumerates its controls. False if the device cannot be
  /// opened; a device with no controls at all opens successfully with an empty
  /// list.
  bool open(const std::string& path);
  void close();
  bool isOpen() const noexcept { return m_fd >= 0; }

  const std::vector<ControlDesc>& controls() const noexcept { return m_controls; }
  const ControlDesc* find(std::uint32_t id) const noexcept;
  const ControlDesc* findBySlug(const std::string& slug) const noexcept;

  /// Current value. Empty for Button (nothing to read) and for a write-only
  /// control.
  std::optional<std::int64_t> get(std::uint32_t id) const;
  std::optional<std::string> getString(std::uint32_t id) const;

  /// Writes, then reads back. The returned value is the hardware's, which may
  /// differ from @p value: on an Orin NX `gain` has step 3, so a request of
  /// 500 lands on 501.
  ControlWriteResult set(std::uint32_t id, std::int64_t value);
  ControlWriteResult setString(std::uint32_t id, const std::string& value);

  /// Re-reads flags and ranges for every control. A control carrying
  /// V4L2_CTRL_FLAG_UPDATE changes *other* controls when written -- on an Orin
  /// NX, `frame_rate` bounds what `exposure` will accept -- so a write to one
  /// invalidates the rest of the table.
  void refresh();

  /// Subscribes to V4L2_EVENT_CTRL for every enumerated control. Verified
  /// working on both uvcvideo and tegra-video, including for changes made by
  /// other processes.
  bool subscribeEvents();

  /// What changed, as reported by the driver.
  struct Event
  {
    std::uint32_t id{};
    std::int64_t value{};
    bool inactive{};
    bool readOnly{};
    bool rangeChanged{};
    std::int64_t min{}, max{}, step{}, defaultValue{};
  };

  /// Drains pending events without blocking. Returns false if the fd is gone.
  bool pollEvents(const std::function<void(const Event&)>& onEvent);

  /// The fd, for a caller that wants to poll() it alongside others.
  int fd() const noexcept { return m_fd; }

private:
  void enumerate();

  int m_fd{-1};
  std::vector<ControlDesc> m_controls;
};

/// Turns a driver-provided control name into a tree-safe slug: lowercase,
/// non-alphanumerics collapsed to underscores. Exposed for testing.
SCORE_ADDON_VIDEOIO_EXPORT std::string controlSlug(const std::string& name);

} // namespace Gfx::V4L2
