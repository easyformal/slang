//------------------------------------------------------------------------------
//! @file Hash.h
//! @brief General hashing algorithms
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "slang/util/Util.h"

#if defined(_MSC_VER) && defined(_M_X64)
#    include <intrin.h>
#    pragma intrinsic(_umul128)
#endif

namespace slang {

namespace detail::hashing {

// Hashing logic taken from:
// https://github.com/martinus/unordered_dense/blob/main/include/ankerl/unordered_dense.h
//
// This is a stripped-down implementation of wyhash: https://github.com/wangyi-fudan/wyhash
// No big-endian support (because different values on different machines don't matter),
// hardcodes seed and the secret, reformattes the code, and clang-tidy fixes.

static inline void mum(uint64_t* a, uint64_t* b) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = *a;
    r *= *b;
    *a = static_cast<uint64_t>(r);
    *b = static_cast<uint64_t>(r >> 64U);
#elif defined(_MSC_VER) && defined(_M_X64)
    *a = _umul128(*a, *b, b);
#else
    uint64_t ha = *a >> 32U;
    uint64_t hb = *b >> 32U;
    uint64_t la = static_cast<uint32_t>(*a);
    uint64_t lb = static_cast<uint32_t>(*b);
    uint64_t hi{};
    uint64_t lo{};
    uint64_t rh = ha * hb;
    uint64_t rm0 = ha * lb;
    uint64_t rm1 = hb * la;
    uint64_t rl = la * lb;
    uint64_t t = rl + (rm0 << 32U);
    auto c = static_cast<uint64_t>(t < rl);
    lo = t + (rm1 << 32U);
    c += static_cast<uint64_t>(lo < t);
    hi = rh + (rm0 >> 32U) + (rm1 >> 32U) + c;
    *a = lo;
    *b = hi;
#endif
}

// multiply and xor mix function, aka MUM
[[nodiscard]] static inline uint64_t mix(uint64_t a, uint64_t b) {
    mum(&a, &b);
    return a ^ b;
}

// read functions. WARNING: we don't care about endianness, so results are different on big endian!
[[nodiscard]] static inline uint64_t r8(const uint8_t* p) {
    uint64_t v{};
    std::memcpy(&v, p, 8U);
    return v;
}

[[nodiscard]] static inline uint64_t r4(const uint8_t* p) {
    uint32_t v{};
    std::memcpy(&v, p, 4);
    return v;
}

// reads 1, 2, or 3 bytes
[[nodiscard]] static inline uint64_t r3(const uint8_t* p, size_t k) {
    return (static_cast<uint64_t>(p[0]) << 16U) | (static_cast<uint64_t>(p[k >> 1U]) << 8U) |
           p[k - 1];
}

[[maybe_unused]] [[nodiscard]] static inline uint64_t hash(void const* key, size_t len) {
    static constexpr auto secret = std::array{UINT64_C(0xa0761d6478bd642f),
                                              UINT64_C(0xe7037ed1a0b428db),
                                              UINT64_C(0x8ebc6af09c88c6e3),
                                              UINT64_C(0x589965cc75374cc3)};

    auto const* p = static_cast<uint8_t const*>(key);
    uint64_t seed = secret[0];
    uint64_t a{};
    uint64_t b{};
    if (len <= 16) [[likely]] {
        if (len >= 4) [[likely]] {
            a = (r4(p) << 32U) | r4(p + ((len >> 3U) << 2U));
            b = (r4(p + len - 4) << 32U) | r4(p + len - 4 - ((len >> 3U) << 2U));
        }
        else if (len > 0) [[likely]] {
            a = r3(p, len);
            b = 0;
        }
        else {
            a = 0;
            b = 0;
        }
    }
    else {
        size_t i = len;
        if (i > 48) [[unlikely]] {
            uint64_t see1 = seed;
            uint64_t see2 = seed;
            do
                [[likely]] {
                    seed = mix(r8(p) ^ secret[1], r8(p + 8) ^ seed);
                    see1 = mix(r8(p + 16) ^ secret[2], r8(p + 24) ^ see1);
                    see2 = mix(r8(p + 32) ^ secret[3], r8(p + 40) ^ see2);
                    p += 48;
                    i -= 48;
                }
            while (i > 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16) [[unlikely]] {
            seed = mix(r8(p) ^ secret[1], r8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = r8(p + i - 16);
        b = r8(p + i - 8);
    }

    return mix(secret[1] ^ len, mix(a ^ secret[1], b ^ seed));
}

[[nodiscard]] static inline uint64_t hash(uint64_t x) {
    return mix(x, UINT64_C(0x9E3779B97F4A7C15));
}

} // namespace detail::hashing

template<typename T, typename Enable = void>
struct hash {
    uint64_t operator()(T const& obj) const
        noexcept(noexcept(std::declval<std::hash<T>>().operator()(std::declval<T const&>()))) {
        return std::hash<T>{}(obj);
    }
};

template<typename CharT>
struct hash<std::basic_string<CharT>> {
    using is_avalanching = void;
    uint64_t operator()(const std::basic_string<CharT>& str) const noexcept {
        return detail::hashing::hash(str.data(), sizeof(CharT) * str.size());
    }
};

template<typename CharT>
struct hash<std::basic_string_view<CharT>> {
    using is_avalanching = void;
    uint64_t operator()(const std::basic_string_view<CharT>& sv) const noexcept {
        return detail::hashing::hash(sv.data(), sizeof(CharT) * sv.size());
    }
};

template<typename T>
struct hash<T*> {
    using is_avalanching = void;
    uint64_t operator()(T* ptr) const noexcept {
        return detail::hashing::hash(reinterpret_cast<uintptr_t>(ptr));
    }
};

template<typename T>
struct hash<std::unique_ptr<T>> {
    using is_avalanching = void;
    uint64_t operator()(std::unique_ptr<T> const& ptr) const noexcept {
        return detail::hashing::hash(reinterpret_cast<uintptr_t>(ptr.get()));
    }
};

template<typename T>
struct hash<std::shared_ptr<T>> {
    using is_avalanching = void;
    uint64_t operator()(const std::shared_ptr<T>& ptr) const noexcept {
        return detail::hashing::hash(reinterpret_cast<uintptr_t>(ptr.get()));
    }
};

template<typename Enum>
    requires std::is_enum_v<Enum>
struct hash<Enum> {
    using is_avalanching = void;
    uint64_t operator()(Enum e) const noexcept {
        using underlying = typename std::underlying_type_t<Enum>;
        return detail::hashing::hash(static_cast<uint64_t>(static_cast<underlying>(e)));
    }
};

#define SLANG_HASH_STATICCAST(T)                                      \
    template<>                                                        \
    struct hash<T> {                                                  \
        using is_avalanching = void;                                  \
        uint64_t operator()(const T& obj) const noexcept {            \
            return detail::hashing::hash(static_cast<uint64_t>(obj)); \
        }                                                             \
    }

#if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
SLANG_HASH_STATICCAST(bool);
SLANG_HASH_STATICCAST(char);
SLANG_HASH_STATICCAST(signed char);
SLANG_HASH_STATICCAST(unsigned char);
SLANG_HASH_STATICCAST(char16_t);
SLANG_HASH_STATICCAST(char32_t);
SLANG_HASH_STATICCAST(wchar_t);
SLANG_HASH_STATICCAST(short);
SLANG_HASH_STATICCAST(unsigned short);
SLANG_HASH_STATICCAST(int);
SLANG_HASH_STATICCAST(unsigned int);
SLANG_HASH_STATICCAST(long);
SLANG_HASH_STATICCAST(long long);
SLANG_HASH_STATICCAST(unsigned long);
SLANG_HASH_STATICCAST(unsigned long long);
#undef SLANG_HASH_STATICCAST
#if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC diagnostic pop
#endif

inline void hash_combine(size_t&) {
}

/// Hash combining function, based on the function from Boost.
/// It hashes the provided @a v object and combines it with the
/// previous hash value in @a seed.
template<typename T, typename... Rest>
inline void hash_combine(size_t& seed, const T& v, Rest... rest) {
    hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    hash_combine(seed, rest...);
}

namespace detail::hashing {

template<typename Tuple, size_t Index = std::tuple_size<Tuple>::value - 1>
struct HashValueImpl {
    static void apply(size_t& seed, const Tuple& tuple) {
        HashValueImpl<Tuple, Index - 1>::apply(seed, tuple);
        hash_combine(seed, std::get<Index>(tuple));
    }
};

template<typename Tuple>
struct HashValueImpl<Tuple, 0> {
    static void apply(size_t& seed, const Tuple& tuple) { hash_combine(seed, std::get<0>(tuple)); }
};

} // namespace detail::hashing

template<typename... TT>
struct hash<std::tuple<TT...>> {
    size_t operator()(const std::tuple<TT...>& tt) const {
        size_t seed = 0;
        detail::hashing::HashValueImpl<std::tuple<TT...>>::apply(seed, tt);
        return seed;
    }
};

template<typename T, typename U>
struct hash<std::pair<T, U>> {
    using is_avalanching = void;
    size_t operator()(std::pair<T, U> const& p) const noexcept {
        size_t seed = 0;
        hash_combine(seed, p.first, p.second);
        return seed;
    }
};

template<typename T>
struct hash<std::vector<T>> {
    using is_avalanching = void;
    size_t operator()(std::vector<T> const& v) const noexcept {
        size_t seed = 0;
        for (const auto& elem : v)
            hash_combine(seed, elem);
        return seed;
    }
};

} // namespace slang
