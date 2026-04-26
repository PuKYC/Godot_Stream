#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

struct ObjectData {
    String uuid;          // 对象唯一标识 (UUID v4 字符串)
    String parent_uuid;   // 父对象 UUID，根对象为预定义常量 `ROOT_PARENT_ID`
    int32_t chunk_id = -1;
    AABB world_aabb;      // 世界空间包围盒（同步更新）
    bool is_loaded = false;

    // 比较相等性
    bool operator==(const ObjectData& o) const {
        return uuid == o.uuid;
    }
};

constexpr const char* ROOT_PARENT_ID = "00000000-0000-0000-0000-000000000000";