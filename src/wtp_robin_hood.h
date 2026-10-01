#pragma once

// Structured binding support for robin_hood::pair (auto [key, value] = ...).
// Kept outside of the unmodified third-party robin_hood.h.

#include <tuple>
#include <type_traits>
#include <utility>
#include "robin_hood.h"

namespace std {
template <typename T1, typename T2>
struct tuple_size<robin_hood::pair<T1, T2>> : std::integral_constant<std::size_t, 2> {};

template <size_t I, typename T1, typename T2>
struct tuple_element<I, robin_hood::pair<T1, T2>> {
    using type = typename std::conditional<I == 0, T1, T2>::type;
};
} // namespace std

namespace robin_hood {

template <size_t I, typename T1, typename T2>
auto& get(pair<T1, T2>& p) noexcept {
    static_assert(I < 2, "index out of bounds");
    if constexpr (I == 0) {
        return p.first;
    } else {
        return p.second;
    }
}

template <size_t I, typename T1, typename T2>
auto const& get(pair<T1, T2> const& p) noexcept {
    static_assert(I < 2, "index out of bounds");
    if constexpr (I == 0) {
        return p.first;
    } else {
        return p.second;
    }
}

template <size_t I, typename T1, typename T2>
auto&& get(pair<T1, T2>&& p) noexcept {
    static_assert(I < 2, "index out of bounds");
    if constexpr (I == 0) {
        return std::move(p.first);
    } else {
        return std::move(p.second);
    }
}

template <size_t I, typename T1, typename T2>
auto const&& get(pair<T1, T2> const&& p) noexcept {
    static_assert(I < 2, "index out of bounds");
    if constexpr (I == 0) {
        return std::move(p.first);
    } else {
        return std::move(p.second);
    }
}

} // namespace robin_hood
