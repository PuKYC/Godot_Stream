#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>

namespace godot {

class StreamManager;

class StreamWorldProbe : public Node3D {
	GDCLASS(StreamWorldProbe, Node3D)

public:
	StreamWorldProbe() = default;
	~StreamWorldProbe() = default;

	void _ready() override;
	void _enter_tree() override;
	void _exit_tree() override;

	void _notification(int p_what);

	void set_aabb(const AABB &aabb);
	AABB get_aabb() const;

	void set_stream_manager_path(NodePath manager);
	NodePath get_stream_manager_path() const;

	AABB get_global_aabb() const;
protected:
	void _on_visibility_changed();

	static void _bind_methods();

private:
	void _resolve_and_cache_manager(); // 解析 stream_manager_path_ 并缓存指针

	AABB aabb_;
	NodePath stream_manager_path_;
	StreamManager *stream_manager_ = nullptr;
};

} //namespace godot
