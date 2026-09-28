#include "altsignalquality/core/nats_subjects.hpp"

namespace altsignalquality::nats_subjects {

std::string subject_token(std::string_view symbol, int64_t canonical_pair_id) {
    const bool usable =
        !symbol.empty() &&
        symbol.find_first_of(".*> \t\r\n") == std::string_view::npos;
    if (usable) return std::string(symbol);
    return "pair-" + std::to_string(canonical_pair_id);
}

} // namespace altsignalquality::nats_subjects
