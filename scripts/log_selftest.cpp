#include "log.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

struct TestPaths {
    std::string base_dir;
    std::string date_dir;
    std::string hour_file;
    std::string hour_file_1;
};

static TestPaths build_paths(const std::string &base_dir) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv = *std::localtime(&t);

    char date_dir[256] = {0};
    std::snprintf(date_dir, sizeof(date_dir), "%s/%d_%02d_%02d_logs",
                  base_dir.c_str(), tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

    char hour_file[320] = {0};
    std::snprintf(hour_file, sizeof(hour_file), "%s/%02d_server.log",
                  date_dir, tmv.tm_hour);

    char hour_file_1[320] = {0};
    std::snprintf(hour_file_1, sizeof(hour_file_1), "%s/%02d_server.log.1",
                  date_dir, tmv.tm_hour);

    return TestPaths{base_dir, date_dir, hour_file, hour_file_1};
}

static bool file_nonempty(const std::string &path) {
    return fs::exists(path) && fs::is_regular_file(path) && fs::file_size(path) > 0;
}

static int run_sync_tests() {
    const std::string base_dir = "./serverlogs_test_sync";
    auto paths = build_paths(base_dir);

    // init in sync mode (log_queue_size = 0), small max_lines for rotation
    if (!Log::getInstance().init(base_dir.c_str(), 0, 1024, 3, 0)) {
        std::cerr << "[sync] init failed\n";
        return 1;
    }

    for (int i = 0; i < 7; ++i) {
        Log::getInstance().write_log(1, "sync line %d", i);
    }
    Log::getInstance().flush();

    if (!file_nonempty(paths.hour_file)) {
        std::cerr << "[sync] base file missing or empty: " << paths.hour_file << "\n";
        return 1;
    }
    if (!file_nonempty(paths.hour_file_1)) {
        std::cerr << "[sync] rotated file missing or empty: " << paths.hour_file_1 << "\n";
        return 1;
    }

    std::cout << "[sync] ok\n";
    return 0;
}

static int run_async_tests() {
    const std::string base_dir = "./serverlogs_test_async";
    auto paths = build_paths(base_dir);

    // init in async mode with small queue
    if (!Log::getInstance().init(base_dir.c_str(), 0, 1024, 5, 2)) {
        std::cerr << "[async] init failed\n";
        return 1;
    }

    for (int i = 0; i < 20; ++i) {
        Log::getInstance().write_log(1, "async line %d", i);
    }

    // give background thread time to flush queued logs
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Log::getInstance().flush();

    if (!file_nonempty(paths.hour_file)) {
        std::cerr << "[async] base file missing or empty: " << paths.hour_file << "\n";
        return 1;
    }

    std::cout << "[async] ok\n";
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " sync|async\n";
        return 2;
    }

    std::string mode = argv[1];
    if (mode == "sync") {
        return run_sync_tests();
    }
    if (mode == "async") {
        return run_async_tests();
    }

    std::cerr << "Unknown mode: " << mode << "\n";
    return 2;
}
