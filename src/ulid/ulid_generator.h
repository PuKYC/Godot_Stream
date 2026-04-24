#include <string>
#include <random>
#include <thread>
#include <chrono>
#include <functional>
#include "ulid_uint128.hh"  

class UlidGenerator {
public:
    // 获取当前线程专属的生成器（懒初始化）
    static UlidGenerator& thread_local_instance();

    // 禁止拷贝和移动
    UlidGenerator(const UlidGenerator&) = delete;
    UlidGenerator& operator=(const UlidGenerator&) = delete;

    // 生成字符串格式的ULID
    std::string next();

    // 生成原始ULID对象
    ulid::ULID next_raw();

private:
    UlidGenerator();  // 私有构造函数，只允许通过 thread_local_instance 访问

    std::mt19937_64 rng_;
    std::uniform_int_distribution<uint8_t> dist_{0, 255};
};