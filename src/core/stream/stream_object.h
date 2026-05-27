#pragma once

#include "uuid.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class StreamManager; // 前置声明

class StreamObjectNode : public Node3D {
	GDCLASS(StreamObjectNode, Node3D)
	friend class StreamManager;

	uuids::uuid uuid;
	uuids::uuid parent_uuid;

	// 用于计算总包围盒的内部节点列表
	TypedArray<NodePath> aabb_sources; // 历遍配置的子节点路径

	// 延迟信号标志：避免在 _notification 中发射信号（场景树可能不一致）
	bool aabb_changed_pending_ = false;

public:
	// 外部只读访问
	uuids::uuid get_uuid() const { return uuid; }
	uuids::uuid get_parent_uuid() const { return parent_uuid; }

	String get_uuid_str() const { return uuids::to_string(uuid).c_str(); }
	String get_parent_uuid_str() const { return uuids::to_string(parent_uuid).c_str(); }

	// 总包围盒（仅包含自身）
	AABB get_aabb() const;

	// 节点生命周期钩子（只发信号，不做业务）
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double delta) override;
	void _notification(int p_what);

protected:
	static void _bind_methods();

	void set_uuid_str(const String &id);
	void set_parent_uuid_str(const String &id);

	void set_uuid(const uuids::uuid &id);
	void set_parent_uuid(const uuids::uuid &id);

	void is_inited(); // 检查是否已经被分配uuid 有则代表已经在数据库注册

	void set_aabb_sources(const TypedArray<NodePath> &arr);
	TypedArray<NodePath> get_aabb_sources() const;
};

} //namespace godot