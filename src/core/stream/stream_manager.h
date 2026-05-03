#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>

#include "../components/async_db_worker.h"
#include "../components/object_scene_cache.h"
#include "../components/object_data.h"
#include "../components/chunk.h"

#include "uuid.h"

#include <queue>
#include <vector>

/*
    object 不论是通过编辑器还是通过 manager 加载，object 进入和离开树的信号 都会连接到stream manager的回调上，
    通过回调来改变 manager 或者 object 的内部状态
*/
class StreamObjectNode;

class StreamManager : public godot::Node3D {
    GDCLASS(StreamManager, Node3D)

public:
    StreamManager();
    ~StreamManager();

    void _ready() override;
    void _process(double delta) override;

    // 公开接口（供编辑器/GDScript）
    void set_database_path(const String& path);
    String get_database_path() const;

    void query_aabb(const AABB& aabb);

    // 对象管理（由 StreamObjectNode 信号触发）
    void add_object(StreamObjectNode* node); // 可能通过 manager通知触发
    void remove_object(const uuids::uuid& uuid);
    void update_object(StreamObjectNode* node);

protected:
    static void _bind_methods();

private:
    // 状态
    String database_path_;
    String object_scene_dir_;

    // 数据库异步工作线程
    Ref<AsyncDbWorker> db_worker_;

    // 缓存与加载
    ObjectSceneCache cache_;
    std::queue<uuids::uuid> load_queue_;          // 待挂载的 UUID

    // 内存注册表：uuid → ObjectData（仅主线程访问）
    a_hashmap<uuids::uuid, ObjectData> registry_;
    // 父子关系辅助：parent_uuid → set<child_uuid>
    a_hashmap<uuids::uuid, a_hashset<uuids::uuid>> children_map_;

    a_hashset<uuids::uuid> pending_removal_;    // manager删除标志位
    a_hashset<uuids::uuid> _collect_descendants(const uuids::uuid &root) const; // 收集子对象

    // 待同步脏数据（主线程收集）
    a_hashset<uuids::uuid> to_upsert_;
    a_hashset<uuids::uuid> to_remove_;
    a_hashset<uuids::uuid> dirty_aabb_;

    // 内部方法
    void _connect_node_signals(StreamObjectNode* node);
    String derive_object_dir(const String &db_path) const;

    // 信号回调
    void _on_object_aabb_changed(StreamObjectNode *node);
    void _on_object_entered(StreamObjectNode *node);
    void _on_object_exited(StreamObjectNode *node);

    // 异步查询完成回调
    void _on_query_result(const a_hashmap<uuids::uuid, ObjectData>& db_objects);

    // 加载一个对象到场景树（从缓存或磁盘）
    void _load_object_scene(const uuids::uuid& uuid);
    // 删除一个对象 (缓存以及磁盘)
    void _delete_object_scene(const uuids::uuid& uuid);

    // 初始化数据库
    void _init_database(const String& path);

    // 卸载并缓存实例节点
    void _unload_object(const uuids::uuid& uuid);

    // 构建对象场景路径
    String _object_scene_path(const uuids::uuid &uuid) const;

    // 序列化并保存场景文件
    void _save_object_to_file(const uuids::uuid& uuid, Node* node);

    // 每帧末数据库同步
    void _flush_pending_db_ops();

};
