#include "features/frame_pacing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <intrin.h>
#include <utility>

#include <Windows.h>

#include "game/hook.hpp"
#include "game/image.hpp"
#include "settings.hpp"

namespace vividerie_mod::features
{
    namespace rt = game::runtime;

    std::int64_t frame_pacing::qpc()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    void frame_pacing::install()
    {
        const auto& runner = rt::runner();
        (void)rt::layout();
        (void)rt::iterator_functions();
        (void)rt::helpers();
        const auto set_speed = rt::find_function("game_set_speed");
        const auto get_speed = rt::find_function("game_get_speed");
        const auto room_speed = rt::find_variable("room_speed");
        const auto delta_time = rt::find_variable("delta_time");
        camera_get_view_x_ = rt::find_function("camera_get_view_x");
        camera_get_view_y_ = rt::find_function("camera_get_view_y");
        camera_set_view_pos_ = rt::find_function("camera_set_view_pos");
        variable_instance_get_ = rt::find_function("variable_instance_get");
        main_loop_ = runner.main_loop;

        std::array args{rt::rvalue::make_real(0)};
        tick_hz_ = rt::call(get_speed, args).number().value_or(0.0);
        applied_rate_ = tick_hz_;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpc_freq_ = f.QuadPart;

        // all or nothing: a throw unhooks the ones already created
        auto frame = game::hook::create(runner.frame, &frame_hook);
        auto io_update = game::hook::create(runner.io_update, &io_update_hook);
        auto update = game::hook::create(runner.update, &update_hook);
        auto get_frame_rate = game::hook::create(runner.get_frame_rate, &get_frame_rate_hook);
        auto game_set_speed = game::hook::create(set_speed, &game_set_speed_hook);
        auto room_speed_set = game::hook::create(room_speed.set, &room_speed_set_hook);
        auto delta_time_get = game::hook::create(delta_time.get, &delta_time_get_hook);
        frame_ = std::move(frame);
        io_update_ = std::move(io_update);
        update_ = std::move(update);
        get_frame_rate_ = std::move(get_frame_rate);
        game_set_speed_ = std::move(game_set_speed);
        room_speed_set_ = std::move(room_speed_set);
        delta_time_get_ = std::move(delta_time_get);
        status_ = std::format("ok, game ticks at {:.0f} Hz", tick_hz_);
    }

    const std::string& frame_pacing::status()
    {
        return status_;
    }

    bool frame_pacing::active()
    {
        return settings::get().decouple_render && tick_hz_ > 0.0;
    }

    void frame_pacing::apply_rate(double rate)
    {
        std::array args{rt::rvalue::make_real(rate), rt::rvalue::make_real(0)}; // gamespeed_fps
        auto result = rt::rvalue::make_real(0);
        game_set_speed_.call(&result, nullptr, nullptr, static_cast<int>(args.size()), args.data());
        applied_rate_ = rate;
    }

    void frame_pacing::begin_frame()
    {
        if (!active())
        {
            tick_ = true;
            alpha_ = 0.0;
            last_qpc_ = 0;
            if (tick_hz_ > 0.0 && applied_rate_ != tick_hz_)
            {
                apply_rate(tick_hz_);
            }
            return;
        }

        const auto& s = settings::get();
        const double rate = s.fps_cap > 0 ? std::max<double>(s.fps_cap, tick_hz_) : UNCAPPED_RATE;
        if (rate != applied_rate_)
        {
            apply_rate(rate);
        }

        const auto now = qpc();
        const double period = 1.0 / tick_hz_;
        if (last_qpc_ == 0)
        {
            tick_ = true;
            accumulator_ = 0.0;
            last_tick_qpc_ = now - static_cast<std::int64_t>(period * qpc_freq_);
        }
        else
        {
            accumulator_ += std::min(static_cast<double>(now - last_qpc_) / qpc_freq_, MAX_FRAME_SECONDS);
            tick_ = accumulator_ >= period;
            if (tick_)
            {
                // one tick per frame at most, like the stock loop; a slow renderer slows the game down
                accumulator_ = std::min(accumulator_ - period, period);
            }
        }
        last_qpc_ = now;
        alpha_ = std::clamp(accumulator_ / period, 0.0, 1.0);
        measure(now);
        if (tick_)
        {
            tick_delta_us_ = static_cast<double>(now - last_tick_qpc_) * 1e6 / qpc_freq_;
            last_tick_qpc_ = now;
        }
    }

