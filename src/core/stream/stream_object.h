#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

class StreamManager; // 前置声明

class StreamObjectNode : public Node3D {
    GDCLASS(StreamObjectNode, Node3D)
    friend class StreamManager;

    String uuid;
    String parent_uuid;
    bool is_stream_remove = false;

    // 用于计算总包围盒的内部节点列表
    TypedArray<NodePath> aabb_sources;   // 手动配置的子节点路径
    Dictionary child_stream_aabbs;      // 子 StreamObjectNode 路径 -> AABB

public:
    // 外部只读访问
    String get_uuid() const { return uuid; }
    String get_parent_uuid() const { return parent_uuid; }

    // 总包围盒（包含自身及子流式对象）
    AABB get_total_aabb() const;

    // 节点生命周期钩子（只发信号，不做业务）
    void _ready() override;
    void _exit_tree() override;
    void _notification(int p_what);

protected:
    static void _bind_methods();

    // 供编辑器使用的属性
    void set_uuid(const String& id);
    void set_parent_uuid(const String& id);
    void set_aabb_sources(const TypedArray<NodePath>& arr);
    TypedArray<NodePath> get_aabb_sources() const;

	// signals
    // 通知管理器状态变化
    void object_entered_tree(StreamObjectNode* node);
    void object_exited_tree(StreamObjectNode* node);
    void object_aabb_changed(StreamObjectNode* node);
};