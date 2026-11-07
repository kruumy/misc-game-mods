#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <Zydis/Zydis.h>

namespace vividerie_mod::game::image
{
    struct section
    {
        std::uintptr_t begin;
        std::uintptr_t end;

        [[nodiscard]] bool contains(std::uintptr_t va) const
        {
            return va >= begin && va < end;
        }
    };

    struct instruction
    {
        std::uintptr_t address;
        ZydisDecodedInstruction info;
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands;

        // absolute target of a rip-relative memory operand or a relative branch, 0 if none
        [[nodiscard]] std::uintptr_t absolute_target() const;
    };

    // analyse another mapped image instead of the host exe (offline tests); call before any lookup
    void use_module(void* base);

    [[nodiscard]] const section& text();
    [[nodiscard]] const section& rdata();
    [[nodiscard]] const section& data();

    // addresses of exact null-terminated copies of `s` in .rdata
    [[nodiscard]] std::vector<std::uintptr_t> find_cstrings(std::string_view s);

    // instructions in .text that reference `target` (in .rdata) through rip-relative addressing
    [[nodiscard]] std::vector<instruction> find_references(std::uintptr_t target);

    // start of the (primary) function containing `address`, from the exception directory
    [[nodiscard]] std::uintptr_t function_start(std::uintptr_t address);

    // linear decode from `start`; stops when `visit` returns false, on invalid bytes, or after `max_bytes`
    void walk(std::uintptr_t start, std::size_t max_bytes, const std::function<bool(const instruction&)>& visit);

    // direct call targets in order of appearance, decoding `max_bytes` from `start`
    [[nodiscard]] std::vector<std::uintptr_t> calls(std::uintptr_t start, std::size_t max_bytes);
} // namespace vividerie_mod::game::image
