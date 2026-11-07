#pragma once

namespace reshade::api
{
    struct effect_runtime;
}

namespace vividerie_mod
{
    struct settings
    {
        bool decouple_render = true;
        int fps_cap = 0; // 0 = uncapped
        bool extrapolate = true;
        float max_step = 64.0f; // room px per tick; faster motion is treated as a teleport
        bool autoaim = true;
        float autoaim_radius = 0.0f; // room px around the aim point; 0 = any on-screen enemy

        [[nodiscard]] static settings& get();
        void load();
        void save() const;

        static void draw_overlay(reshade::api::effect_runtime* runtime);

      private:
        static constexpr const char* SECTION = "VIVIDERIE_MOD";
    };
} // namespace vividerie_mod
