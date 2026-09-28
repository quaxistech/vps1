#include "altsignalgateway/domain/delivery_state.hpp"

namespace altsignalgateway::domain {

std::optional<DeliveryState> parse_delivery_state(std::string_view name) {
    if (name == "JOURNALED") return DeliveryState::journaled;
    if (name == "QUEUED") return DeliveryState::queued;
    if (name == "SENT") return DeliveryState::sent;
    if (name == "ACKED") return DeliveryState::acked;
    if (name == "EXPIRED") return DeliveryState::expired;
    if (name == "FAILED") return DeliveryState::failed;
    return std::nullopt;
}

} // namespace altsignalgateway::domain