    void frame_pacing::measure(std::int64_t now)
    {
        ++frames_;
        ticks_ += tick_ ? 1 : 0;
        if (window_start_ == 0)
        {
            window_start_ = now;
        }
        const double seconds = static_cast<double>(now - window_start_) / qpc_freq_;
        if (seconds >= 1.0)
        {
            const auto moving = [](const motion& m) { return m.vel.x != 0.0 || m.vel.y != 0.0; };
            const auto bodies = std::ranges::count_if(tracked_, [&](const auto& kv) { return moving(kv.second.body); });
            const auto draws = std::ranges::count_if(tracked_, [&](const auto& kv) { return moving(kv.second.draw); });
            status_ = std::format("ok, {:.0f} fps, {:.0f} ticks/s (game speed {:.0f})\ntracked {}, moving {} (+{} tdmc), "
                                  "written/frame {}, camera vel ({:.2f}, {:.2f})",
                                  frames_ / seconds, ticks_ / seconds, tick_hz_, tracked_.size(), bodies, draws,
                                  written_stat_, camera_.vel.x, camera_.vel.y);
            frames_ = 0;
            ticks_ = 0;
            window_start_ = now;
        }
    }

    void frame_pacing::frame_hook()
    {
        begin_frame();
        frame_.call();
        restore();
    }

    void frame_pacing::io_update_hook()
    {
        // pressed/released edges keep accumulating until the frame that runs the step events
        if (tick_)
        {
            io_update_.call();
        }
    }

    frame_pacing::view frame_pacing::body_position(rt::instance* inst)
    {
        const auto& lay = rt::layout();
        return {rt::field<float>(inst, lay.x), rt::field<float>(inst, lay.y)};
    }

    void frame_pacing::set_body_position(rt::instance* inst, view pos)
    {
        const auto& lay = rt::layout();
        rt::set_field(inst, lay.x, static_cast<float>(pos.x));
        rt::set_field(inst, lay.y, static_cast<float>(pos.y));
    }

    std::optional<rt::rvalue> frame_pacing::tdmc_of(rt::instance* inst)
    {
        static rt::gml_string name{"tdmc"};
        std::array args{rt::instance_handle(inst), name.value()};
        const auto v = rt::call(variable_instance_get_, args);
        return v.type() == rt::kind::object ? std::optional{v} : std::nullopt;
    }

    std::optional<frame_pacing::view> frame_pacing::draw_position(rt::rvalue tdmc)
    {
        const auto x = rt::struct_get(tdmc, draw_x_name_).number();
        const auto y = rt::struct_get(tdmc, draw_y_name_).number();
        if (!x || !y)
        {
            return std::nullopt;
        }
        return view{*x, *y};
    }

    void frame_pacing::set_draw_position(rt::rvalue tdmc, view pos)
    {
        rt::struct_set(tdmc, draw_x_name_, rt::rvalue::make_real(pos.x));
        rt::struct_set(tdmc, draw_y_name_, rt::rvalue::make_real(pos.y));
    }

    frame_pacing::view frame_pacing::tick_velocity(displacement d)
    {
        const double max_step = settings::get().max_step;
        const view v{d.after.x - d.before.x, d.after.y - d.before.y};
        return std::abs(v.x) <= max_step && std::abs(v.y) <= max_step ? v : view{};
    }

    void frame_pacing::update_hook()
    {
        if (tick_)
        {
            std::unordered_map<rt::instance*, tracked> before;
            if (active() && settings::get().extrapolate)
            {
                const auto& lay = rt::layout();
                before.reserve(tracked_.size());
                rt::for_each_instance([&](rt::instance* inst) {
                    tracked t{};
                    t.id = rt::field<std::uint32_t>(inst, lay.id);
                    t.body.pos = body_position(inst);
                    const auto tdmc = tdmc_of(inst);
                    if (const auto d = tdmc ? draw_position(*tdmc) : std::nullopt)
                    {
                        t.has_tdmc = true;
                        t.draw.pos = *d;
                    }
                    before[inst] = t;
                });
                if (const auto cam = rt::view_camera())
                {
                    if (const auto pos = view_position(*cam))
                    {
                        camera_.cur = *pos;
                    }
                }
            }
            in_update_ = true;
            update_.call();
            in_update_ = false;
            snapshot_after_tick(before);
        }
        extrapolate();
    }

