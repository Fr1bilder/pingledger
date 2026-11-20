#include "monitor.hpp"
#include "probe.hpp"
#include <iostream>

int main() {
    using namespace pingledger;
    unsigned checks = 0;
    auto check = [&](bool condition, const char* message) {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    };
    try {
        auto targets = parseTargets(" Example.com,127.0.0.1;example.COM\r\n1.1.1.1 ");
        check(targets.size() == 3 && targets[0] == "example.com", "target normalization and deduplication");
        for (auto input : {"", "https://example.com", "host:80", "bad\"host"}) {
            bool rejected = false;
            try { parseTargets(input); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "invalid target accepted");
        }
        Statistics stats;
        check(health(stats, 150, 3) == Health::waiting && stats.loss() == 0, "empty stats");
        stats.add({{}, true, 10, ""});
        stats.add({{}, true, 30, ""});
        stats.add({{}, false, 0, "timeout"});
        check(stats.sent == 3 && stats.received == 2 && stats.average() == 20, "average counts failures");
        check(stats.minimum == 10 && stats.maximum == 30, "min max");
        check(std::abs(stats.loss() - 100.0 / 3) < 0.001, "packet loss");
        check(stats.jitter() == 20, "jitter");
        check(health(stats, 150, 3) == Health::slow, "transient failure");
        stats.add({{}, false, 0, ""});
        stats.add({{}, false, 0, ""});
        check(health(stats, 150, 3) == Health::down, "outage threshold");
        stats.add({{}, true, 160, ""});
        check(health(stats, 150, 3) == Health::slow, "latency threshold");
        stats.add({{}, true, 1, ""});
        check(health(stats, 150, 3) == Health::healthy && stats.consecutiveFailures == 0, "recovery");
        for (unsigned i = 0; i < 200; ++i) stats.add({{}, true, i, ""});
        check(stats.history.size() == 120 && stats.history.front().milliseconds == 80, "bounded history");
        check(csvField("a,\"b\"") == "\"a,\"\"b\"\"\"", "CSV escaping");
        auto row = csvRow("localhost", {{}, false, 0, "timeout"});
        check(row.find(",failed,,\"timeout\"") != std::string::npos, "failed latency must remain empty");
        Winsock winsock;
        IcmpProbe probe;
        auto local = probe.run("127.0.0.1", 1000);
        check(local.success, "real ICMP loopback request");
        auto bad = probe.run("invalid..domain", 500);
        check(!bad.success && bad.detail.find("DNS error") == 0, "DNS failure");
        std::cout << "PASS: " << checks << " checks, including ICMP loopback and DNS failure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
