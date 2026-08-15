#pragma once

/**
 * @file V4L2ControlTree.hpp
 * @brief Publish a V4L2 device's controls as `<cam>/controls/<name>`.
 *
 * Joins the driver-side enumeration in V4L2Controls to the device tree. Every
 * control the driver publishes becomes a parameter, whatever the camera is:
 * nothing here names a specific control, so a webcam gets its focus and zoom
 * and a sensor gets its black level and sync mode with no per-device code.
 *
 * The tree tracks changes it did not make. V4L2_EVENT_CTRL reports writes from
 * other processes, controls becoming inactive because an `auto` was toggled,
 * and ranges moving underneath -- an Orin NX bounds `exposure` by `frame_rate`,
 * so setting one silently reshapes the other. Without that the explorer would
 * show a value the hardware stopped holding.
 *
 * Two rules the implementation exists to obey:
 *
 *  - **Never push into a parameter from inside that parameter's callback.**
 *    `callback_container::send` holds a non-recursive mutex while it runs
 *    callbacks, so a corrective push made inline deadlocks the calling thread
 *    -- the GUI, in practice. Corrections are posted to the Qt thread instead.
 *  - **Writes can arrive on any thread.** The explorer, OSC and the execution
 *    engine all reach a parameter, while driver events arrive on the Qt thread,
 *    so every touch of the ControlSet is serialised.
 */

#include <v4l2/V4L2Controls.hpp>

#include <ossia/network/base/device.hpp>
#include <ossia/network/base/node.hpp>
#include <ossia/network/base/parameter.hpp>

#include <QObject>

#include <score_addon_videoio_export.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class QSocketNotifier;

namespace Gfx::V4L2
{

class SCORE_ADDON_VIDEOIO_EXPORT ControlTree
{
public:
  /// Enumerates @p devicePath and builds `<parent>/<group>/...`.
  ControlTree(
      const std::string& devicePath, ossia::net::device_base& dev,
      ossia::net::node_base& parent, std::string group = "controls");
  ~ControlTree();

  ControlTree(const ControlTree&) = delete;
  ControlTree& operator=(const ControlTree&) = delete;

  /// False when the device could not be opened; the tree is then empty and the
  /// camera still streams, since controls are not required for capture.
  bool valid() const noexcept { return m_open; }

  std::size_t count() const noexcept { return m_params.size(); }

private:
  void build(ossia::net::device_base& dev, ossia::net::node_base& parent,
             const std::string& group);
  void write(std::uint32_t id, const ossia::value& v);
  void drainEvents();
  void applyEvent(const ControlSet::Event& e);
  void publish(std::uint32_t id, std::int64_t raw);
  ossia::net::parameter_base* paramFor(std::uint32_t id) const noexcept;

  /// Serialises every ControlSet touch: writes arrive on the caller's thread,
  /// events on the Qt thread.
  mutable std::mutex m_deviceLock;
  ControlSet m_set;
  bool m_open{};

  /// Fixed after build(), so it is readable without the lock.
  std::vector<std::pair<std::uint32_t, ossia::net::parameter_base*>> m_params;

  /// The thread currently pushing a driver-sourced value, so that thread's
  /// re-entrant callback can tell "this came from the hardware" from "a user
  /// wrote this". A plain flag would also silence a genuine write arriving on
  /// another thread at the same moment.
  std::atomic<std::thread::id> m_echoing{};

  /// Owns the queued corrections, and cancels the pending ones when destroyed.
  QObject m_context;

  std::unique_ptr<QSocketNotifier> m_notifier;
};

} // namespace Gfx::V4L2
