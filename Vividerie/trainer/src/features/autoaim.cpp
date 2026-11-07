#include "features/autoaim.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <stdexcept>

#include <Windows.h>

#include "game/hook.hpp"
#include "settings.hpp"

namespace vividerie_mod::features
{
    namespace rt = game::runtime;

    void autoaim::install()
    {
        (void)rt::layout();
        (void)rt::iterator_functions();
        (void)rt::helpers();
        object_is_ancestor_ = rt::find_function("object_is_ancestor");
        camera_get_view_x_ = rt::find_function("camera_get_view_x");
        camera_get_view_y_ = rt::find_function("camera_get_view_y");
        camera_get_view_width_ = rt::find_function("camera_get_view_width");
        camera_get_view_height_ = rt::find_function("camera_get_view_height");
        bbox_left_ = rt::find_variable("bbox_left").get;
        bbox_right_ = rt::find_variable("bbox_right").get;
        bbox_top_ = rt::find_variable("bbox_top").get;
        bbox_bottom_ = rt::find_variable("bbox_bottom").get;
        visible_ = rt::find_variable("visible").get;
        variable_global_get_ = rt::find_function("variable_global_get");
        variable_global_set_ = rt::find_function("variable_global_set");
        const auto skill_use = rt::find_script("gml_Script_skill_use");

        const auto asset_get_index = rt::find_function("asset_get_index");
        for (const auto* name : ENEMY_PARENTS)
        {
            rt::gml_string str{name};
            std::array args{str.value()};
            const auto parent = rt::call(asset_get_index, args);
            if (parent.number().value_or(-1.0) >= 0.0)
            {
                enemy_parents_.push_back(parent);
            }
        }
        if (enemy_parents_.empty())
        {
            throw std::runtime_error("no enemy parent objects found");
        }

        // the player's aim (mouse, or the right stick via window_mouse_set) is read through mouse_x/mouse_y
        const auto mouse_x = rt::find_variable("mouse_x").get;
        const auto mouse_y = rt::find_variable("mouse_y").get;
        // all or nothing: a throw unhooks the ones already created
        auto mouse_x_hook_ = game::hook::create(mouse_x, &mouse_x_hook);
        auto mouse_y_hook_ = game::hook::create(mouse_y, &mouse_y_hook);
        auto skill_use_hook_ = game::hook::create(skill_use, &skill_use_hook);
        mouse_x_ = std::move(mouse_x_hook_);
        mouse_y_ = std::move(mouse_y_hook_);
        skill_use_ = std::move(skill_use_hook_);
        status_ = std::format("ok, {} enemy parent object(s)", enemy_parents_.size());
    }

    void autoaim::next_frame()
    {
        cache_valid_ = false;
    }

    const std::string& autoaim::status()
    {
        return status_;
    }

    bool autoaim::shooting()
    {
        // skill_use(primary) runs once per shot; stay locked until a shot is clearly overdue
        return GetTickCount64() - last_fire_ms_ <= firing_window_ms_;
    }

    std::optional<autoaim::primary_state> autoaim::read_primary_state()
    {
        // global.run.cooldowns.primary_* only change when the primary skill is used
        static rt::gml_string run_name{"run"};
        static rt::gml_string cooldowns_name{"cooldowns"};
        static rt::gml_string stacks_name{"primary_stacks_current"};
        static rt::gml_string current_name{"primary_current"};
        std::array run_args{run_name.value()};
        const auto run = rt::call(variable_global_get_, run_args);
        if (run.type() != rt::kind::object)
        {
            return std::nullopt;
        }
        const auto cooldowns = rt::struct_get(run, cooldowns_name);
        if (cooldowns.type() != rt::kind::object)
        {
            return std::nullopt;
        }
        const auto stacks = rt::struct_get(cooldowns, stacks_name).number();
        const auto current = rt::struct_get(cooldowns, current_name).number();
        if (!stacks && !current)
        {
            return std::nullopt;
        }
        return primary_state{stacks.value_or(0.0), current.value_or(0.0)};
    }

