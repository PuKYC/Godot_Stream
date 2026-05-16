#pragma once

#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>

#include <uuid.h>

struct ObjectData {
	uuids::uuid parent_uuid = uuids::uuid(); // 父对象 UUID，根对象为uuids::uuid
	godot::ObjectID node_root = godot::ObjectID(); // 节点根对象, 不会保存到数据库
};