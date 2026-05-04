#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/core/object_id.hpp>

#include <uuid.h>

using namespace godot;

struct ObjectData {
    uuids::uuid parent_uuid = uuids::uuid();   // 父对象 UUID，根对象为uuids::uuid
    int32_t chunk_id = -1;      // 根据wolrd_aabb 计算
    AABB world_aabb;      // 世界空间包围盒（同步更新） 不会保存到数据库

    ObjectID node_root;     // 节点根对象, 不会保存到数据库
};