    void frame_pacing::snapshot_after_tick(const std::unordered_map<rt::instance*, tracked>& before)
    {
        tracked_.clear();
        camera_.valid = false;
        if (before.empty())
        {
            return;
        }
        const auto& lay = rt::layout();
        rt::for_each_instance([&](rt::instance* inst) {
            const auto it = before.find(inst);
            const auto id = rt::field<std::uint32_t>(inst, lay.id);
            if (it == before.end() || it->second.id != id)
            {
                return;
            }
            tracked t{};
            t.id = id;
            t.body.pos = body_position(inst);
            t.body.vel = tick_velocity({.before = it->second.body.pos, .after = t.body.pos});
            // TDMC movers draw at a smoothed copy of x/y, so that copy needs extrapolating too
            if (const auto tdmc = it->second.has_tdmc ? tdmc_of(inst) : std::nullopt)
            {
                if (const auto d = draw_position(*tdmc))
                {
                    t.has_tdmc = true;
                    t.draw.pos = *d;
                    t.draw.vel = tick_velocity({.before = it->second.draw.pos, .after = *d});
                }
            }
            tracked_[inst] = t;
        });

        if (const auto cam = rt::view_camera())
        {
            if (const auto pos = view_position(*cam))
            {
                camera_.vel = tick_velocity({.before = camera_.cur, .after = *pos});
                camera_.cur = *pos;
                camera_.valid = true;
            }
        }
    }

    void frame_pacing::extrapolate()
    {
        if (!active() || !settings::get().extrapolate || alpha_ <= 0.0)
        {
            return;
        }
        const auto& lay = rt::layout();
        last_written_ = 0;
        rt::for_each_instance([&](rt::instance* inst) {
            const auto it = tracked_.find(inst);
            if (it == tracked_.end() || it->second.id != rt::field<std::uint32_t>(inst, lay.id))
            {
                return;
            }
            auto& t = it->second;
            if (advance(t.body, body_position(inst)))
            {
                set_body_position(inst, t.body.written);
                ++last_written_;
            }
            const auto tdmc = t.has_tdmc ? tdmc_of(inst) : std::nullopt;
            if (const auto d = tdmc ? draw_position(*tdmc) : std::nullopt; d && advance(t.draw, *d))
            {
                set_draw_position(*tdmc, t.draw.written);
                ++last_written_;
            }
        });

        if (camera_.valid && (camera_.vel.x != 0.0 || camera_.vel.y != 0.0))
        {
            if (const auto cam = rt::view_camera())
            {
                const auto pos = view_position(*cam);
                if (pos && same_position(*pos, camera_.cur))
                {
                    camera_.written = {camera_.cur.x + camera_.vel.x * alpha_, camera_.cur.y + camera_.vel.y * alpha_};
                    set_view_position(*cam, camera_.written);
                    camera_.applied = true;
                }
            }
        }
    }

    bool frame_pacing::advance(motion& m, view current)
    {
        // skip anything moved since the tick (e.g. by async events) or not moving at all
        if ((m.vel.x == 0.0 && m.vel.y == 0.0) || !same_position(current, m.pos))
        {
            return false;
        }
        m.written = {m.pos.x + m.vel.x * alpha_, m.pos.y + m.vel.y * alpha_};
        m.applied = true;
        return true;
    }

    std::optional<frame_pacing::view> frame_pacing::retreat(motion& m, view current)
    {
        if (!std::exchange(m.applied, false))
        {
            return std::nullopt;
        }
        if (same_position(current, m.written))
        {
            return m.pos;
        }
        // moved during draw too: keep that change, drop only the extrapolation (an absolute write lands one offset short)
        return view{current.x - (m.written.x - m.pos.x), current.y - (m.written.y - m.pos.y)};
    }

