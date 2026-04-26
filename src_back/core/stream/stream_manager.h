#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include "../components/object_data.h"
#include "../components/object_scene_cache.h"
#include "../components/async_db_worker.h"

class StreamObjectNode;

class StreamManager : public Node3D {
    GDCLASS(StreamManager, Node3D)

    // 数据库路径
    String database_path;
    String object_scene_dir;

    // 异步数据库工作者
    Ref<AsyncDbWorker> db_worker;

    // 对象注册表：UUID -> ObjectData
    std::unordered_map<String, ObjectData> object_registry;
    // 父子关系辅助：parent_uuid -> set<child_uuid>
    std::unordered_map<String, std::unordered_set<String>> children_map;
    // 待处理的数据库写操作（在主线程收集，定期批量提交）
    std::unordered_set<String> to_upsert;
    std::unordered_set<String> to_remove;

    // 场景加载管理
    ObjectCache scene_cache;
    std::queue<String> load_queue;       // 待加载的 UUID
    std::queue<String> loading_in_progress; // 加载中的 UUID

public:
    void _ready() override;
    void _process(double delta) override;

    // 查询并加载 AABB 区域内的对象
    void query_aabb_async(const AABB& aabb);

    // 手动添加/移除/更新对象（仍保留接口，但内部通过信号触发）
    void add_object(StreamObjectNode* obj);
    void remove_object(const String& uuid);
    void update_object(StreamObjectNode* obj);

protected:
    static void _bind_methods();

private:
    // 信号连接槽函数
    void _on_object_entered(StreamObjectNode* node);
    void _on_object_exited(StreamObjectNode* node);
    void _on_object_aabb_changed(StreamObjectNode* node);

    // 异步获取查询结果后，与内存状态合并，执行加载/卸载
    void _on_query_result(const std::vector<ObjectData>& db_objects,
                          const AABB& original_aabb); // 回调闭包

    // 内部方法
    void _init_database(const String& path);
    void _upsert_objects_in_db();
    void _remove_objects_in_db();

    // 加载一个对象的场景到场景树
    void _load_object_into_tree(const String& uuid);

    // 保存并卸载对象
    void _save_and_unload_object(const String& uuid);

    // 创建对象场景文件路径
    String _object_scene_path(const String& uuid) const;
};

