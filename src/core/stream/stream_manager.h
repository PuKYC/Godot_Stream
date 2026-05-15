#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/core/object_id.hpp>

#include "stream_object.h"
#include "stream_world_probe.h"

#include "../components/async_db_worker.h"
#include "../components/chunk.h"
#include "../components/object_data.h"
#include "../components/object_scene_cache.h"

#include "uuid.h"

#include <queue>
#include <vector>

namespace godot {

class StreamObjectNode;

class StreamManager : public Node3D {
	GDCLASS(StreamManager, Node3D)

public:
	StreamManager();
	~StreamManager();

	void _ready() override;
	void _process(double delta) override;

	// 公开接口（供编辑器/GDScript）
	void set_database_path(const String &path);
	String get_database_path() const;

	// object信号回调
	void _on_object_aabb_changed(StreamObjectNode *node); // aabb改变时
	void _on_object_entered(Node *node); // 进入场景树时
	void _on_object_exited(Node *node); // 离开场景树时
	// probe信号回调
	void _on_load_probe(StreamWorldProbe *probe);
	void _on_unload_probe(StreamWorldProbe *probe);

	// 对象管理（由 StreamObjectNode 信号触发）
	void add_object(StreamObjectNode *node); // 可能通过 manager通知触发
	void remove_object(const uuids::uuid &uuid);
	void update_object(StreamObjectNode *node);

protected:
	static void _bind_methods();

private:
	// 状态
	String database_path_;
	String object_scene_dir_;
	uint8_t query_process = 0;// 计数器

	// 数据库异步工作线程
	Ref<AsyncDbWorker> db_worker_;

	// 缓存与加载
	ObjectSceneCache cache_;
	std::queue<uuids::uuid> load_queue_; // 待挂载的 UUID

	// 内存注册表：uuid → ObjectData（仅主线程访问）
	a_hashmap<uuids::uuid, ObjectData> registry_;
	// 父子关系辅助：parent_uuid → set<child_uuid>
	a_hashmap<uuids::uuid, a_hashset<uuids::uuid>> children_map_;

	a_hashset<uuids::uuid> pending_removal_; // manager删除标志位

	// 待同步脏数据（主线程收集）
	a_hashset<uuids::uuid> to_upsert_;
	a_hashset<uuids::uuid> to_remove_;
	a_hashset<uuids::uuid> dirty_aabb_;

	// 已注册probe
	a_hashset<uint64_t> registered_probes_;

	// 内部方法
	static uuids::uuid _generate_uuid();
	void _connect_node_signals(StreamObjectNode *node);
	String _derive_object_dir(const String &db_path) const;
	a_hashset<uuids::uuid> _collect_descendants(const uuids::uuid &root) const; // 收集子对象
	void _query_aabb(std::vector<AABB> &aabbs);
	void _query_aabb(const AABB &aabb);

	// 异步查询完成回调
	void _on_query_result(const a_hashmap<uuids::uuid, ObjectData> &db_objects);

	// 加载一个对象到场景树（从缓存或磁盘）
	void _load_object_scene(const uuids::uuid &uuid);
	// 删除一个对象 (缓存以及磁盘)
	void _delete_object_scene(const uuids::uuid &uuid);

	// 初始化数据库
	void _init_database(const String &path);

	// 卸载并缓存实例节点
	void _unload_object(const uuids::uuid &uuid);

	// 构建对象场景路径
	String _object_scene_path(const uuids::uuid &uuid) const;

	// 序列化并保存场景文件
	void _save_object_to_file(const uuids::uuid &uuid, Node *node);

	// 每帧末数据库同步
	void _flush_pending_db_ops();
};

} //namespace godot