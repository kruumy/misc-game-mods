#include "game/runtime.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "game/image.hpp"

namespace vividerie_mod::game::runtime
{
    std::optional<double> rvalue::number() const
    {
        switch (type())
        {
        case kind::real:
        case kind::boolean:
            return real;
        case kind::int32:
            return static_cast<std::int32_t>(i64);
        case kind::int64:
            return static_cast<double>(i64);
        case kind::ref:
            return static_cast<std::int32_t>(i64 & 0xFFFFFFFF);
        default:
            return std::nullopt;
        }
    }

    // `lea rcx, name` ... `call add_fn` with the handler pointers in rdx and r8
    struct registration
    {
        std::uintptr_t add_fn = 0;
        std::uintptr_t rdx = 0;
        std::uintptr_t r8 = 0;
    };

    static void collect_args(const image::instruction& ins, registration& reg)
    {
        if (ins.info.mnemonic != ZYDIS_MNEMONIC_LEA || ins.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER)
        {
            return;
        }
        const auto target = ins.absolute_target();
        if (!image::text().contains(target))
        {
            return;
        }
        if (ins.operands[0].reg.value == ZYDIS_REGISTER_RDX)
        {
            reg.rdx = target;
        }
        else if (ins.operands[0].reg.value == ZYDIS_REGISTER_R8)
        {
            reg.r8 = target;
        }
    }

    static std::vector<registration> find_registrations(std::string_view name)
    {
        std::vector<registration> out;
        for (const auto str : image::find_cstrings(name))
        {
            for (const auto& lea : image::find_references(str))
            {
                if (lea.info.mnemonic != ZYDIS_MNEMONIC_LEA || lea.operands[0].reg.value != ZYDIS_REGISTER_RCX)
                {
                    continue;
                }
                registration reg;
                // arguments loaded before `lea rcx`: resync a linear decode that lands exactly on it
                constexpr std::uintptr_t LOOKBACK = 40;
                for (auto back = LOOKBACK; back > 0; --back)
                {
                    registration attempt;
                    auto p = lea.address - back;
                    bool ok = true;
                    image::walk(p, back, [&](const image::instruction& ins) {
                        if (ins.address + ins.info.length > lea.address)
                        {
                            ok = false;
                            return false;
                        }
                        if (ins.info.meta.category == ZYDIS_CATEGORY_CALL || ins.info.meta.category == ZYDIS_CATEGORY_RET ||
                            ins.info.meta.category == ZYDIS_CATEGORY_UNCOND_BR)
                        {
                            attempt = {};
                        }
                        collect_args(ins, attempt);
                        p = ins.address + ins.info.length;
                        return true;
                    });
                    if (ok && p == lea.address)
                    {
                        reg = attempt;
                        break;
                    }
                }
                // remaining arguments and the call itself
                image::walk(lea.address + lea.info.length, 32, [&](const image::instruction& ins) {
                    if (ins.info.mnemonic == ZYDIS_MNEMONIC_CALL)
                    {
                        reg.add_fn = ins.absolute_target();
                        return false;
                    }
                    collect_args(ins, reg);
                    return true;
                });
                if (reg.add_fn != 0 && reg.rdx != 0)
                {
                    out.push_back(reg);
                }
            }
        }
        return out;
    }

    static std::uintptr_t registrar_of(std::string_view anchor)
    {
        const auto regs = find_registrations(anchor);
        if (regs.size() != 1)
        {
            throw std::runtime_error(std::format("registration anchor '{}' matched {} times", anchor, regs.size()));
        }
        return regs.front().add_fn;
    }

    static registration find_registration(std::string_view name, std::uintptr_t registrar)
    {
        const auto regs = find_registrations(name);
        const auto it = std::ranges::find(regs, registrar, &registration::add_fn);
        if (it == regs.end())
        {
            throw std::runtime_error(std::format("'{}' not registered", name));
        }
        return *it;
    }

    builtin_fn find_function(std::string_view name)
    {
        static const auto function_add = registrar_of("instance_find");
        return reinterpret_cast<builtin_fn>(find_registration(name, function_add).rdx);
    }

    variable find_variable(std::string_view name)
    {
        static const auto variable_add = registrar_of("xprevious");
        const auto reg = find_registration(name, variable_add);
        return {reinterpret_cast<getter_fn>(reg.rdx), reinterpret_cast<setter_fn>(reg.r8)};
    }

