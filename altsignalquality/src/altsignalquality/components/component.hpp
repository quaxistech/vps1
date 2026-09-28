#pragma once

#include "altsignalquality/core/error.hpp"

#include <string_view>

namespace altsignalquality {

// A unit of the unified service. Lifecycle: init() runs sequentially on the
// supervisor thread (fail-fast, before READY=1 is signalled); run() blocks on
// a dedicated thread and must return promptly once
// ShutdownToken::instance().stop_requested() is true.
class Component {
public:
    virtual ~Component() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<void> init() = 0;
    virtual void run() = 0;
};

} // namespace altsignalquality
