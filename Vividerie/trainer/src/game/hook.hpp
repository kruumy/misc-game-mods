#pragma once

#include <format>
#include <stdexcept>

#include <safetyhook.hpp>

namespace vividerie_mod::game::hook
{
    // safetyhook::create_inline returns an empty hook on failure; this throws instead
    template <typename T, typename U> [[nodiscard]] safetyhook::InlineHook create(T target, U destination)
    {
        auto hook = safetyhook::create_inline(target, destination);
        if (!hook)
        {
            throw std::runtime_error(std::format("cannot hook {}", reinterpret_cast<const void*>(target)));
        }
        return hook;
    }
} // namespace vividerie_mod::game::hook
