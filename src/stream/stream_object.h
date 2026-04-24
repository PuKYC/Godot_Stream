#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <string>
#include <ulid/ulid_uint128.hh>

using namespace godot;

class StreamObject : public Node3D {
	GDCLASS(StreamObject, Node3D)
public:
	StreamObject() = default;
	~StreamObject() = default;

	void _ready() override;
	void _exit_tree() override;
	void _notification(int p_what);
	//void _process(double delta) override;

	ulid::ULID ulid = ulid::ULID(0);
	ulid::ULID parent_ulid = ulid::ULID(0);

	NodePath stream_path;

	//到时直接监听直接子节点的增删来更改节点列表
	void update_aabb_list();
	void update_aabb_stream();

	void set_ulid(const String &var_ulid);
	String get_ulid();

	void set_parent_ulid(const String &var_parent_ulid);
	String get_parent_ulid();

	void set_aabb_objects(TypedArray<NodePath> list);
	TypedArray<NodePath> get_aabb_objects() const;

	void set_aabb_stream(Dictionary aabb_stream);
	Dictionary get_aabb_stream() const;

	AABB get_object_aabb();

	void update();

	void stream_remove();
	bool is_stream_remove = false;

protected:
	static void _bind_methods();

	bool added_as_scene;
	void update_stream_path();
	void update_parent_ulid();

	TypedArray<NodePath> aabb_objects;
	Dictionary aabb_stream;// nodepath aabb
};