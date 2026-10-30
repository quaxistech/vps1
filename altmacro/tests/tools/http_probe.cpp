#include "altmacro/core/http.hpp"
#include "altmacro/core/shutdown.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    altmacro::ShutdownToken::instance().install_signal_handlers();
    altmacro::core::HttpOptions options;
    options.host = argv[1]; options.port = argv[2];
    options.connect_timeout_ms = std::stoi(argv[3]); options.request_timeout_ms = std::stoi(argv[4]);
    altmacro::core::HttpsClient client(options);
    const std::string flag = argc > 5 ? argv[5] : "";
    const auto target = flag == "large" ? "/?fixture_secret=" + std::string(16 * 1024 * 1024, 'x') :
                                         "/?fixture_secret=DO_NOT_LOG";
    for (int i = 0; i < (flag == "repeat" ? 2 : 1); ++i) {
        const auto response = client.get(target);
        if (!response) {
            std::cout << nlohmann::json{{"ok",false},{"code",response.error().code_name()},
                                      {"message",response.error().message}} << '\n';
            return 1;
        }
        std::cout << nlohmann::json{{"ok",true},{"status",response->status}} << '\n';
    }
}
