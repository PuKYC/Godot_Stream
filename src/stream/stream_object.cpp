#include "stream_object.h"
#include "debug_def.h"
#include "stream_manager.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <sstream>

AABB transform_aabb(const AABB &aabb, const Transform3D &transform) {
	// 获取原始 AABB 的 8 个顶点
	Vector3 vertices[8];
	vertices[0] = aabb.position;
	vertices[1] = aabb.position + Vector3(aabb.size.x, 0, 0);
	vertices[2] = aabb.position + Vector3(0, aabb.size.y, 0);
	vertices[3] = aabb.position + Vector3(0, 0, aabb.size.z);
	vertices[4] = aabb.position + Vector3(aabb.size.x, aabb.size.y, 0);
	vertices[5] = aabb.position + Vector3(aabb.size.x, 0, aabb.size.z);
	vertices[6] = aabb.position + Vector3(0, aabb.size.y, aabb.size.z);
	vertices[7] = aabb.position + aabb.size;

	// 变换所有顶点
	Vector3 transformed[8];
	for (int i = 0; i < 8; ++i) {
		transformed[i] = transform.xform(vertices[i]); // Godot 4: transform * vertices[i] 或 transform.xform()
	}

	// 计算新的包围盒
	AABB new_aabb(transformed[0], Vector3());
	for (int i = 1; i < 8; ++i) {
		new_aabb.expand_to(transformed[i]);
	}
	return new_aabb;
}
void StreamObject::_ready() {
	set_notify_transform(true);

	if (Engine::get_singleton()->is_editor_hint()) {
		if (get_tree()->get_edited_scene_root() == get_parent()) {
			added_as_scene = false;
		} else {
			added_as_scene = true;
		}
	} else {
		// 以下代码仅在运行时执行
		if (get_tree()->get_current_scene() == get_parent()) {
			added_as_scene = false;
		} else {
			added_as_scene = true;
		}
	}

	if (added_as_scene) {
		update_stream_path();

		if (has_node(stream_path) && ulid == ulid::ULID(0)) {
			update_parent_ulid();

			auto stream = get_node<StreamManager>(stream_path);

			// 在第一次添加到树时，同一帧移出树编辑器会出现报错，所以需要延迟调用
			// stream->add_object(*this);
			Callable add = callable_mp(stream, &StreamManager::add_object);
			add.call_deferred(this);
		}
	}

	DEBUG_CODE({ UtilityFunctions::print("StreamObject ready.", "added_as_scene:", added_as_scene); });
}

void StreamObject::_exit_tree() {
	DEBUG_CODE({ UtilityFunctions::print("StreamObject exit_tree."); });

	if (!is_stream_remove && has_node(stream_path)) {
		auto stream = get_node<StreamManager>(stream_path);

		Callable remove = callable_mp(stream, &StreamManager::remove_object);
		remove.call_deferred(ulid::Marshal(ulid).c_str());

		DEBUG_CODE({ UtilityFunctions::print("StreamObject removed from tree. ", ulid::Marshal(ulid).c_str()); });
	}
}

void StreamObject::_notification(int p_what) {
	//DEBUG_CODE({ UtilityFunctions::print("StreamObject notification:", p_what); });
	if (is_node_ready() && is_inside_tree()) {
		if (added_as_scene) {
			// 需要使用检测node直接节点变化的信号提高性能
			update_aabb_stream();

			switch (p_what) {
				case NOTIFICATION_PARENTED:
				case NOTIFICATION_ENTER_TREE:
				case NOTIFICATION_PATH_RENAMED: {
					DEBUG_CODE({ UtilityFunctions::print("StreamObject parented or path renamed."); });

					update_stream_path();

					if (!has_node(stream_path)) {
						return;
					}
					update_parent_ulid();

					auto stream = get_node<StreamManager>(stream_path);
					stream->update_object(this);

					break;
				}

				// 一帧触发多次 不用这么高频
				case NOTIFICATION_TRANSFORM_CHANGED: {
					DEBUG_CODE(UtilityFunctions::print("Object move"););

					if (!has_node(stream_path)) {
						return;
					}

					auto stream = get_node<StreamManager>(stream_path);
					stream->update_object(this);

					break;
				}
			}
		} else {
			update_aabb_list();
		}
	}
}

void StreamObject::update_aabb_list() {
	if (is_inside_tree()) {
		auto parent = get_parent();
		if (!added_as_scene && is_node_ready()) {
			godot::TypedArray<godot::NodePath> new_aabb_object;

			for (auto var : get_parent()->find_children("*", "VisualInstance3D", true)) {
				new_aabb_object.append(get_path_to(Object::cast_to<Node>(var)));
			};

			set_aabb_objects(new_aabb_object);
		}
	}
}

void StreamObject::update_aabb_stream() {
	if (is_inside_tree()) {
		auto parent = get_parent();
		aabb_stream.clear();

		for (auto var : parent->get_children()) {
			auto node = Object::cast_to<Node>(var);
			for (auto children : node->get_children()) {
				if (Object::cast_to<Node>(children)->is_class("StreamObject")) {
					auto obj = Object::cast_to<StreamObject>(children);
					aabb_stream[get_parent()->get_path_to(obj)] = obj->get_object_aabb();
				}
			}
		}
	}
}

void StreamObject::set_ulid(const String &var_ulid) {
	ulid = ulid::Unmarshal(var_ulid.utf8().get_data());
}

String StreamObject::get_ulid() {
	return String(ulid::Marshal(ulid).c_str());
}

