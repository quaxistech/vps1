#include "core/decimal.hpp"

#include <array>
#include <limits>
#include <optional>

// Decimal arithmetic intentionally relies on GCC/Clang's native 128-bit
// integer extension to prove that scale alignment cannot overflow. Keep
// -Wpedantic enabled for the rest of the project and scope the exception to
// this implementation only.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

namespace altspotter {

namespace {

using int128 = __int128;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();

constexpr std::array<int64_t, 19> kPow10 = [] {
    std::array<int64_t, 19> p{};
    p[0] = 1;
    for (size_t i = 1; i < p.size(); ++i) p[i] = p[i - 1] * 10;
    return p;
}();

} // namespace

Result<Decimal> Decimal::parse(std::string_view text) {
    if (text.empty()) return fail(ErrorCode::parse_error, "empty decimal string");

    size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = (text[pos] == '-');
        ++pos;
    }
    if (pos >= text.size()) return fail(ErrorCode::parse_error, "decimal has no digits");

    std::string digits;
    digits.reserve(text.size() - pos);
    int32_t scale = 0;
    bool seen_point = false;
    bool seen_digit = false;

    for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (c == '.') {
            if (seen_point) return fail(ErrorCode::parse_error, "multiple decimal points");
            seen_point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return fail(ErrorCode::parse_error,
                        "invalid character in decimal: '" + std::string(text) + "'");
        }
        seen_digit = true;
        digits.push_back(c);
        if (seen_point) ++scale;
    }
    if (!seen_digit) return fail(ErrorCode::parse_error, "decimal has no digits");

    const auto accumulate = [](std::string_view value) -> std::optional<int64_t> {
        unsigned __int128 parsed = 0;
        for (const char c : value) {
            const auto digit = static_cast<unsigned>(c - '0');
            const auto maximum = static_cast<unsigned __int128>(kInt64Max);
            if (parsed > (maximum - digit) / 10) return std::nullopt;
            parsed = parsed * 10 + digit;
        }
        return static_cast<int64_t>(parsed);
    };

    // Preserve exchange formatting (including meaningful trailing scale) when
    // it fits.  PostgreSQL NUMERIC commonly appends harmless fractional zeros;
    // only strip those when the raw mantissa/scale would otherwise be outside
    // the fixed-point representation.
    auto value = accumulate(digits);
    if (!value || scale > kMaxScale) {
        while (scale > 0 && !digits.empty() && digits.back() == '0') {
            digits.pop_back();
            --scale;
        }
        value = accumulate(digits);
    }
    if (scale > kMaxScale) {
        return fail(ErrorCode::parse_error, "decimal scale exceeds 18: '" + std::string(text) + "'");
    }
    if (!value) {
        return fail(ErrorCode::parse_error,
                    "decimal exceeds fixed-point range: '" + std::string(text) + "'");
    }

    return Decimal{negative ? -*value : *value, scale};
}

std::string Decimal::to_string() const {
    const bool neg = mantissa_ < 0;
    // Avoid UB on INT64_MIN by widening before negation.
    unsigned __int128 abs_val =
        neg ? static_cast<unsigned __int128>(-(static_cast<int128>(mantissa_)))
            : static_cast<unsigned __int128>(mantissa_);

    char digits[48];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + static_cast<int>(abs_val % 10));
        abs_val /= 10;
    } while (abs_val != 0);
    while (n <= scale_) digits[n++] = '0'; // pad to "0.xxx" form

    std::string out;
    if (neg) out.push_back('-');
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(digits[i]);
        if (i == scale_ && i != 0) out.push_back('.');
    }
    return out;
}

std::strong_ordering Decimal::operator<=>(const Decimal& other) const noexcept {
    // Cross-multiply to the common scale using 128-bit intermediates (no overflow:
    // |mantissa| <= 2^63, 10^18 < 2^60, product < 2^123).
    const int128 lhs = static_cast<int128>(mantissa_) * kPow10[static_cast<size_t>(other.scale_)];
    const int128 rhs = static_cast<int128>(other.mantissa_) * kPow10[static_cast<size_t>(scale_)];
    if (lhs < rhs) return std::strong_ordering::less;
    if (lhs > rhs) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

Result<Decimal> Decimal::add(const Decimal& other) const {
    const int32_t target = std::max(scale_, other.scale_);
    const int128 lhs = static_cast<int128>(mantissa_) * kPow10[static_cast<size_t>(target - scale_)];
    const int128 rhs =
        static_cast<int128>(other.mantissa_) * kPow10[static_cast<size_t>(target - other.scale_)];
    const int128 sum = lhs + rhs;
    if (sum > kInt64Max || sum < -static_cast<int128>(kInt64Max) - 1) {
        return fail(ErrorCode::internal_error, "decimal addition overflow");
    }
    return Decimal{static_cast<int64_t>(sum), target};
}

Result<Decimal> Decimal::mul(const Decimal& other) const {
    int128 product = static_cast<int128>(mantissa_) * static_cast<int128>(other.mantissa_);
    int32_t scale = scale_ + other.scale_;
    // Strip trailing zeros / reduce scale into range while keeping exactness.
    while ((scale > kMaxScale || product > kInt64Max ||
            product < -static_cast<int128>(kInt64Max) - 1) &&
           scale > 0 && product % 10 == 0) {
        product /= 10;
        --scale;
    }
    if (scale > kMaxScale || product > kInt64Max ||
        product < -static_cast<int128>(kInt64Max) - 1) {
        return fail(ErrorCode::internal_error, "decimal multiplication overflow");
    }
    return Decimal{static_cast<int64_t>(product), scale}.normalized();
}

Decimal Decimal::normalized() const noexcept {
    int64_t m = mantissa_;
    int32_t s = scale_;
    while (s > 0 && m != 0 && m % 10 == 0) {
        m /= 10;
        --s;
    }
    if (m == 0) s = 0;
    return Decimal{m, s};
}

} // namespace altspotter

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