    script_fn find_script(std::string_view name)
    {
        // YYC script table entries in .data: { const char* name; function; ... }
        const auto& d = image::data();
        for (const auto str : image::find_cstrings(name))
        {
            for (auto p = d.begin; p + 16 <= d.end; p += 8)
            {
                std::array<std::uintptr_t, 2> entry;
                std::memcpy(entry.data(), reinterpret_cast<const void*>(p), sizeof(entry));
                if (entry[0] == str && image::text().contains(entry[1]))
                {
                    return reinterpret_cast<script_fn>(entry[1]);
                }
            }
        }
        throw std::runtime_error(std::format("script '{}' not found", name));
    }

    // instructions referencing a profiler label string
    static std::vector<image::instruction> label_references(std::string_view label)
    {
        std::vector<image::instruction> refs;
        for (const auto str : image::find_cstrings(label))
        {
            std::ranges::copy(image::find_references(str), std::back_inserter(refs));
        }
        return refs;
    }

    static runner_functions resolve_runner()
    {
        runner_functions r{};
        const auto wait = label_references("TimingWait");
        if (wait.size() != 1)
        {
            throw std::runtime_error(std::format("label 'TimingWait' referenced {} times", wait.size()));
        }
        r.main_loop = image::function_start(wait.front().address);

        // the frame scopes "IO&YoYo" and calls the function scoping "Update"; an IO-only function scopes "IO&YoYo" too
        std::optional<image::instruction> io;
        const auto updates = label_references("Update");
        for (const auto& io_ref : label_references("IO&YoYo"))
        {
            const auto frame = image::function_start(io_ref.address);
            const auto frame_calls = image::calls(frame, 0x400);
            for (const auto& update_ref : updates)
            {
                const auto fn = image::function_start(update_ref.address);
                if (!std::ranges::contains(frame_calls, fn))
                {
                    continue;
                }
                if (io && (r.frame != frame || r.update != fn))
                {
                    throw std::runtime_error("frame function ambiguous");
                }
                io = io_ref;
                r.frame = frame;
                r.update = fn;
            }
        }
        if (!io)
        {
            throw std::runtime_error("frame function not found");
        }
        // profiler push, then the input frame update
        const auto io_calls = image::calls(io->address, 24);
        if (io_calls.size() < 2)
        {
            throw std::runtime_error("input update call not found");
        }
        r.io_update = io_calls[1];

        // room_speed's getter is `return timer.GetFrameRate()`
        const auto room_speed_calls = image::calls(reinterpret_cast<std::uintptr_t>(find_variable("room_speed").get), 0x40);
        const auto speed_calls = image::calls(reinterpret_cast<std::uintptr_t>(find_function("game_get_speed")), 0x80);
        if (room_speed_calls.size() != 1 || !std::ranges::contains(speed_calls, room_speed_calls[0]))
        {
            throw std::runtime_error("frame rate getter not found");
        }
        r.get_frame_rate = room_speed_calls[0];
        return r;
    }

    const runner_functions& runner()
    {
        static const auto r = resolve_runner();
        return r;
    }

