#include "stream_world_probe.h"
#include "stream_manager.h" // 可选，仅用于类型转换，若不需要可注释
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void StreamWorldProbe::_bind_methods() {
	// 绑定属性 getter/setter
	ClassDB::bind_method(D_METHOD("set_aabb", "aabb"), &StreamWorldProbe::set_aabb);
	ClassDB::bind_method(D_METHOD("get_aabb"), &StreamWorldProbe::get_aabb);
	ClassDB::bind_method(D_METHOD("set_stream_manager_path", "path"), &StreamWorldProbe::set_stream_manager_path);
	ClassDB::bind_method(D_METHOD("get_stream_manager_path"), &StreamWorldProbe::get_stream_manager_path);

	// 绑定内部方法（供引擎回调）
	ClassDB::bind_method(D_METHOD("_on_visibility_changed"), &StreamWorldProbe::_on_visibility_changed);
	ClassDB::bind_method(D_METHOD("_connect_manager_signals"), &StreamWorldProbe::_connect_manager_signals);

	// 添加属性到编辑器
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "aabb"), "set_aabb", "get_aabb");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "stream_manager_path"), "set_stream_manager_path", "get_stream_manager_path");

	// 定义信号（带一个 Object 参数，传递探测器自身）
	MethodInfo mi_load("load_probe");
	ADD_SIGNAL(mi_load);

	MethodInfo mi_unload("unload_probe");
	ADD_SIGNAL(mi_unload);
}

void StreamWorldProbe::_ready() {
	// 连接自身的可见性变化信号
	if (!is_connected("visibility_changed", Callable(this, "_on_visibility_changed"))) {
		connect("visibility_changed", Callable(this, "_on_visibility_changed"));
	}

	_connect_manager_signals();

	if (is_visible_in_tree()) {
		emit_signal("load_probe", this);
	}
}

void StreamWorldProbe::_enter_tree() {
	if (is_visible_in_tree()) {
		emit_signal("load_probe", this);
	}
}

void StreamWorldProbe::_exit_tree() {
	// 离开树时主动发射卸载信号，避免残留
	if (is_visible_in_tree()) {
		emit_signal("unload_probe", this);
	}
}

void StreamWorldProbe::_on_visibility_changed() {
	if (!is_inside_tree()) {
		return;
	}

	if (is_visible_in_tree()) {
		emit_signal("load_probe", this);
	} else {
		emit_signal("unload_probe", this);
	}
}

void StreamWorldProbe::_connect_manager_signals() {
	// 若需要与 StreamManager 交互（如监听其销毁、重载事件），可在此实现
	// 示例：获取管理器节点，检查有效性（不强制要求）
	if (stream_manager_path_.is_empty()) {
		return;
	}
	auto *manager_node = get_node<StreamManager>(stream_manager_path_);
	if (!manager_node) {
		UtilityFunctions::push_warning("StreamWorldProbe: StreamManager not found at path: ", stream_manager_path_);
	}
	// 连接管理器回调
	manager_node->connect("load_probe", callable_mp(manager_node, &StreamManager::_on_load_probe), CONNECT_APPEND_SOURCE_OBJECT);
	manager_node->connect("unload_probe", callable_mp(manager_node, &StreamManager::_on_unload_probe), CONNECT_APPEND_SOURCE_OBJECT);
}

// 属性实现
void StreamWorldProbe::set_aabb(const AABB &aabb) {
	aabb_ = aabb;
	update_gizmos(); // 更新编辑器显示
}

AABB StreamWorldProbe::get_aabb() const {
	return aabb_;
}

void StreamWorldProbe::set_stream_manager_path(NodePath manager) {
	stream_manager_path_ = manager;
}

NodePath StreamWorldProbe::get_stream_manager_path() const {
	return stream_manager_path_;
}

} // namespace godot