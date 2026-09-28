#include "core/types.hpp"

#include <sstream>

namespace altspotter {

std::string InstrumentSpec::normalized_fingerprint() const {
    std::ostringstream os;
    os << exchange_symbol << '|' << base_asset << '|' << quote_asset << '|'
       << settle_asset << '|' << contract_value.normalized().to_string() << '|'
       << contract_type << '|'
       << instrument_status_label(status) << '|' << list_time_ms << '|'
       << tick_size.normalized().to_string() << '|' << price_precision << '|'
       << quantity_step.normalized().to_string() << '|' << quantity_precision << '|'
       << min_order_quantity.normalized().to_string() << '|' << min_notional.normalized().to_string()
       << '|' << maker_fee_rate.normalized().to_string() << '|'
       << taker_fee_rate.normalized().to_string() << '|' << min_leverage << '|' << max_leverage
       << '|' << funding_interval_hours;
    return os.str();
}

} // namespace altspotter
