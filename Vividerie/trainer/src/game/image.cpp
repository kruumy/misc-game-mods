#include "game/image.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <Windows.h>

namespace vividerie_mod::game::image
{
    static std::uintptr_t& module_base_storage()
    {
        static auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        return base;
    }

    static std::uintptr_t module_base()
    {
        return module_base_storage();
    }

    void use_module(void* base)
    {
        module_base_storage() = reinterpret_cast<std::uintptr_t>(base);
    }

    static section find_section(std::string_view name)
    {
        const auto base = module_base();
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const std::span sections{IMAGE_FIRST_SECTION(nt), nt->FileHeader.NumberOfSections};
        const auto it = std::ranges::find_if(sections, [&](const IMAGE_SECTION_HEADER& s) {
            return std::string_view{reinterpret_cast<const char*>(s.Name), strnlen(reinterpret_cast<const char*>(s.Name), 8)} == name;
        });
        if (it == sections.end())
        {
            throw std::runtime_error(std::string{"missing section "} + std::string{name});
        }
        const auto begin = base + it->VirtualAddress;
        return {begin, begin + std::min(it->Misc.VirtualSize, it->SizeOfRawData)};
    }

    const section& text()
    {
        static const auto s = find_section(".text");
        return s;
    }

    const section& rdata()
    {
        static const auto s = find_section(".rdata");
        return s;
    }

    const section& data()
    {
        static const auto s = find_section(".data");
        return s;
    }

    static const ZydisDecoder& decoder()
    {
        static const auto d = [] {
            ZydisDecoder dec{};
            ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            return dec;
        }();
        return d;
    }

    static bool decode(std::uintptr_t address, std::size_t max_len, instruction& out)
    {
        out.address = address;
        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder(), reinterpret_cast<const void*>(address), max_len, &out.info,
                                                   out.operands.data()));
    }

    static bool has_ignored_prefix(const instruction& ins)
    {
        return std::ranges::any_of(std::span{ins.info.raw.prefixes, ins.info.raw.prefix_count},
                                   [](const auto& p) { return p.type == ZYDIS_PREFIX_TYPE_IGNORED; });
    }

    std::uintptr_t instruction::absolute_target() const
    {
        for (const auto& op : std::span{operands.data(), info.operand_count_visible})
        {
            const bool rip_mem = op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP;
            const bool rel_imm = op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative;
            ZyanU64 result = 0;
            if ((rip_mem || rel_imm) && ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&info, &op, address, &result)))
            {
                return static_cast<std::uintptr_t>(result);
            }
        }
        return 0;
    }

    std::vector<std::uintptr_t> find_cstrings(std::string_view s)
    {
        std::vector<std::uintptr_t> out;
        const auto& r = rdata();
        const std::string_view hay{reinterpret_cast<const char*>(r.begin), r.end - r.begin};
        std::string needle{'\0'};
        needle.append(s);
        needle.push_back('\0');
        for (auto pos = hay.find(needle); pos != std::string_view::npos; pos = hay.find(needle, pos + 1))
        {
            out.push_back(r.begin + pos + 1);
        }
        return out;
    }

    // every disp32/rel32 in .text that would point into .rdata, sorted by target; built in one pass
    struct rdata_candidate
    {
        std::uintptr_t target;
        std::uintptr_t disp;
        std::uintptr_t tail;
    };

    static const std::vector<rdata_candidate>& rdata_candidates()
    {
        static const auto index = [] {
            std::vector<rdata_candidate> out;
            const auto& t = text();
            const auto& r = rdata();
            // a disp32/rel32 ends the instruction or is followed by an imm8/imm32
            for (auto p = t.begin; p + 4 <= t.end; ++p)
            {
                std::int32_t disp;
                std::memcpy(&disp, reinterpret_cast<const void*>(p), 4);
                for (const std::uintptr_t tail : {0u, 1u, 4u})
                {
                    if (const auto target = p + 4 + tail + static_cast<std::intptr_t>(disp); r.contains(target))
                    {
                        out.push_back({target, p, tail});
                    }
                }
            }
            std::ranges::stable_sort(out, {}, &rdata_candidate::target);
            return out;
        }();
        return index;
    }

    std::vector<instruction> find_references(std::uintptr_t target)
    {
        if (!rdata().contains(target))
        {
            throw std::invalid_argument("reference target outside .rdata");
        }
        std::vector<instruction> out;
        for (const auto& c : std::ranges::equal_range(rdata_candidates(), target, {}, &rdata_candidate::target))
        {
            // opcode + modrm (+ prefixes) sit 1..4 bytes before the displacement; the longest clean decode
            // keeps the REX prefix (lea rcx, not lea ecx one byte later)
            for (std::uintptr_t back = 4; back >= 1; --back)
            {
                instruction ins;
                if (decode(c.disp - back, 15, ins) && ins.info.length == back + 4 + c.tail && ins.absolute_target() == target &&
                    !has_ignored_prefix(ins))
                {
                    out.push_back(ins);
                    break;
                }
            }
        }
        return out;
    }

    std::uintptr_t function_start(std::uintptr_t address)
    {
        DWORD64 image_base = 0;
        auto* entry = RtlLookupFunctionEntry(address, &image_base, nullptr);
        if (entry == nullptr)
        {
            throw std::runtime_error("address has no unwind entry");
        }
        // follow chained unwind info (split function chunks) to the primary entry
        for (;;)
        {
            const auto* unwind = reinterpret_cast<const std::uint8_t*>(image_base + entry->UnwindData);
            if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0)
            {
                break;
            }
            const std::uint8_t code_count = unwind[2];
            const auto chained = 4 + ((code_count + 1) & ~1) * 2;
            entry = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<std::uint8_t*>(unwind + chained));
        }
        return image_base + entry->BeginAddress;
    }

    void walk(std::uintptr_t start, std::size_t max_bytes, const std::function<bool(const instruction&)>& visit)
    {
        for (auto p = start; p < start + max_bytes;)
        {
            instruction ins;
            if (!decode(p, 15, ins) || !visit(ins))
            {
                return;
            }
            p += ins.info.length;
        }
    }

    std::vector<std::uintptr_t> calls(std::uintptr_t start, std::size_t max_bytes)
    {
        std::vector<std::uintptr_t> out;
        walk(start, max_bytes, [&](const instruction& ins) {
            if (ins.info.mnemonic == ZYDIS_MNEMONIC_CALL)
            {
                if (const auto t = ins.absolute_target(); text().contains(t))
                {
                    out.push_back(t);
                }
            }
            return ins.info.mnemonic != ZYDIS_MNEMONIC_INT3;
        });
        return out;
    }
} // namespace vividerie_mod::game::image
