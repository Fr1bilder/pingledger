#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include "monitor.hpp"

namespace pingledger {
class Winsock {
public:
    Winsock() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    }
    ~Winsock() { WSACleanup(); }
    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
};

class IcmpProbe {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
    IcmpProbe() : handle_(IcmpCreateFile()) {
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("IcmpCreateFile failed");
    }
    ~IcmpProbe() { IcmpCloseHandle(handle_); }
    IcmpProbe(const IcmpProbe&) = delete;
    IcmpProbe& operator=(const IcmpProbe&) = delete;

    Sample run(const std::string& target, unsigned timeoutMs) {
        Sample sample;
        addrinfo hints{}, *resolved = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        auto result = getaddrinfo(target.c_str(), nullptr, &hints, &resolved);
        if (result || !resolved) {
            if (resolved) freeaddrinfo(resolved);
            sample.detail = "DNS error " + std::to_string(result);
            return sample;
        }
        const auto address = reinterpret_cast<sockaddr_in*>(resolved->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(resolved);
        char payload[] = "PingLedger";
        std::vector<unsigned char> response(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8);
        DWORD replies = IcmpSendEcho(handle_, address, payload, sizeof(payload), nullptr,
            response.data(), static_cast<DWORD>(response.size()), timeoutMs);
        if (!replies) {
            sample.detail = "ICMP error " + std::to_string(GetLastError());
        } else {
            auto* reply = reinterpret_cast<ICMP_ECHO_REPLY*>(response.data());
            sample.success = reply->Status == IP_SUCCESS;
            sample.milliseconds = reply->RoundTripTime;
            char formatted[INET_ADDRSTRLEN]{};
            IN_ADDR actual{};
            actual.s_addr = reply->Address;
            inet_ntop(AF_INET, &actual, formatted, sizeof(formatted));
            sample.detail = sample.success ? formatted : "ICMP status " + std::to_string(reply->Status);
        }
        sample.time = std::chrono::system_clock::now();
        return sample;
    }
};
} // namespace pingledger
