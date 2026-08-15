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
 */

#include <v4l2/V4L2Controls.hpp>

#include <ossia/network/base/device.hpp>
#include <ossia/network/base/node.hpp>
#include <ossia/network/base/parameter.hpp>

#include <score_addon_videoio_export.h>

#include <cstdint>
#include <memory>
#include <string>
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
  bool valid() const noexcept { return m_set.isOpen(); }

  std::size_t count() const noexcept { return m_set.controls().size(); }

private:
  void build(ossia::net::device_base& dev, ossia::net::node_base& parent,
             const std::string& group);
  ossia::net::parameter_base* paramFor(std::uint32_t id) const noexcept;
  void onDriverEvent(const ControlSet::Event& e);

  ControlSet m_set;
  std::vector<ossia::net::parameter_base*> m_params;

  /// Set while pushing a driver-sourced value into a parameter, so the
  /// parameter's own callback does not write it straight back to the driver.
  std::shared_ptr<bool> m_echo;

  std::unique_ptr<QSocketNotifier> m_notifier;
};

} // namespace Gfx::V4L2
