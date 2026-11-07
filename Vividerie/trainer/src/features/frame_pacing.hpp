#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <safetyhook.hpp>

#include "game/runtime.hpp"

namespace vividerie_mod::features
{
    // Runs game logic at its own tick rate while drawing every frame the display allows;
    // between ticks, instances and the view are drawn extrapolated along their last tick's motion.
    class frame_pacing final
    {
      public:
        static void install();
        [[nodiscard]] static const std::string& status();

      private:
        struct view
        {
            double x;
            double y;
        };

        // a position as of the last tick, its per-tick velocity, and what was drawn this frame
        struct motion
        {
            view pos;
            view vel;
            view written;
            bool applied;
        };

        struct tracked
        {
            std::uint32_t id;
            motion body;                               // instance x/y
            bool has_tdmc; // has a TDMC mover struct, re-read each use since it may be replaced or collected
            motion draw;   // tdmc.drawX/drawY
        };

        struct camera_track
        {
            bool valid;
            view cur;
            view vel;
            bool applied;
            view written;
        };

        struct displacement
        {
            view before;
            view after;
        };

        static constexpr double UNCAPPED_RATE = 1000.0;
        static constexpr double MAX_FRAME_SECONDS = 0.25;

        static std::int64_t qpc();
        static void frame_hook();
        static void io_update_hook();
        static void update_hook();
        static double get_frame_rate_hook(void* timer);
        static void game_set_speed_hook(game::runtime::rvalue* result, game::runtime::instance* self,
                                        game::runtime::instance* other, int argc, game::runtime::rvalue* args);
        static bool room_speed_set_hook(game::runtime::instance* self, int array_index, game::runtime::rvalue* value);
        static bool delta_time_get_hook(game::runtime::instance* self, int array_index, game::runtime::rvalue* out);

        static void begin_frame();
        static void measure(std::int64_t now);
        static void apply_rate(double rate);
        static void snapshot_after_tick(const std::unordered_map<game::runtime::instance*, tracked>& before);
        static void extrapolate();
        static void restore();
        [[nodiscard]] static bool advance(motion& m, view current);
        [[nodiscard]] static std::optional<view> retreat(motion& m, view current);
        [[nodiscard]] static bool same_position(view a, view b);
        [[nodiscard]] static view tick_velocity(displacement d);
        [[nodiscard]] static view body_position(game::runtime::instance* inst);
        static void set_body_position(game::runtime::instance* inst, view pos);
        [[nodiscard]] static std::optional<game::runtime::rvalue> tdmc_of(game::runtime::instance* inst);
        [[nodiscard]] static std::optional<view> draw_position(game::runtime::rvalue tdmc);
        static void set_draw_position(game::runtime::rvalue tdmc, view pos);
        [[nodiscard]] static std::optional<view> view_position(game::runtime::rvalue cam);
        static void set_view_position(game::runtime::rvalue cam, view pos);
        [[nodiscard]] static bool active();
        [[nodiscard]] static bool called_from_main_loop(std::uintptr_t return_address);

        static inline safetyhook::InlineHook frame_;
        static inline safetyhook::InlineHook io_update_;
        static inline safetyhook::InlineHook update_;
        static inline safetyhook::InlineHook get_frame_rate_;
        static inline safetyhook::InlineHook game_set_speed_;
        static inline safetyhook::InlineHook room_speed_set_;
        static inline safetyhook::InlineHook delta_time_get_;

        static inline game::runtime::builtin_fn camera_get_view_x_ = nullptr;
        static inline game::runtime::builtin_fn camera_get_view_y_ = nullptr;
        static inline game::runtime::builtin_fn camera_set_view_pos_ = nullptr;
        static inline game::runtime::builtin_fn variable_instance_get_ = nullptr;
        static inline game::runtime::gml_string draw_x_name_{"drawX"};
        static inline game::runtime::gml_string draw_y_name_{"drawY"};
        static inline std::uintptr_t main_loop_ = 0;
        static inline std::unordered_map<std::uintptr_t, bool> main_loop_callers_;

        static inline std::string status_ = "not installed";
        static inline double tick_hz_ = 0.0;      // the game's logical speed
        static inline double applied_rate_ = 0.0; // what the frame limiter runs at
        static inline std::int64_t qpc_freq_ = 0;
        static inline std::int64_t last_qpc_ = 0;
        static inline std::int64_t last_tick_qpc_ = 0;
        static inline double accumulator_ = 0.0;
        static inline double alpha_ = 0.0;
        static inline double tick_delta_us_ = 0.0;
        static inline bool tick_ = true;
        static inline bool in_update_ = false;
        static inline std::int64_t window_start_ = 0;
        static inline int frames_ = 0;
        static inline int ticks_ = 0;
        static inline int last_written_ = 0;
        static inline int written_stat_ = 0;

        static inline std::unordered_map<game::runtime::instance*, tracked> tracked_;
        static inline camera_track camera_{};
    };
} // namespace vividerie_mod::features