    void autoaim::mark_firing()
    {
        const auto now = GetTickCount64();
        const auto interval = now - last_fire_ms_;
        firing_window_ms_ = std::clamp<std::uint64_t>(interval + interval / 2, MIN_FIRING_WINDOW_MS, MAX_FIRING_WINDOW_MS);
        last_fire_ms_ = now;
        cache_valid_ = false;
    }

    void autoaim::aim_shot(rt::instance* self)
    {
        // skills shoot along global.direction_aim, so this shot is aimed even before the mouse override kicks in
        const auto cursor = real_cursor(self);
        const auto target = cursor ? find_target(*cursor) : std::nullopt;
        if (!target)
        {
            return;
        }
        const auto& lay = rt::layout();
        const double dx = target->x - rt::field<float>(self, lay.x);
        const double dy = target->y - rt::field<float>(self, lay.y);
        const double degrees = std::fmod(std::atan2(-dy, dx) * 180.0 / std::numbers::pi + 360.0, 360.0);
        static rt::gml_string aim_name{"direction_aim"};
        std::array aim_args{aim_name.value(), rt::rvalue::make_real(degrees)};
        (void)rt::call(variable_global_set_, aim_args);
    }

    std::optional<autoaim::point> autoaim::real_cursor(rt::instance* self)
    {
        auto x = rt::rvalue::make_real(0);
        auto y = rt::rvalue::make_real(0);
        if (!mouse_x_.call<bool>(self, rt::NO_ARRAY_INDEX, &x) || !mouse_y_.call<bool>(self, rt::NO_ARRAY_INDEX, &y))
        {
            return std::nullopt;
        }
        return point{x.number().value_or(0.0), y.number().value_or(0.0)};
    }

    rt::rvalue* autoaim::skill_use_hook(rt::instance* self, rt::instance* other, rt::rvalue* result, int argc,
                                        rt::rvalue** args)
    {
        const auto skill = argc >= 1 ? args[0]->number() : std::nullopt;
        if (!skill || !settings::get().autoaim)
        {
            return skill_use_.call<rt::rvalue*>(self, other, result, argc, args);
        }
        const bool primary = primary_skill_ == skill;
        if (primary)
        {
            shooter_ = self;
            mark_firing();
            aim_shot(self);
        }
        // the primary is the skill that most often touches the primary cooldown; a one-off reset can't outvote it
        const auto before = read_primary_state();
        auto* r = skill_use_.call<rt::rvalue*>(self, other, result, argc, args);
        const auto after = read_primary_state();
        if (before && after && (after->stacks != before->stacks || after->current != before->current))
        {
            const int votes = ++primary_votes_[*skill];
            if (!primary && (!primary_skill_ || votes > primary_votes_[*primary_skill_]))
            {
                primary_skill_ = skill;
                shooter_ = self;
                mark_firing();
            }
        }
        return r;
    }

    bool autoaim::is_enemy(std::int32_t object_index)
    {
        if (object_index < 0)
        {
            return false;
        }
        if (static_cast<std::size_t>(object_index) >= enemy_cache_.size())
        {
            enemy_cache_.resize(object_index + 1, -1);
        }
        auto& cached = enemy_cache_[object_index];
        if (cached < 0)
        {
            cached = 0;
            for (const auto& parent : enemy_parents_)
            {
                // same encoding as the parent's handle, different index
                auto child = parent;
                if (child.type() == rt::kind::real)
                {
                    child.real = object_index;
                }
                else
                {
                    child.i64 = (parent.i64 & ~0xFFFFFFFFll) | static_cast<std::uint32_t>(object_index);
                }
                std::array args{child, parent};
                if (child.i64 == parent.i64 || rt::call(object_is_ancestor_, args).number().value_or(0.0) != 0.0)
                {
                    cached = 1;
                    break;
                }
            }
        }
        return cached == 1;
    }

    std::optional<autoaim::point> autoaim::bbox_centre(rt::instance* inst)
    {
        const auto read = [&](rt::getter_fn get) -> std::optional<double> {
            auto v = rt::rvalue::make_real(0);
            return get(inst, rt::NO_ARRAY_INDEX, &v) ? v.number() : std::nullopt;
        };
        const auto l = read(bbox_left_);
        const auto r = read(bbox_right_);
        const auto t = read(bbox_top_);
        const auto b = read(bbox_bottom_);
        if (!l || !r || !t || !b)
        {
            return std::nullopt;
        }
        return point{(*l + *r) * 0.5, (*t + *b) * 0.5};
    }

