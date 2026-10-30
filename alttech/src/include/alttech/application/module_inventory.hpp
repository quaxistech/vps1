#pragma once
#include "alttech/application/module_supervisor.hpp"
#include "alttech/config/analytics_config.hpp"
#include "alttech/modules/feature.hpp"
#include "alttech/modules/microstructure.hpp"
#include "alttech/modules/structure.hpp"
#include "alttech/modules/regime.hpp"
#include "alttech/modules/cross_venue.hpp"
#include "alttech/modules/macro.hpp"
#include <array>

namespace alttech::application {
using ModuleRunner = Result<void> (*)(const YAML::Node&, persistence::PgPool&,
                                      messaging::NatsClient&, ModuleSupervisor&);
struct ModuleLaunch { std::string_view name; ModuleRunner runner; const YAML::Node* config; };
inline auto module_inventory(const config::AnalyticsConfig& configuration) {
    return std::array{
        ModuleLaunch{"feature", modules::run_feature, &configuration.feature},
        ModuleLaunch{"microstructure", modules::run_microstructure, &configuration.microstructure},
        ModuleLaunch{"structure", modules::run_structure, &configuration.structure},
        ModuleLaunch{"regime", modules::run_regime, &configuration.regime},
        ModuleLaunch{"cross_venue", modules::run_cross_venue, &configuration.cross_venue},
        ModuleLaunch{"macro", modules::run_macro, &configuration.macro}};
}
} // namespace alttech::application
