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

	void set_aabb(const AABB &aabb);
	AABB get_aabb() const;

	void set_stream_manager_path(NodePath manager);
	NodePath get_stream_manager_path() const;

protected:
	void _on_visibility_changed();

	void _ready() override;
	void _enter_tree() override;
	void _exit_tree() override;

	void _connect_manager_signals();

	static void _bind_methods();

private:
	AABB aabb_;
	NodePath stream_manager_path_;
};

} //namespace godot