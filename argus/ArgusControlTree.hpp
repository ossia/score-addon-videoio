#pragma once

/**
 * @file ArgusControlTree.hpp
 * @brief `<stream>/controls` for an Argus sensor.
 *
 * The V4L2 backend gets this group for free: the driver publishes its controls
 * and V4L2::ControlTree walks them. Argus publishes nothing to walk -- the
 * knobs are C++ setters on a Request -- so the equivalent group has to be
 * written out by hand, which is what this is.
 *
 * Only what libargus accepts on a running capture is here. Sensor mode,
 * geometry and frame duration are settings, not controls: changing them means
 * rebuilding the streams, so they stay in the device's settings dialog.
 *
 * Under Argus the ISP owns auto-exposure and auto-white-balance. Setting an
 * exposure time or a gain does not by itself take AE out of the loop -- lock it
 * (`ae_lock`) or the loop will drive the value back.
 */

#include <argus/ArgusSettings.hpp>

#include <Gfx/ControlTree.hpp>

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>

namespace Gfx::Argus
{

/**
 * @brief Builds `<parent>/controls` for one Argus sensor.
 *
 * Holds the working copy the controls edit, exactly like CaptureControlTree:
 * each control owns one field, so a write is a read-modify-write of the whole
 * struct and has to be serialised -- two controls written at once from
 * different threads would otherwise lose one of the two edits.
 *
 * @p apply is called with the updated settings on the writing thread. It is
 * what reaches the session; nothing here knows whether one exists yet, so a
 * control written before the capture starts simply updates the struct that
 * open() will use.
 */
class ArgusControlTree
{
public:
  ArgusControlTree(
      const ArgusSettings& initial, std::function<bool(const ArgusSettings&)> apply,
      ossia::net::device_base& dev, ossia::net::node_base& parent,
      std::string group = "controls")
      : m_value{initial}
      , m_apply{std::move(apply)}
  {
    auto edit = [this](auto&& fn) {
      ArgusSettings copy;
      {
        std::lock_guard g{m_mutex};
        fn(m_value);
        copy = m_value;
      }
      if(m_apply)
        m_apply(copy);
    };

    std::vector<TreeControl> c;

    // --- exposure ----------------------------------------------------------
    {
      TreeControl t;
      t.name = "exposure_time";
      t.description
          = "Sensor exposure time in nanoseconds. Pinned as a one-value range, "
            "so auto-exposure has nothing left to choose; it is still the AE "
            "loop that applies it unless ae_lock is on. 0 hands the range back "
            "to the sensor default.";
      t.type = ossia::val_type::INT;
      t.initial = int(initial.exposureTimeNs.set ? initial.exposureTimeNs.min : 0);
      t.onSet = [edit](const ossia::value& v) {
        if(auto n = v.target<int>())
          edit([n](ArgusSettings& s) {
            s.exposureTimeNs.set = (*n > 0);
            s.exposureTimeNs.min = s.exposureTimeNs.max = double(*n);
          });
      };
      c.push_back(std::move(t));
    }
    {
      TreeControl t;
      t.name = "gain";
      t.description = "Analogue sensor gain, pinned as a one-value range. The "
                      "sensor clamps to what its active mode allows. 0 restores "
                      "the sensor default range.";
      t.type = ossia::val_type::FLOAT;
      t.initial = float(initial.gain.set ? initial.gain.min : 0.f);
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) {
            s.gain.set = (*f > 0.f);
            s.gain.min = s.gain.max = double(*f);
          });
      };
      c.push_back(std::move(t));
    }
    {
      TreeControl t;
      t.name = "isp_digital_gain";
      t.description = "Digital gain applied by the ISP, after the sensor. 0 "
                      "leaves it to the ISP.";
      t.type = ossia::val_type::FLOAT;
      t.initial
          = float(initial.ispDigitalGain.set ? initial.ispDigitalGain.min : 0.f);
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) {
            s.ispDigitalGain.set = (*f > 0.f);
            s.ispDigitalGain.min = s.ispDigitalGain.max = double(*f);
          });
      };
      c.push_back(std::move(t));
    }
    {
      TreeControl t;
      t.name = "exposure_compensation";
      t.description = "Stops of exposure bias for the AE loop, -2 to 2. Has no "
                      "effect once ae_lock is on.";
      t.type = ossia::val_type::FLOAT;
      t.domain = ossia::make_domain(-2.f, 2.f);
      t.initial = initial.exposureCompensation;
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) { s.exposureCompensation = *f; });
      };
      c.push_back(std::move(t));
    }

    // --- the loops themselves ----------------------------------------------
    {
      TreeControl t;
      t.name = "ae_lock";
      t.description = "Freeze auto-exposure where it is. This is what makes a "
                      "manual exposure_time or gain stay put.";
      t.type = ossia::val_type::BOOL;
      t.initial = initial.aeLock;
      t.onSet = [edit](const ossia::value& v) {
        if(auto b = v.target<bool>())
          edit([b](ArgusSettings& s) { s.aeLock = *b; });
      };
      c.push_back(std::move(t));
    }
    {
      TreeControl t;
      t.name = "awb_lock";
      t.description = "Freeze auto-white-balance where it is.";
      t.type = ossia::val_type::BOOL;
      t.initial = initial.awbLock;
      t.onSet = [edit](const ossia::value& v) {
        if(auto b = v.target<bool>())
          edit([b](ArgusSettings& s) { s.awbLock = *b; });
      };
      c.push_back(std::move(t));
    }
    {
      static const std::vector<std::string> awb{
          "Off",         "Auto",           "Incandescent", "Fluorescent",
          "WarmFluorescent", "Daylight",   "CloudyDaylight", "Twilight",
          "Shade",       "Manual"};
      TreeControl t;
      t.name = "awb_mode";
      t.description = "White-balance preset.";
      t.type = ossia::val_type::STRING;
      t.domain = ossia::make_domain(awb);
      t.initial = awb[std::min<std::size_t>(std::size_t(initial.awbMode), awb.size() - 1)];
      t.onSet = [edit](const ossia::value& v) {
        const auto s = v.target<std::string>();
        if(!s)
          return;
        // A name we do not know leaves the mode alone: a client with a typo
        // must not silently reset white balance to Off.
        for(std::size_t i = 0; i < awb.size(); i++)
          if(awb[i] == *s)
          {
            edit([i](ArgusSettings& st) { st.awbMode = AwbMode(i); });
            return;
          }
      };
      c.push_back(std::move(t));
    }
    {
      static const std::vector<std::string> ab{"Off", "Auto", "50Hz", "60Hz"};
      TreeControl t;
      t.name = "ae_antibanding";
      t.description = "Match the exposure to mains frequency so lighting does "
                      "not beat against the shutter.";
      t.type = ossia::val_type::STRING;
      t.domain = ossia::make_domain(ab);
      t.initial = ab[std::min<std::size_t>(std::size_t(initial.aeAntibanding), ab.size() - 1)];
      t.onSet = [edit](const ossia::value& v) {
        const auto s = v.target<std::string>();
        if(!s)
          return;
        for(std::size_t i = 0; i < ab.size(); i++)
          if(ab[i] == *s)
          {
            edit([i](ArgusSettings& st) { st.aeAntibanding = AeAntibanding(i); });
            return;
          }
      };
      c.push_back(std::move(t));
    }

    // --- ISP picture controls ----------------------------------------------
    {
      TreeControl t;
      t.name = "saturation";
      t.description = "Colour saturation, 0 to 2. Writing it also switches the "
                      "ISP from its own saturation to this one.";
      t.type = ossia::val_type::FLOAT;
      t.domain = ossia::make_domain(0.f, 2.f);
      t.initial = initial.saturation;
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) {
            s.saturation = *f;
            s.saturationSet = true;
          });
      };
      c.push_back(std::move(t));
    }
    addQuality(c, "denoise_mode", "Noise reduction: Off, Fast, HighQuality.",
               initial.denoiseMode,
               [edit](Quality q) { edit([q](ArgusSettings& s) { s.denoiseMode = q; }); });
    {
      TreeControl t;
      t.name = "denoise_strength";
      t.description = "0 to 1. Negative leaves the strength to the ISP.";
      t.type = ossia::val_type::FLOAT;
      t.domain = ossia::make_domain(-1.f, 1.f);
      t.initial = initial.denoiseStrength;
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) { s.denoiseStrength = *f; });
      };
      c.push_back(std::move(t));
    }
    addQuality(c, "edge_enhance_mode", "Edge enhancement: Off, Fast, HighQuality.",
               initial.edgeEnhanceMode, [edit](Quality q) {
                 edit([q](ArgusSettings& s) { s.edgeEnhanceMode = q; });
               });
    {
      TreeControl t;
      t.name = "edge_enhance_strength";
      t.description = "0 to 1. Negative leaves the strength to the ISP.";
      t.type = ossia::val_type::FLOAT;
      t.domain = ossia::make_domain(-1.f, 1.f);
      t.initial = initial.edgeEnhanceStrength;
      t.onSet = [edit](const ossia::value& v) {
        if(auto f = v.target<float>())
          edit([f](ArgusSettings& s) { s.edgeEnhanceStrength = *f; });
      };
      c.push_back(std::move(t));
    }

    m_params = addControlGroup(dev, parent, group, c);
    m_count = c.size();
  }

  ~ArgusControlTree()
  {
    // The parameters outlive this object -- the device owns them and is
    // destroyed after -- and their callbacks capture `this` through `edit`.
    // Cut the link before it can dangle.
    for(auto* p : m_params)
      if(p)
        p->callbacks_clear();
  }

  ArgusControlTree(const ArgusControlTree&) = delete;
  ArgusControlTree& operator=(const ArgusControlTree&) = delete;

  std::size_t count() const noexcept { return m_count; }

private:
  template <typename F>
  static void addQuality(
      std::vector<TreeControl>& c, const char* name, const char* desc, Quality initial,
      F set)
  {
    static const std::vector<std::string> q{"Off", "Fast", "HighQuality"};
    TreeControl t;
    t.name = name;
    t.description = desc;
    t.type = ossia::val_type::STRING;
    t.domain = ossia::make_domain(q);
    t.initial = q[std::min<std::size_t>(std::size_t(initial), q.size() - 1)];
    t.onSet = [set](const ossia::value& v) {
      const auto s = v.target<std::string>();
      if(!s)
        return;
      for(std::size_t i = 0; i < q.size(); i++)
        if(q[i] == *s)
        {
          set(Quality(i));
          return;
        }
    };
    c.push_back(std::move(t));
  }

  std::mutex m_mutex;
  ArgusSettings m_value;
  std::function<bool(const ArgusSettings&)> m_apply;
  std::vector<ossia::net::parameter_base*> m_params;
  std::size_t m_count{};
};

}