    bool frame_pacing::same_position(view a, view b)
    {
        // x/y and camera view positions are stored as floats, so compare at float precision
        return static_cast<float>(a.x) == static_cast<float>(b.x) && static_cast<float>(a.y) == static_cast<float>(b.y);
    }

    void frame_pacing::restore()
    {
        // only instances still in the room are touched
        const auto& lay = rt::layout();
        if (last_written_ > 0)
        {
            rt::for_each_instance([&](rt::instance* inst) {
                const auto it = tracked_.find(inst);
                if (it == tracked_.end() || it->second.id != rt::field<std::uint32_t>(inst, lay.id))
                {
                    return;
                }
                auto& t = it->second;
                if (const auto pos = retreat(t.body, body_position(inst)))
                {
                    set_body_position(inst, *pos);
                }
                const auto tdmc = t.draw.applied ? tdmc_of(inst) : std::nullopt;
                if (const auto d = tdmc ? draw_position(*tdmc) : std::nullopt)
                {
                    if (const auto pos = retreat(t.draw, *d))
                    {
                        set_draw_position(*tdmc, *pos);
                    }
                }
            });
            std::ranges::for_each(tracked_, [](auto& kv) { kv.second.body.applied = kv.second.draw.applied = false; });
            written_stat_ = std::exchange(last_written_, 0);
        }

        if (camera_.applied)
        {
            camera_.applied = false;
            if (const auto cam = rt::view_camera())
            {
                const auto pos = view_position(*cam);
                if (pos && same_position(*pos, camera_.written))
                {
                    set_view_position(*cam, camera_.cur);
                }
            }
        }
    }

    std::optional<frame_pacing::view> frame_pacing::view_position(rt::rvalue cam)
    {
        std::array args{cam};
        const auto x = rt::call(camera_get_view_x_, args).number();
        const auto y = rt::call(camera_get_view_y_, args).number();
        if (!x || !y)
        {
            return std::nullopt;
        }
        return view{*x, *y};
    }

    void frame_pacing::set_view_position(rt::rvalue cam, view pos)
    {
        std::array args{cam, rt::rvalue::make_real(pos.x), rt::rvalue::make_real(pos.y)};
        (void)rt::call(camera_set_view_pos_, args);
    }

    bool frame_pacing::called_from_main_loop(std::uintptr_t return_address)
    {
        const auto [it, inserted] = main_loop_callers_.try_emplace(return_address, false);
        if (inserted)
        {
            try
            {
                it->second = game::image::function_start(return_address) == main_loop_;
            }
            catch (const std::exception&)
            {
                it->second = false;
            }
        }
        return it->second;
    }

    double frame_pacing::get_frame_rate_hook(void* timer)
    {
        // the limiter in the main loop sees the render rate; game logic keeps seeing its own speed
        const auto rate = get_frame_rate_.call<double>(timer);
        if (!active() || called_from_main_loop(reinterpret_cast<std::uintptr_t>(_ReturnAddress())))
        {
            return rate;
        }
        return tick_hz_;
    }

    void frame_pacing::game_set_speed_hook(rt::rvalue* result, rt::instance* self, rt::instance* other, int argc,
                                           rt::rvalue* args)
    {
        game_set_speed_.call(result, self, other, argc, args);
        if (argc >= 2)
        {
            const auto value = args[0].number().value_or(0.0);
            const auto type = args[1].number().value_or(0.0);
            if (value > 0.0)
            {
                tick_hz_ = type == 0.0 ? value : 1e6 / value;
                applied_rate_ = 0.0;
            }
        }
    }

    bool frame_pacing::room_speed_set_hook(rt::instance* self, int array_index, rt::rvalue* value)
    {
        const bool ok = room_speed_set_.call<bool>(self, array_index, value);
        if (const auto v = value->number(); ok && v && *v > 0.0)
        {
            tick_hz_ = *v;
            applied_rate_ = 0.0;
        }
        return ok;
    }

    bool frame_pacing::delta_time_get_hook(rt::instance* self, int array_index, rt::rvalue* out)
    {
        const bool ok = delta_time_get_.call<bool>(self, array_index, out);
        if (ok && in_update_ && active() && out->type() == rt::kind::real)
        {
            out->real = tick_delta_us_;
        }
        return ok;
    }
} // namespace vividerie_mod::features
