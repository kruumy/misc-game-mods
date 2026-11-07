#include <cstdio>
#include <exception>
#include <format>
#include <initializer_list>
#include <string>

#include <Windows.h>

#include "game/image.hpp"
#include "game/runtime.hpp"

namespace vividerie_mod::tests::resolve_test
{
    namespace rt = game::runtime;

    // expected values were found by hand in this exact Vividerie.exe build
    static int run(const wchar_t* exe)
    {
        auto* mod = LoadLibraryExW(exe, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (mod == nullptr)
        {
            std::fputs("cannot map exe\n", stderr);
            return 2;
        }
        game::image::use_module(mod);
        const auto base = reinterpret_cast<std::uintptr_t>(mod);
        const auto rva = [&](auto p) { return std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(p) - base + 0x140000000); };

        int failures = 0;
        const auto check = [&](const char* what, const std::string& expected, auto&& fn) {
            try
            {
                const auto actual = fn();
                failures += actual == expected ? 0 : 1;
                std::puts(std::format("{:<22} {} {}", what, actual == expected ? "ok  " : "BAD ", actual).c_str());
                if (actual != expected)
                {
                    std::puts(std::format("{:<22}      expected {}", "", expected).c_str());
                }
            }
            catch (const std::exception& e)
            {
                ++failures;
                std::puts(std::format("{:<22} FAILED: {}", what, e.what()).c_str());
            }
        };

        check("x get/set", "0x140f90940 0x140f946f0", [&] {
            const auto v = rt::find_variable("x");
            return rva(v.get) + " " + rva(v.set);
        });
        check("room_speed get/set", "0x140f8fd80 0x140f939b0", [&] {
            const auto v = rt::find_variable("room_speed");
            return rva(v.get) + " " + rva(v.set);
        });
        check("skill_use", "0x140872280", [&] { return rva(rt::find_script("gml_Script_skill_use")); });
        check("runner", "main 0x140f98020 frame 0x140f96e50 io 0x141061e90 update 0x140f97000 rate 0x140f819c0", [&] {
            const auto& r = rt::runner();
            return std::format("main {} frame {} io {} update {} rate {}", rva(r.main_loop), rva(r.frame), rva(r.io_update),
                               rva(r.update), rva(r.get_frame_rate));
        });
        check("layout", "x ec y f0 obj b8 id b4 flags b0 mask 100003", [&] {
            const auto& l = rt::layout();
            return std::format("x {:x} y {:x} obj {:x} id {:x} flags {:x} mask {:x}", l.x, l.y, l.object_index, l.id, l.flags,
                               l.skip_mask);
        });
        check("iterator", "init 0x140f5af90 get 0x140f5b1d0 advance 0x140f5c870", [&] {
            const auto& f = rt::iterator_functions();
            return std::format("init {} get {} advance {}", rva(f.init), rva(f.get), rva(f.advance));
        });

        struct named
        {
            const char* name;
            const char* expected;
        };
        for (const auto& [name, expected] : std::initializer_list<named>{
                 {"game_set_speed", "0x140ea10a0"},
                 {"game_get_speed", "0x140ea1040"},
                 {"instance_find", "0x140f007e0"},
                 {"variable_global_get", "0x140ea2ae0"},
                 {"variable_global_set", "0x140ea2bb0"},
                 {"variable_instance_get", "0x140ea3150"},
                 {"variable_struct_get", "0x140ea3150"},
                 {"variable_struct_set", "0x140ea3830"},
                 {"object_is_ancestor", "0x140f23110"},
                 {"view_get_camera", "0x140f3e680"},
                 {"camera_get_view_x", "0x140f3de40"},
                 {"camera_set_view_pos", "0x140f3e430"},
                 {"camera_get_view_width", "0x140f3ddd0"},
                 {"asset_get_index", "0x140f24890"},
             })
        {
            check(name, expected, [&] { return rva(rt::find_function(name)); });
        }
        for (const auto& [name, expected] : std::initializer_list<named>{
                 {"delta_time", "0x140f8e5d0"},
                 {"mouse_x", "0x140f8ec70"},
                 {"mouse_y", "0x140f8ecf0"},
                 {"bbox_left", "0x140f8df50"},
                 {"bbox_right", "0x140f8dfa0"},
                 {"bbox_top", "0x140f8dff0"},
                 {"bbox_bottom", "0x140f8df00"},
                 {"visible", "0x140f908d0"},
                 {"object_index", "0x140f8edb0"},
                 {"y", "0x140f909a0"},
             })
        {
            check(name, expected, [&] { return rva(rt::find_variable(name).get); });
        }
        return failures == 0 ? 0 : 1;
    }
} // namespace vividerie_mod::tests::resolve_test

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fputs("usage: resolve_test <Vividerie.exe>\n", stderr);
        return 2;
    }
    return vividerie_mod::tests::resolve_test::run(argv[1]);
}
