#include "settings.hpp"

#include <imgui.h>
#include <reshade.hpp>

#include "features/autoaim.hpp"
#include "features/frame_pacing.hpp"

namespace vividerie_mod
{
    settings& settings::get()
    {
        static settings s;
        return s;
    }

    void settings::load()
    {
        reshade::get_config_value(nullptr, SECTION, "DecoupleRender", decouple_render);
        reshade::get_config_value(nullptr, SECTION, "FpsCap", fps_cap);
        reshade::get_config_value(nullptr, SECTION, "Extrapolate", extrapolate);
        reshade::get_config_value(nullptr, SECTION, "MaxStep", max_step);
        reshade::get_config_value(nullptr, SECTION, "Autoaim", autoaim);
        reshade::get_config_value(nullptr, SECTION, "AutoaimRadius", autoaim_radius);
    }

    void settings::save() const
    {
        reshade::set_config_value(nullptr, SECTION, "DecoupleRender", decouple_render);
        reshade::set_config_value(nullptr, SECTION, "FpsCap", fps_cap);
        reshade::set_config_value(nullptr, SECTION, "Extrapolate", extrapolate);
        reshade::set_config_value(nullptr, SECTION, "MaxStep", max_step);
        reshade::set_config_value(nullptr, SECTION, "Autoaim", autoaim);
        reshade::set_config_value(nullptr, SECTION, "AutoaimRadius", autoaim_radius);
    }

    void settings::draw_overlay(reshade::api::effect_runtime*)
    {
        auto& s = get();
        bool changed = false;

        ImGui::TextUnformatted("High FPS");
        ImGui::TextDisabled("%s", features::frame_pacing::status().c_str());
        changed |= ImGui::Checkbox("Render above tick rate", &s.decouple_render);
        changed |= ImGui::SliderInt("FPS cap (0 = uncapped)", &s.fps_cap, 0, 500);
        changed |= ImGui::Checkbox("Extrapolate motion", &s.extrapolate);
        changed |= ImGui::SliderFloat("Teleport threshold (px/tick)", &s.max_step, 8.0f, 256.0f, "%.0f");

        ImGui::Separator();
        ImGui::TextUnformatted("Autoaim");
        ImGui::TextDisabled("%s", features::autoaim::status().c_str());
        changed |= ImGui::Checkbox("Aim at enemies while firing", &s.autoaim);
        changed |= ImGui::SliderFloat("Radius around aim point (0 = screen)", &s.autoaim_radius, 0.0f, 600.0f, "%.0f");

        if (changed)
        {
            s.save();
        }
    }
} // namespace vividerie_mod
