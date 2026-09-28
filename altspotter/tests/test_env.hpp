#pragma once

// Native test environment helpers:
//   * spawn/terminate child processes (nats-server, the altspotter binary);
//   * TempNats: throwaway NATS server with JetStream on a random localhost port
//     and a temporary storage dir (binary: $ALTSPOTTER_TEST_NATS_SERVER or
//     "nats-server" in PATH);
//   * TempDb: throwaway PostgreSQL database created via $ALTSPOTTER_TEST_PG_DSN
//     (a role allowed to CREATE DATABASE). Tests SKIP when the environment is not
//     available, and run fully natively where it is (VPS/CI).

#include <pqxx/pqxx>

#include <csignal>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace altspotter::test {

namespace fs = std::filesystem;

// Runtime override first (VPS runs use the installed release), compile-time
// default second (local build tree).
inline std::string bin_dir() {
    if (const char* env = std::getenv("ALTSPOTTER_BIN_DIR")) return env;
#ifdef ALTSPOTTER_BIN_DIR
    return ALTSPOTTER_BIN_DIR;
#else
    return "build/bin";
#endif
}

inline std::string migrations_dir() {
    if (const char* env = std::getenv("ALTSPOTTER_MIGRATIONS_DIR")) return env;
#ifdef ALTSPOTTER_MIGRATIONS_DIR
    return ALTSPOTTER_MIGRATIONS_DIR;
#else
    return "db/migrations";
#endif
}

class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const std::string& exe, std::vector<std::string> args)
        : ChildProcess(exe, std::move(args), std::string{}) {}
    // With `output_path`, the child's stdout and stderr go to that file, so a
    // test can assert on what the real binary logged.
    ChildProcess(const std::string& exe, std::vector<std::string> args,
                 const std::string& output_path) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (auto& a : args_storage_ = std::move(args)) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        pid_ = ::fork();
        if (pid_ == 0) {
            if (!output_path.empty()) {
                // The logger writes straight to journald when JOURNAL_STREAM is
                // set (inherited from a desktop session); the file is the point.
                ::unsetenv("JOURNAL_STREAM");
                const int fd = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    ::dup2(fd, STDOUT_FILENO);
                    ::dup2(fd, STDERR_FILENO);
                    ::close(fd);
                }
            }
            ::execvp(exe.c_str(), argv.data());
            ::_exit(127);
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;
    ~ChildProcess() { terminate(); }

    void terminate(int sig = SIGTERM, int wait_ms = 5000) {
        if (pid_ <= 0) return;
        ::kill(pid_, sig);
        for (int i = 0; i < wait_ms / 50; ++i) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    [[nodiscard]] pid_t pid() const { return pid_; }
    [[nodiscard]] bool running() const {
        if (pid_ <= 0) return false;
        int status = 0;
        return ::waitpid(pid_, &status, WNOHANG) == 0;
    }

private:
    pid_t pid_ = -1;
    std::vector<std::string> args_storage_;
};

// Run a binary to completion; returns exit code (or -1).
inline int run_tool(const std::string& exe, const std::vector<std::string>& args,
                    int timeout_sec = 60) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execvp(exe.c_str(), argv.data());
        ::_exit(127);
    }
    for (int i = 0; i < timeout_sec * 10; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return -1;
}

class TempNats {
public:
    static std::optional<std::string> server_binary() {
        if (const char* env = std::getenv("ALTSPOTTER_TEST_NATS_SERVER")) return std::string(env);
        for (const char* p : {"/usr/local/bin/nats-server", "/usr/bin/nats-server",
                              "/usr/sbin/nats-server"}) {
            if (fs::exists(p)) return std::string(p);
        }
        return std::nullopt;
    }

    bool start() {
        auto binary = server_binary();
        if (!binary) return false;
        std::mt19937 rng(std::random_device{}());
        port_ = 20000 + static_cast<int>(rng() % 20000);
        store_ = fs::temp_directory_path() /
                 ("altspotter_js_" + std::to_string(::getpid()) + "_" + std::to_string(port_));
        fs::create_directories(store_);
        proc_ = std::make_unique<ChildProcess>(
            *binary, std::vector<std::string>{"-a", "127.0.0.1", "-p", std::to_string(port_),
                                              "--jetstream", "-sd", store_.string()});
        // wait for readiness
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!proc_->running()) return false;
            if (tcp_ready()) return true;
        }
        return false;
    }

    ~TempNats() {
        if (proc_) proc_->terminate();
        std::error_code ec;
        fs::remove_all(store_, ec);
    }

    [[nodiscard]] std::string url() const {
        return "nats://127.0.0.1:" + std::to_string(port_);
    }

private:
    bool tcp_ready() const;
    int port_ = 0;
    fs::path store_;
    std::unique_ptr<ChildProcess> proc_;
};

class TempDb {
public:
    static std::optional<std::string> admin_dsn() {
        if (const char* env = std::getenv("ALTSPOTTER_TEST_PG_DSN")) return std::string(env);
        return std::nullopt;
    }

    bool create() {
        auto admin = admin_dsn();
        if (!admin) return false;
        name_ = "altspotter_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::random_device{}() % 100000);
        try {
            pqxx::connection conn(*admin);
            pqxx::nontransaction txn(conn);
            txn.exec("CREATE DATABASE " + txn.esc(name_) +
                     " TEMPLATE template0 ENCODING 'UTF8'");
        } catch (const std::exception&) {
            return false;
        }
        // derive DSN: postgresql://user:pass@host:port/dbname[?...]
        const std::string& base = *admin;
        const auto scheme = base.find("://");
        const auto q = base.find('?');
        const auto last_slash = base.rfind('/', q == std::string::npos ? base.size() : q);
        if (scheme == std::string::npos || last_slash == std::string::npos ||
            last_slash <= scheme + 2) {
            return false;
        }
        dsn_ = base.substr(0, last_slash + 1) + name_;
        if (q != std::string::npos) dsn_ += base.substr(q);
        return true;
    }

    ~TempDb() {
        if (name_.empty()) return;
        auto admin = admin_dsn();
        if (!admin) return;
        try {
            pqxx::connection conn(*admin);
            pqxx::nontransaction txn(conn);
            txn.exec("DROP DATABASE IF EXISTS " + txn.esc(name_) + " WITH (FORCE)");
        } catch (const std::exception&) {
        }
    }

    [[nodiscard]] const std::string& dsn() const { return dsn_; }

private:
    std::string name_;
    std::string dsn_;
};

}  // namespace altspotter::test
