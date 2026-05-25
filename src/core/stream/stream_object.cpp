#include "stream_object.h"

#include "uuid.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

using namespace godot;

// 辅助：变换 AABB
static AABB transform_aabb(const AABB &local_aabb, const Transform3D &transform) {
	Vector3 vertices[8] = {
		local_aabb.position,
		local_aabb.position + Vector3(local_aabb.size.x, 0, 0),
		local_aabb.position + Vector3(0, local_aabb.size.y, 0),
		local_aabb.position + Vector3(0, 0, local_aabb.size.z),
		local_aabb.position + Vector3(local_aabb.size.x, local_aabb.size.y, 0),
		local_aabb.position + Vector3(local_aabb.size.x, 0, local_aabb.size.z),
		local_aabb.position + Vector3(0, local_aabb.size.y, local_aabb.size.z),
		local_aabb.position + local_aabb.size
	};

	AABB world_aabb(transform.xform(vertices[0]), Vector3());
	for (int i = 1; i < 8; ++i) {
		world_aabb.expand_to(transform.xform(vertices[i]));
	}
	return world_aabb;
}

void StreamObjectNode::_ready() {
	set_notify_transform(true);
}

void StreamObjectNode::_notification(int p_what) {
	// 仅在节点就绪且处于场景树中时处理
	if (!is_node_ready() || !is_inside_tree())
		return;

	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		call_deferred("emit_signal", "object_aabb_changed");
	}
}

// 仅获取自身aabb 不包括子对象
AABB StreamObjectNode::get_aabb() const {
	AABB total = AABB(get_global_position(), Vector3(0, 0, 0));

	if (aabb_sources.size() == 0 or aabb_sources.is_empty()) {
		return total;
	}

	// 合并 aabb_sources 中指定的视觉实例（已在场景树中）
	for (int i = 0; i < aabb_sources.size(); ++i) {
		const NodePath path = aabb_sources[i];
		if (has_node(path)) {
			Node *n = get_node<Node>(path);
			VisualInstance3D *vis = Object::cast_to<VisualInstance3D>(n);
			if (vis && vis->is_inside_tree()) {
				AABB world = transform_aabb(vis->get_aabb(), vis->get_global_transform());

				total = total.merge(world);
			}
		}
	}

	return total;
}

void StreamObjectNode::is_inited() {
	// uuid 为空（全零）表示尚未注册到数据库
	if (uuid.is_nil()) {
		// 仅打印警告，不中断
		WARN_PRINT(vformat("StreamObjectNode '%s' has no valid UUID.", get_name()));
	}
}

// 属性绑定
void StreamObjectNode::_bind_methods() {
	// 只读 uuid
	ClassDB::bind_method(D_METHOD("get_uuid"), &StreamObjectNode::get_uuid_str);
	ClassDB::bind_method(D_METHOD("set_uuid", "id"), &StreamObjectNode::set_uuid_str);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "uuid", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_READ_ONLY),
			"set_uuid", "get_uuid");

	// 只读 parent_uuid
	ClassDB::bind_method(D_METHOD("get_parent_uuid"), &StreamObjectNode::get_parent_uuid_str);
	ClassDB::bind_method(D_METHOD("set_parent_uuid", "id"), &StreamObjectNode::set_parent_uuid_str);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "parent_uuid", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_READ_ONLY),
			"set_parent_uuid", "get_parent_uuid");

	// aabb_sources 可配置
	ClassDB::bind_method(D_METHOD("set_aabb_sources", "arr"), &StreamObjectNode::set_aabb_sources);
	ClassDB::bind_method(D_METHOD("get_aabb_sources"), &StreamObjectNode::get_aabb_sources);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "aabb_sources", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR),
			"set_aabb_sources", "get_aabb_sources");

	MethodInfo object_aabb_changed;
	object_aabb_changed.name = "object_aabb_changed";
	ADD_SIGNAL(object_aabb_changed);
}

void StreamObjectNode::set_uuid_str(const String &id) {
	auto opt_uuid = uuids::uuid::from_string(id.utf8().get_data());
	if (opt_uuid.has_value()) {
		set_uuid(opt_uuid.value());
	} else {
		// 如果字符串无效，可以设置为nil或抛出错误
		ERR_PRINT(vformat("Invalid UUID string: %s", id.utf8().get_data()));
	}
}

void StreamObjectNode::set_parent_uuid_str(const String &id) {
	auto opt_uuid = uuids::uuid::from_string(id.utf8().get_data());
	if (opt_uuid.has_value()) {
		set_parent_uuid(opt_uuid.value());
	} else {
		// 如果字符串无效，可以设置为nil或抛出错误
		ERR_PRINT(vformat("Invalid UUID string: %s", id.utf8().get_data()));
	}
}

void StreamObjectNode::set_uuid(const uuids::uuid &id) {
	uuid = id;
}

void StreamObjectNode::set_parent_uuid(const uuids::uuid &id) {
	parent_uuid = id;
}

void StreamObjectNode::set_aabb_sources(const TypedArray<NodePath> &arr) {
	aabb_sources = arr;
}

TypedArray<NodePath> StreamObjectNode::get_aabb_sources() const {
	return aabb_sources;
}
