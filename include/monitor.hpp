#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pingledger {
struct Sample {
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now();
    bool success = false;
    unsigned milliseconds = 0;
    std::string detail;
};

struct Statistics {
    std::uint64_t sent = 0, received = 0;
    std::uint64_t total = 0;
    unsigned minimum = 0, maximum = 0;
    unsigned consecutiveFailures = 0;
    std::deque<Sample> history;
    void add(const Sample& sample) {
        ++sent;
        if (sample.success) {
            minimum = received ? std::min(minimum, sample.milliseconds) : sample.milliseconds;
            maximum = std::max(maximum, sample.milliseconds);
            total += sample.milliseconds;
            ++received;
            consecutiveFailures = 0;
        } else {
            ++consecutiveFailures;
        }
        history.push_back(sample);
        if (history.size() > 120) history.pop_front();
    }
    double loss() const { return sent ? 100.0 * static_cast<double>(sent - received) / static_cast<double>(sent) : 0; }
    double average() const { return received ? static_cast<double>(total) / static_cast<double>(received) : 0; }
    double jitter() const {
        double sum = 0;
        unsigned pairs = 0;
        for (std::size_t i = 1; i < history.size(); ++i) {
            if (history[i - 1].success && history[i].success) {
                sum += std::abs(static_cast<double>(history[i].milliseconds) - history[i - 1].milliseconds);
                ++pairs;
            }
        }
        return pairs ? sum / pairs : 0;
    }
};

enum class Health { waiting, healthy, slow, down };
inline Health health(const Statistics& stats, unsigned threshold, unsigned failureLimit) {
    if (stats.history.empty()) return Health::waiting;
    if (stats.consecutiveFailures >= failureLimit) return Health::down;
    if (!stats.history.back().success || stats.history.back().milliseconds > threshold) return Health::slow;
    return Health::healthy;
}

inline std::vector<std::string> parseTargets(const std::string& input) {
    std::vector<std::string> result;
    std::string token;
    auto append = [&] {
        if (token.empty()) return;
        if (token.size() > 253) throw std::invalid_argument("Address exceeds 253 characters");
        for (unsigned char c : token) {
            if (!(std::isalnum(c) || c == '-' || c == '.'))
                throw std::invalid_argument("Use IPv4 or an ASCII domain, without protocol or port");
        }
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(result.begin(), result.end(), token) == result.end()) result.push_back(token);
        token.clear();
    };
    for (unsigned char c : input) {
        if (std::isspace(c) || c == ',' || c == ';') append();
        else token += static_cast<char>(c);
    }
    append();
    if (result.empty() || result.size() > 16) throw std::invalid_argument("Enter between 1 and 16 unique addresses");
    return result;
}

inline std::string csvField(const std::string& value) {
    std::string out = "\"";
    for (char c : value) out += c == '"' ? "\"\"" : std::string(1, c);
    return out + "\"";
}
inline std::string timestamp(std::chrono::system_clock::time_point value) {
    const auto epoch = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &epoch);
#else
    localtime_r(&epoch, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}
inline std::string csvRow(const std::string& target, const Sample& sample) {
    return csvField(timestamp(sample.time)) + "," + csvField(target) + "," +
        (sample.success ? "ok," + std::to_string(sample.milliseconds) : "failed,") + "," + csvField(sample.detail) + "\n";
}
} // namespace pingledger
