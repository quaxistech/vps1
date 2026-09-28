#pragma once

#include "core/error.hpp"

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace altspotter {

// Exact fixed-point decimal: value = mantissa * 10^(-scale), scale in [0, 18].
// Exchange decimal strings are parsed digit-by-digit; binary floating point is
// never used, so no precision is lost. Arithmetic is overflow-checked (__int128).
class Decimal {
public:
    static constexpr int32_t kMaxScale = 18;

    constexpr Decimal() = default;
    constexpr Decimal(int64_t mantissa, int32_t scale) : mantissa_(mantissa), scale_(scale) {}

    [[nodiscard]] static Result<Decimal> parse(std::string_view text);

    [[nodiscard]] int64_t mantissa() const noexcept { return mantissa_; }
    [[nodiscard]] int32_t scale() const noexcept { return scale_; }
    [[nodiscard]] bool is_zero() const noexcept { return mantissa_ == 0; }
    [[nodiscard]] bool is_negative() const noexcept { return mantissa_ < 0; }

    // Canonical text form, e.g. "0.05", "-12.3", "42". Round-trips through parse().
    [[nodiscard]] std::string to_string() const;

    // Exact value comparison across scales (1.50 == 1.5).
    [[nodiscard]] std::strong_ordering operator<=>(const Decimal& other) const noexcept;
    [[nodiscard]] bool operator==(const Decimal& other) const noexcept {
        return (*this <=> other) == std::strong_ordering::equal;
    }

    // Checked addition; fails on int64 overflow after scale alignment.
    [[nodiscard]] Result<Decimal> add(const Decimal& other) const;

    // Checked multiplication (contract-size normalization: size * contract_value).
    // Result scale = sum of scales, normalized; fails if the exact product does
    // not fit the fixed-point representation.
    [[nodiscard]] Result<Decimal> mul(const Decimal& other) const;

    // Drop trailing fractional zeros (canonicalize: 1.500 -> 1.5).
    [[nodiscard]] Decimal normalized() const noexcept;

    [[nodiscard]] static const Decimal& zero() noexcept {
        static const Decimal z{0, 0};
        return z;
    }

private:
    int64_t mantissa_ = 0;
    int32_t scale_ = 0;
};

} // namespace altspotter