    std::optional<autoaim::rect> autoaim::view_rect()
    {
        const auto cam = rt::view_camera();
        if (!cam)
        {
            return std::nullopt;
        }
        std::array args{*cam};
        const rect r{rt::call(camera_get_view_x_, args).number().value_or(0.0),
                     rt::call(camera_get_view_y_, args).number().value_or(0.0),
                     rt::call(camera_get_view_width_, args).number().value_or(0.0),
                     rt::call(camera_get_view_height_, args).number().value_or(0.0)};
        return r.width > 0.0 && r.height > 0.0 ? std::optional{r} : std::nullopt;
    }

    std::optional<autoaim::point> autoaim::find_target(point cursor)
    {
        const auto& s = settings::get();
        const auto& lay = rt::layout();
        std::optional<point> best;
        last_enemies_ = 0;
        double best_d2 = s.autoaim_radius > 0.0f ? double{s.autoaim_radius} * s.autoaim_radius
                                                 : std::numeric_limits<double>::infinity();
        const auto view = view_rect(); // no view: the whole room counts as on screen
        rt::for_each_instance([&](rt::instance* inst) {
            if (!is_enemy(rt::field<std::int32_t>(inst, lay.object_index)))
            {
                return;
            }
            auto vis = rt::rvalue::make_real(1);
            if (visible_(inst, rt::NO_ARRAY_INDEX, &vis) && vis.number().value_or(1.0) == 0.0)
            {
                return;
            }
            const auto c = bbox_centre(inst);
            if (!c)
            {
                return;
            }
            ++last_enemies_;
            const double d2 = (c->x - cursor.x) * (c->x - cursor.x) + (c->y - cursor.y) * (c->y - cursor.y);
            if (d2 < best_d2 && (!view || view->contains(*c)))
            {
                best_d2 = d2;
                best = c;
            }
        });
        return best;
    }

    std::optional<autoaim::point> autoaim::target_for_shooter()
    {
        // mouse_x/mouse_y are read several times a frame; pick the target once per frame so x and y agree
        if (cache_valid_)
        {
            return cached_;
        }
        const bool firing = settings::get().autoaim && shooting();
        const auto cursor = firing ? real_cursor(shooter_) : std::nullopt;
        cached_ = cursor ? find_target(*cursor) : std::nullopt;
        cache_valid_ = true;
        const auto now = GetTickCount64();

        locks_ += cached_ ? 1 : 0;
        shooting_queries_ += firing ? 1 : 0;
        if (now - window_start_ >= 1000)
        {
            if (shooting_queries_ > 0)
            {
                last_shot_ = std::format("last time firing: {} of {} frames locked on, {} enemies on screen", locks_,
                                         shooting_queries_, last_enemies_);
            }
            status_ = std::format("ok, {} enemy parent(s), primary skill {}\n{}", enemy_parents_.size(),
                                  primary_skill_ ? std::format("id {}", *primary_skill_) : "not seen yet", last_shot_);
            locks_ = shooting_queries_ = 0;
            window_start_ = now;
        }
        return cached_;
    }

    bool autoaim::mouse_x_hook(rt::instance* self, int array_index, rt::rvalue* out)
    {
        const bool ok = mouse_x_.call<bool>(self, array_index, out);
        // only the shooter's own reads: menus and the gamepad cursor code keep seeing the real cursor
        if (ok && self == shooter_ && out->type() == rt::kind::real)
        {
            if (const auto t = target_for_shooter())
            {
                out->real = t->x;
            }
        }
        return ok;
    }

    bool autoaim::mouse_y_hook(rt::instance* self, int array_index, rt::rvalue* out)
    {
        const bool ok = mouse_y_.call<bool>(self, array_index, out);
        // only the shooter's own reads: menus and the gamepad cursor code keep seeing the real cursor
        if (ok && self == shooter_ && out->type() == rt::kind::real)
        {
            if (const auto t = target_for_shooter())
            {
                out->real = t->y;
            }
        }
        return ok;
    }
} // namespace vividerie_mod::features
