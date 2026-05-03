#pragma once

#include "ankerl/unordered_dense.h"

template <
		class Key,
		class Hash = std::hash<Key>,
		class KeyEqual = std::equal_to<Key>,
		class Allocator = std::allocator<Key>>
using a_hashset = ankerl::unordered_dense::set<Key, Hash, KeyEqual, Allocator>;

template <
		class Key,
		class T,
		class Hash = std::hash<Key>,
		class KeyEqual = std::equal_to<Key>,
		class Allocator = std::allocator<std::pair<Key, T>>>
using a_hashmap = ankerl::unordered_dense::map<Key, T, Hash, KeyEqual, Allocator>;