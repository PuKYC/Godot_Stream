#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <functional>

#include "ulid_generator.h"

UlidGenerator &UlidGenerator::thread_local_instance() {
    thread_local UlidGenerator instance;
    return instance;
}

std::string UlidGenerator::next() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    ulid::ULID result = ulid::Create(
        static_cast<time_t>(ms),
        [this]() -> uint8_t { return dist_(rng_); }
    );
    return ulid::Marshal(result);
}

ulid::ULID UlidGenerator::next_raw() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return ulid::Create(
        static_cast<time_t>(ms),
        [this]() -> uint8_t { return dist_(rng_); }
    );
}

UlidGenerator::UlidGenerator() {
    auto tid  = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::seed_seq seeds{
        rd(), rd(), rd(), rd(),
        static_cast<std::uint32_t>(tid),
        static_cast<std::uint32_t>(tid >> 32),
        static_cast<std::uint32_t>(now),
        static_cast<std::uint32_t>(now >> 32)
    };
    rng_.seed(seeds);
}
