#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <safetyhook.hpp>

#include "game/runtime.hpp"

namespace vividerie_mod::features
{
    // While the player is firing its primary skill, shots and the shooter's mouse_x/mouse_y (which drive its aim)
    // point at the centre of the enemy nearest to the real aim point instead.
    class autoaim final
    {
      public:
        static void install();
        // call once per presented frame; mouse_x and mouse_y share one target within a frame
        static void next_frame();
        [[nodiscard]] static const std::string& status();

      private:
        struct point
        {
            double x;
            double y;
        };

        struct rect
        {
            double x;
            double y;
            double width;
            double height;

            [[nodiscard]] bool contains(point p) const
            {
                return p.x >= x && p.x <= x + width && p.y >= y && p.y <= y + height;
            }
        };

        static constexpr std::array ENEMY_PARENTS{"o_enemy_parent", "o_boss_parent"};
        static constexpr std::uint64_t MIN_FIRING_WINDOW_MS = 250;
        static constexpr std::uint64_t MAX_FIRING_WINDOW_MS = 1000;

        static bool mouse_x_hook(game::runtime::instance* self, int array_index, game::runtime::rvalue* out);
        static bool mouse_y_hook(game::runtime::instance* self, int array_index, game::runtime::rvalue* out);

        static game::runtime::rvalue* skill_use_hook(game::runtime::instance* self, game::runtime::instance* other,
                                                     game::runtime::rvalue* result, int argc, game::runtime::rvalue** args);

        [[nodiscard]] static std::optional<point> target_for_shooter();
        [[nodiscard]] static std::optional<point> real_cursor(game::runtime::instance* self);

        struct primary_state
        {
            double stacks;
            double current;
        };
        [[nodiscard]] static std::optional<primary_state> read_primary_state();
        static void mark_firing();
        static void aim_shot(game::runtime::instance* self);
        [[nodiscard]] static std::optional<point> find_target(point cursor);
        [[nodiscard]] static bool is_enemy(std::int32_t object_index);
        [[nodiscard]] static bool shooting();
        [[nodiscard]] static std::optional<point> bbox_centre(game::runtime::instance* inst);
        [[nodiscard]] static std::optional<rect> view_rect();

        static inline safetyhook::InlineHook mouse_x_;
        static inline safetyhook::InlineHook mouse_y_;
        static inline safetyhook::InlineHook skill_use_;
        static inline game::runtime::builtin_fn variable_global_get_ = nullptr;
        static inline std::optional<double> primary_skill_; // skill_use argument that means "primary", learned in play
        static inline std::unordered_map<double, int> primary_votes_; // primary-cooldown changes seen per skill
        static inline game::runtime::instance* shooter_ = nullptr;     // whose aim is overridden
        static inline game::runtime::builtin_fn variable_global_set_ = nullptr;
        static inline std::uint64_t last_fire_ms_ = 0;

        static inline game::runtime::builtin_fn object_is_ancestor_ = nullptr;
        static inline game::runtime::builtin_fn camera_get_view_x_ = nullptr;
        static inline game::runtime::builtin_fn camera_get_view_y_ = nullptr;
        static inline game::runtime::builtin_fn camera_get_view_width_ = nullptr;
        static inline game::runtime::builtin_fn camera_get_view_height_ = nullptr;
        static inline game::runtime::getter_fn bbox_left_ = nullptr;
        static inline game::runtime::getter_fn bbox_right_ = nullptr;
        static inline game::runtime::getter_fn bbox_top_ = nullptr;
        static inline game::runtime::getter_fn bbox_bottom_ = nullptr;
        static inline game::runtime::getter_fn visible_ = nullptr;

        static inline std::vector<game::runtime::rvalue> enemy_parents_;
        static inline std::vector<std::int8_t> enemy_cache_; // -1 unknown, 0 no, 1 yes
        static inline std::optional<point> cached_;
        static inline bool cache_valid_ = false;
        static inline std::string status_ = "not installed";
        static inline std::uint64_t window_start_ = 0;
        static inline int locks_ = 0;
        static inline std::uint64_t firing_window_ms_ = 250;
        static inline int shooting_queries_ = 0;
        static inline std::string last_shot_ = "not fired yet";
        static inline int last_enemies_ = 0;
    };
} // namespace vividerie_mod::features