void StreamObject::set_parent_ulid(const String &var_parent_ulid) {
	parent_ulid = ulid::Unmarshal(var_parent_ulid.utf8().get_data());
}

String StreamObject::get_parent_ulid() {
	return String(ulid::Marshal(parent_ulid).c_str());
}

void StreamObject::set_aabb_objects(TypedArray<NodePath> list) {
	DEBUG_CODE({ UtilityFunctions::print("StreamObject set_aabb_objects."); });
	aabb_objects = std::move(list);
}

TypedArray<NodePath> StreamObject::get_aabb_objects() const {
	return aabb_objects;
}

void StreamObject::set_aabb_stream(Dictionary aabb_stream) {
	aabb_stream = std::move(aabb_stream);
}

Dictionary StreamObject::get_aabb_stream() const {
	return aabb_stream;
}

// 需要添加缓存，以及脏标记提高性能
AABB StreamObject::get_object_aabb() {
	auto parent = get_parent();

	AABB aabb;
	for (auto var : aabb_objects) {
		const NodePath node_path = var;
		if (has_node(node_path)) {
			auto obj = get_node<VisualInstance3D>(node_path);
			if (!obj->is_inside_tree())
				;
			aabb = aabb.merge(transform_aabb(obj->get_aabb(), obj->get_global_transform()));
		}
	}

	auto keys = aabb_stream.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const NodePath path = keys[i];

		auto obj = parent->get_node<StreamObject>(path);
		aabb = aabb.merge(obj->get_object_aabb());
	}

	return aabb;
}

void StreamObject::update() {
	if (has_node(stream_path)) {
		get_node<StreamManager>(stream_path)->update_object(this);
	}
}

void StreamObject::stream_remove() {
	is_stream_remove = true;

	auto obj_path = get_aabb_stream().keys();
	for (auto node : obj_path) {
		if (has_node(node))
			get_node<StreamObject>(node)->stream_remove();
	}
}

void StreamObject::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_stream_path"), &StreamObject::update_stream_path);
	ClassDB::bind_method(D_METHOD("update_parent_ulid"), &StreamObject::update_parent_ulid);

	// 绑定 aabb_objects 属性的 getter/setter
	ClassDB::bind_method(D_METHOD("set_aabb_objects", "val"), &StreamObject::set_aabb_objects);
	ClassDB::bind_method(D_METHOD("get_aabb_objects"), &StreamObject::get_aabb_objects);

	// 注册 aabb_objects 属性
	PropertyInfo info_objects(Variant::ARRAY, "aabb_objects");
	info_objects.usage = PROPERTY_USAGE_STORAGE; // 仅用于存储，不显示在编辑器中
	ClassDB::add_property(get_class_static(), info_objects, "set_aabb_objects", "get_aabb_objects");

	// 绑定 aabb_stream 属性的 getter/setter
	ClassDB::bind_method(D_METHOD("set_aabb_stream", "val"), &StreamObject::set_aabb_stream);
	ClassDB::bind_method(D_METHOD("get_aabb_stream"), &StreamObject::get_aabb_stream);

	// 注册 aabb_stream 属性
	PropertyInfo info_stream(Variant::ARRAY, "aabb_stream");
	info_stream.usage = PROPERTY_USAGE_STORAGE; // 仅用于存储，不显示在编辑器中
	ClassDB::add_property(get_class_static(), info_stream, "set_aabb_stream", "get_aabb_stream");

	ClassDB::bind_method(D_METHOD("set_ulid", "var"), &StreamObject::set_ulid);
	ClassDB::bind_method(D_METHOD("get_ulid"), &StreamObject::get_ulid);

	// 添加只读标志：编辑器显示但不允许修改
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "ulid", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_STORAGE),
				 "set_ulid", "get_ulid");

	ClassDB::bind_method(D_METHOD("set_parent_ulid", "var"), &StreamObject::set_parent_ulid);
	ClassDB::bind_method(D_METHOD("get_parent_ulid"), &StreamObject::get_parent_ulid);

	// 添加只读标志：编辑器显示但不允许修改
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "parent_ulid", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_STORAGE),
				 "set_parent_ulid", "get_parent_ulid");
}

void StreamObject::update_stream_path() {
	if (is_inside_tree()) {
		auto canvas = get_parent();
		while (canvas && !canvas->is_class("StreamManager")) {
			canvas = canvas->get_parent();
		}

		if (canvas) {
			stream_path = canvas->get_path();
		} else {
			DEBUG_CODE({ UtilityFunctions::print("Not find stream manager"); });
		};

		DEBUG_CODE({ UtilityFunctions::print("Stream path changed"); });
	}

	else {
		DEBUG_CODE({ UtilityFunctions::print("Node not in tree, path unavailable"); });
	}
}

void StreamObject::update_parent_ulid() {
	if (is_inside_tree()) {
		auto canvas = get_parent()->get_parent();
		while (canvas) {
			for (auto var : canvas->get_children()) {
				Node *child = Object::cast_to<Node>(var);
				if (child->is_class("StreamObject")) {
					set_parent_ulid(ulid::Marshal(Object::cast_to<StreamObject>(child)->ulid).c_str());
					break;
				}
			}

			canvas = canvas->get_parent();
		}

		DEBUG_CODE({
			UtilityFunctions::print("Parent ulid changed: ", String(ulid::Marshal(parent_ulid).c_str()));
		});
	}

	else {
		DEBUG_CODE({ UtilityFunctions::print("Not find parent"); });
	}
}
