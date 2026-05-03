#pragma once

#include "caches/cache.hpp"
#include "caches/lru_cache_policy.hpp"
#include "ankerl/unordered_dense.h"

// 缓存类型定义
template <typename Key, typename Value>
using lru_cache_t = typename caches::fixed_sized_cache<Key, Value, caches::LRUCachePolicy, ankerl::unordered_dense::map<Key, Value>>;
