#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vividerie_mod::game::runtime
{
    enum class kind : std::uint32_t
    {
        real = 0,
        string = 1,
        object = 6,
        int32 = 7,
        int64 = 10,
        boolean = 13,
        ref = 15,
    };

    // GameMaker RValue
    struct rvalue
    {
        union {
            double real;
            std::int64_t i64;
            void* ptr;
        };
        std::uint32_t flags = 0;
        std::uint32_t kind_bits = 0;

        [[nodiscard]] kind type() const
        {
            return static_cast<kind>(kind_bits & 0xFFFFFF);
        }

        [[nodiscard]] std::optional<double> number() const;

        [[nodiscard]] static rvalue make_real(double v)
        {
            rvalue r;
            r.real = v;
            return r;
        }
    };
    static_assert(sizeof(rvalue) == 16);

    using instance = std::byte;
    using builtin_fn = void (*)(rvalue* result, instance* self, instance* other, int argc, rvalue* args);
    using getter_fn = bool (*)(instance* self, int array_index, rvalue* out);
    using setter_fn = bool (*)(instance* self, int array_index, rvalue* value);
    using script_fn = rvalue* (*)(instance* self, instance* other, rvalue* result, int argc, rvalue** args);

    constexpr int NO_ARRAY_INDEX = static_cast<int>(0x80000000);

    struct variable
    {
        getter_fn get;
        setter_fn set;
    };

    // builtins and compiled game scripts resolved by name; all throw std::runtime_error when not found
    [[nodiscard]] builtin_fn find_function(std::string_view name);
    [[nodiscard]] variable find_variable(std::string_view name);
    [[nodiscard]] script_fn find_script(std::string_view name);

    struct runner_functions
    {
        std::uintptr_t main_loop;      // frame pacing / timing wait
        std::uintptr_t frame;          // io, update, draw for one frame
        std::uintptr_t io_update;      // starts the input frame (clears pressed/released)
        std::uintptr_t update;         // all step events
        std::uintptr_t get_frame_rate; // CTimingSource frame rate getter
    };
    [[nodiscard]] const runner_functions& runner();

    struct instance_layout
    {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t object_index;
        std::uint32_t id;
        std::uint32_t flags;
        std::uint32_t skip_mask;
    };
    [[nodiscard]] const instance_layout& layout();

    template <typename T> [[nodiscard]] T field(const instance* inst, std::uint32_t offset)
    {
        T v;
        std::memcpy(&v, inst + offset, sizeof(T));
        return v;
    }

    template <typename T> void set_field(instance* inst, std::uint32_t offset, T v)
    {
        std::memcpy(inst + offset, &v, sizeof(T));
    }

    struct instance_iterator_functions
    {
        void* (*init)(void* it, int object, instance* self, instance* other, bool flag);
        instance* (*get)(void* it);
        void* (*advance)(void* it);
    };
    [[nodiscard]] const instance_iterator_functions& iterator_functions();

    // visits every live instance in the current room, the same walk instance_find(all, n) performs
    template <std::invocable<instance*> F> void for_each_instance(F&& visit)
    {
        constexpr int ALL = -3;
        const auto& fns = iterator_functions();
        const auto& lay = layout();
        alignas(16) std::array<std::byte, 0x100> it{};
        fns.init(it.data(), ALL, nullptr, nullptr, false);
        for (auto* cur = fns.get(it.data()); cur != nullptr; cur = fns.get(it.data()))
        {
            fns.advance(it.data());
            if ((field<std::uint32_t>(cur, lay.flags) & lay.skip_mask) == 0)
            {
                visit(cur);
            }
        }
    }

    // calls a builtin with no self/other
    rvalue call(builtin_fn fn, std::span<rvalue> args);

    // the GML handle (`id`) of a live instance, for builtins that take one
    [[nodiscard]] rvalue instance_handle(const instance* inst);

    // a GML string argument that outlives any call it is passed to
    class gml_string
    {
      public:
        explicit gml_string(std::string text);
        gml_string(const gml_string&) = delete;
        gml_string& operator=(const gml_string&) = delete;

        [[nodiscard]] rvalue value();

      private:
        struct ref_string
        {
            const char* text;
            int refs;
            int size;
        };
        std::string text_;
        ref_string ref_;
    };

    // builtins behind the helpers below; resolve at install so a missing one fails there, not mid-frame
    struct helper_builtins
    {
        builtin_fn view_get_camera;
        builtin_fn variable_struct_get;
        builtin_fn variable_struct_set;
    };
    [[nodiscard]] const helper_builtins& helpers();

    // view 0's camera, if the room has one
    [[nodiscard]] std::optional<rvalue> view_camera();

    [[nodiscard]] rvalue struct_get(rvalue obj, gml_string& name);
    void struct_set(rvalue obj, gml_string& name, rvalue value);
} // namespace vividerie_mod::game::runtime
