#include <exception>
#include <format>
#include <utility>

#include <Windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include "features/autoaim.hpp"
#include "features/frame_pacing.hpp"
#include "settings.hpp"

extern "C" __declspec(dllexport) const char* NAME = "Vividerie Mod";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Renders above the 60 Hz game tick with extrapolated motion, and aims at enemies while shooting.";

namespace vividerie_mod
{
    template <typename F> static void install_feature(const char* name, F&& install)
    {
        try
        {
            install();
            reshade::log::message(reshade::log::level::info, std::format("{} installed", name).c_str());
        }
        catch (const std::exception& e)
        {
            // a game update broke a lookup: leave the game untouched for this feature
            reshade::log::message(reshade::log::level::error, std::format("{} disabled: {}", name, e.what()).c_str());
        }
    }

    // hooks go in on the game's own thread, between frames of its render loop
    static void on_present(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
                           const reshade::api::rect*, uint32_t, const reshade::api::rect*)
    {
        static bool installed = false;
        if (!std::exchange(installed, true))
        {
            settings::get().load();
            install_feature("frame pacing", &features::frame_pacing::install);
            install_feature("autoaim", &features::autoaim::install);
        }
        features::autoaim::next_frame();
    }

    static void attach()
    {
        reshade::register_event<reshade::addon_event::present>(&on_present);
        reshade::register_overlay("Vividerie Mod", &settings::draw_overlay);
    }
} // namespace vividerie_mod

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
        {
            return FALSE;
        }
        vividerie_mod::attach();
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    default:
        break;
    }
    return TRUE;
}
