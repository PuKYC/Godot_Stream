#include "stream_world_probe.h"
#include "stream_manager.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void StreamWorldProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_aabb", "aabb"), &StreamWorldProbe::set_aabb);
	ClassDB::bind_method(D_METHOD("get_aabb"), &StreamWorldProbe::get_aabb);
	ClassDB::bind_method(D_METHOD("set_stream_manager_path", "path"), &StreamWorldProbe::set_stream_manager_path);
	ClassDB::bind_method(D_METHOD("get_stream_manager_path"), &StreamWorldProbe::get_stream_manager_path);

	ClassDB::bind_method(D_METHOD("_on_visibility_changed"), &StreamWorldProbe::_on_visibility_changed);

	ADD_PROPERTY(PropertyInfo(Variant::AABB, "aabb"), "set_aabb", "get_aabb");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "stream_manager_path"), "set_stream_manager_path", "get_stream_manager_path");
}

void StreamWorldProbe::_resolve_and_cache_manager() {
	stream_manager_ = nullptr;
	if (stream_manager_path_.is_empty() || !is_inside_tree())
		return;
	if (!has_node(stream_manager_path_))
		return;
	stream_manager_ = get_node<StreamManager>(stream_manager_path_);
}

void StreamWorldProbe::_ready() {
	if (!is_connected("visibility_changed", Callable(this, "_on_visibility_changed")))
		connect("visibility_changed", Callable(this, "_on_visibility_changed"), CONNECT_DEFERRED);

	_resolve_and_cache_manager();
	if (stream_manager_)
		stream_manager_->on_load_probe(this);
}

void StreamWorldProbe::_enter_tree() {
	if (is_visible_in_tree()) {
		_resolve_and_cache_manager();
		if (stream_manager_)
			stream_manager_->on_load_probe(this);
	}
}

void StreamWorldProbe::_exit_tree() {
	if (stream_manager_)
		stream_manager_->on_unload_probe(this);
	stream_manager_ = nullptr;
}

void StreamWorldProbe::_on_visibility_changed() {
	if (!is_inside_tree())
		return;

	_resolve_and_cache_manager();
	if (!stream_manager_)
		return;

	if (is_visible_in_tree())
		stream_manager_->on_load_probe(this);
	else
		stream_manager_->on_unload_probe(this);
}

void StreamWorldProbe::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE)
		stream_manager_ = nullptr;
}

void StreamWorldProbe::set_aabb(const AABB &aabb) {
	aabb_ = aabb;
	update_gizmos();
}

AABB StreamWorldProbe::get_aabb() const {
	return aabb_;
}

void StreamWorldProbe::set_stream_manager_path(NodePath manager) {
	stream_manager_path_ = manager;
	_resolve_and_cache_manager();
	if (stream_manager_ && is_visible_in_tree() && is_inside_tree())
		stream_manager_->on_load_probe(this);
}

NodePath StreamWorldProbe::get_stream_manager_path() const {
	return stream_manager_path_;
}

AABB StreamWorldProbe::get_global_aabb() const {
	auto aabb = get_aabb();
	return AABB(aabb.position + get_global_position() - aabb.size / 2, aabb.size);
}

} // namespace godot