    // displacement of the first `mnemonic` that touches memory based on `base`
    static std::uint32_t member_offset(std::uintptr_t fn, ZydisMnemonic mnemonic, ZydisRegister base)
    {
        std::optional<std::uint32_t> off;
        image::walk(fn, 0x40, [&](const image::instruction& ins) {
            if (ins.info.mnemonic == mnemonic)
            {
                for (const auto& op : std::span{ins.operands.data(), ins.info.operand_count_visible})
                {
                    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == base && op.mem.index == ZYDIS_REGISTER_NONE)
                    {
                        off = static_cast<std::uint32_t>(op.mem.disp.value);
                        return false;
                    }
                }
            }
            return ins.info.mnemonic != ZYDIS_MNEMONIC_RET;
        });
        if (!off)
        {
            throw std::runtime_error("member offset not found");
        }
        return *off;
    }

    static instance_layout resolve_layout()
    {
        instance_layout l{};
        const auto getter = [](std::string_view n) { return reinterpret_cast<std::uintptr_t>(find_variable(n).get); };
        l.x = member_offset(getter("x"), ZYDIS_MNEMONIC_MOVSS, ZYDIS_REGISTER_RCX);
        l.y = member_offset(getter("y"), ZYDIS_MNEMONIC_MOVSS, ZYDIS_REGISTER_RCX);
        l.object_index = member_offset(getter("object_index"), ZYDIS_MNEMONIC_MOV, ZYDIS_REGISTER_RCX);

        // instance_find: `test [inst+flags], skip_mask` inside the loop, then `mov eax, [inst+id]` for the result
        bool have_flags = false;
        std::optional<std::uint32_t> id;
        image::walk(reinterpret_cast<std::uintptr_t>(find_function("instance_find")), 0x200, [&](const image::instruction& ins) {
            const auto& op0 = ins.operands[0];
            const auto& op1 = ins.operands[1];
            const bool inst_mem = op0.type == ZYDIS_OPERAND_TYPE_MEMORY || op1.type == ZYDIS_OPERAND_TYPE_MEMORY;
            const auto& mem = op0.type == ZYDIS_OPERAND_TYPE_MEMORY ? op0.mem : op1.mem;
            const bool heap_base = inst_mem && mem.base != ZYDIS_REGISTER_RSP && mem.base != ZYDIS_REGISTER_RBP &&
                                   mem.base != ZYDIS_REGISTER_RIP && mem.index == ZYDIS_REGISTER_NONE;
            if (!have_flags && ins.info.mnemonic == ZYDIS_MNEMONIC_TEST && heap_base && op1.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            {
                l.flags = static_cast<std::uint32_t>(mem.disp.value);
                l.skip_mask = static_cast<std::uint32_t>(op1.imm.value.u);
                have_flags = true;
            }
            else if (have_flags && !id && ins.info.mnemonic == ZYDIS_MNEMONIC_MOV && op0.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                     op0.size == 32 && heap_base && op1.type == ZYDIS_OPERAND_TYPE_MEMORY)
            {
                id = static_cast<std::uint32_t>(mem.disp.value);
            }
            return ins.info.mnemonic != ZYDIS_MNEMONIC_RET;
        });
        if (!have_flags || !id)
        {
            throw std::runtime_error("instance flags/id offsets not found");
        }
        l.id = *id;
        return l;
    }

    const instance_layout& layout()
    {
        static const auto l = resolve_layout();
        return l;
    }

    static instance_iterator_functions resolve_iterator()
    {
        // instance_find: arg, arg, iterator init, get, advance, get
        const auto c = image::calls(reinterpret_cast<std::uintptr_t>(find_function("instance_find")), 0x200);
        if (c.size() < 6 || c[0] != c[1] || c[3] != c[5])
        {
            throw std::runtime_error("instance iterator not found");
        }
        return {
            reinterpret_cast<decltype(instance_iterator_functions::init)>(c[2]),
            reinterpret_cast<decltype(instance_iterator_functions::get)>(c[3]),
            reinterpret_cast<decltype(instance_iterator_functions::advance)>(c[4]),
        };
    }

    const instance_iterator_functions& iterator_functions()
    {
        static const auto f = resolve_iterator();
        return f;
    }

    rvalue call(builtin_fn fn, std::span<rvalue> args)
    {
        auto result = rvalue::make_real(0);
        fn(&result, nullptr, nullptr, static_cast<int>(args.size()), args.data());
        return result;
    }

    rvalue instance_handle(const instance* inst)
    {
        // learn the runtime's handle encoding once from instance_find(all, 0)
        static const auto encoding = [] {
            constexpr double ALL = -3.0;
            std::array args{rvalue::make_real(ALL), rvalue::make_real(0)};
            return call(find_function("instance_find"), args);
        }();
        const auto id = field<std::uint32_t>(inst, layout().id);
        if (encoding.type() != kind::ref)
        {
            return rvalue::make_real(id);
        }
        auto handle = encoding;
        handle.i64 = (encoding.i64 & ~0xFFFFFFFFll) | id;
        return handle;
    }

    gml_string::gml_string(std::string text)
        : text_(std::move(text)), ref_{text_.c_str(), 1 << 30, static_cast<int>(text_.size())}
    {
    }

    rvalue gml_string::value()
    {
        rvalue r;
        r.ptr = &ref_;
        r.kind_bits = static_cast<std::uint32_t>(kind::string);
        return r;
    }

    const helper_builtins& helpers()
    {
        static const helper_builtins h{find_function("view_get_camera"), find_function("variable_struct_get"),
                                       find_function("variable_struct_set")};
        return h;
    }

    std::optional<rvalue> view_camera()
    {
        std::array args{rvalue::make_real(0)};
        const auto cam = call(helpers().view_get_camera, args);
        return cam.number().value_or(-1.0) >= 0.0 ? std::optional{cam} : std::nullopt;
    }

    rvalue struct_get(rvalue obj, gml_string& name)
    {
        std::array args{obj, name.value()};
        return call(helpers().variable_struct_get, args);
    }

    void struct_set(rvalue obj, gml_string& name, rvalue value)
    {
        std::array args{obj, name.value(), value};
        (void)call(helpers().variable_struct_set, args);
    }
} // namespace vividerie_mod::game::runtime
