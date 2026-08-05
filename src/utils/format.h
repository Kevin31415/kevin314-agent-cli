#pragma once

#include <string>
#include <cstdint>
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>

#ifndef _WIN32
    #include <sys/stat.h>
#endif

#ifdef _WIN32
    #define KACLI_LOCALTIME(buf, t) localtime_s(buf, t)
#else
    #define KACLI_LOCALTIME(buf, t) localtime_r(t, buf)
#endif

namespace goose {
namespace utils {

inline std::string format_time(int64_t timestamp) {
    std::time_t t = static_cast<std::time_t>(timestamp);
    struct tm tm_buf;
#ifdef _WIN32
    if (localtime_s(&tm_buf, &t) != 0) return "";
#else
    if (localtime_r(&t, &tm_buf) == nullptr) return "";
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

inline void set_file_permissions_private(const std::string& path) {
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
}

inline std::string generate_uuid() {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    std::ostringstream oss;
    for (int i = 0; i < 4; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(8) << dist(rng);
    }
    std::string raw = oss.str();
    raw[6] = '4';
    raw[8] = '8';
    return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" +
           raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" + raw.substr(20, 12);
}

} // namespace utils
} // namespace goose
