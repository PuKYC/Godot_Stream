#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include <godot_cpp/variant/aabb.hpp>

struct Chunk {
	int32_t x, y, z;
	uint8_t level;

	bool operator==(const Chunk &other) const {
		return x == other.x && y == other.y && z == other.z && level == other.level;
	}

	// 计算 chunk
	static Chunk compute_chunk(const godot::AABB &aabb) {
		float m = std::max({ aabb.size.x, aabb.size.y, aabb.size.z, 0.0f });

		uint8_t level;

		if (m <= 1.0f) {
			level = 0;
		} else {
			level = static_cast<int>(std::ceil(std::log2(m) / 2.0));

			if (level >= 0 && level < 32 && ((1ULL << (2 * level)) < static_cast<uint64_t>(m))) {
				++level;
			};
		}
		level = (level < 0 || level > 31) ? 0 : level;

		int32_t x, y, z;
		// 用无符号掩码，避免 (-1 << n) 的负数左移 UB
		uint32_t mask = (2 * level < 32) ? ~((1u << (2 * level)) - 1u) : 0u;
		x = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.x)) & mask);
		y = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.y)) & mask);
		z = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.z)) & mask);

		return { x, y, z, level };
	};
};

namespace std {
	template <>
	struct hash<Chunk> {
		size_t operator()(const Chunk &chunk) const noexcept {
			size_t h1 = hash<int32_t>()(chunk.x);
			size_t h2 = hash<int32_t>()(chunk.y);
			size_t h3 = hash<int32_t>()(chunk.z);
			size_t h4 = hash<uint8_t>()(chunk.level);
			size_t seed = h1;
			seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			seed ^= h4 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			return seed;
		}
	};
} //namespace std