/* TODO
 * 在godot编辑器中暴露缓存配置 实现父子关系 加入编辑器缓存 实现只读数据库
 */
#include "stream_manager.h"

#include <chrono>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

// 构造 析构
StreamManager::StreamManager() :
		cache_(16, 32) {
	// 缓存容量可后续调整为可配置属性

	// 连接自身回调
	connect("child_entered_tree", callable_mp(this, &StreamManager::_on_object_entered));
	connect("child_exiting_tree", callable_mp(this, &StreamManager::_on_object_exited));
}

StreamManager::~StreamManager() {
	// 确保数据库操作完成
	if (db_worker_.is_valid()) {
		_flush_pending_db_ops(); // 最后一次同步
	}
} // db_worker_ Ref 在此处析构 → worker 线程可能已 join

void StreamManager::_ready() {
	if (!database_path_.is_empty()) {
		_init_database(database_path_);
	}
}

void godot::StreamManager::_exit_tree() {
	object_removal_.clear();
}

void StreamManager::_process(double delta) {
	// FIXME 时机不对
	// 执行删除
	for (auto uuid : object_removal_) {
		_remove_object(uuid);
	}
	object_removal_.clear();

	// 异步数据库回调执行
	if (db_worker_.is_valid()) {
		// 批量数据库同步（含脏 AABB 处理）
		_flush_pending_db_ops();

		// 执行上一帧的任务回调
		db_worker_->flush_callbacks();

		// 执行查询
		query_process++;
		if (query_process >= 4) {
			std::vector<AABB> query_aabbs;
			for (auto id : registered_probes_) {
				StreamWorldProbe *probe = Object::cast_to<StreamWorldProbe>(ObjectDB::get_instance(id));
				if (probe) {
					query_aabbs.push_back(probe->get_global_aabb());
				} else {
					// DEBUG: 探针已被销毁但未从注册表中移除，属于逻辑错误
					WARN_PRINT("StreamWorldProbe instance no longer valid, removing from registered_probes_.");
					registered_probes_.erase(id);
				}
			}

			if (!query_aabbs.empty()) {
				_query_aabb(query_aabbs);
			}

			query_process = 0;
		}
	};

	// 轮询场景缓存异步加载状态
	cache_.update();

	// 处理待加载队列（分批进行，避免单帧负载过高）
	const int MAX_LOADS_PER_FRAME = 4;
	int loads = 0;
	while (!load_queue_.empty() && loads < MAX_LOADS_PER_FRAME) {
		uuids::uuid uuid = load_queue_.front();
		load_queue_.pop();
		_load_object_scene(uuid);

		++loads;
	}
	while (!loaded_queue_.empty()) {
		uuids::uuid uuid = loaded_queue_.front();
		load_queue_.push(uuid);
		loaded_queue_.pop();
	}
}

// 数据库初始化
void StreamManager::_init_database(const String &path) {
	// DEBUG: 数据库路径不能为空
	ERR_FAIL_COND_MSG(path.is_empty(), "Cannot initialize database with an empty path.");

	object_scene_dir_ = _derive_object_dir(path);

	// 确保目录存在
	if (!DirAccess::dir_exists_absolute(object_scene_dir_)) {
		Error err = DirAccess::make_dir_absolute(object_scene_dir_);
		if (err != OK) {
			ERR_PRINT("Failed to create object scene directory: " + object_scene_dir_);
			return; // 目录创建失败则无法继续
		}
	}

	// 创建异步数据库 worker（独立线程）
	db_worker_ = Ref<AsyncDbWorker>(memnew(AsyncDbWorker(path)));
	// DEBUG: worker 创建后应确保有效
	DEV_ASSERT(db_worker_.is_valid());

	db_worker_->push_task({
			[](StreamSqliteDB &db) {
				// 可选：执行启动时的数据库维护或读取版本信息
			},
			[]() {} // 无需回调
	});
}

// 公开接口
void StreamManager::set_database_path(const String &path) {
	database_path_ = path;
	// 若已运行，应立即初始化数据库
	if (is_node_ready())
		_init_database(path);
}

String StreamManager::get_database_path() const {
	return database_path_;
}

void StreamManager::_query_aabb(const AABB &aabb) {
	auto result_ptr = std::make_shared<a_hashmap<uuids::uuid, ObjectData>>();
	db_worker_->push_task({ [aabb, result_ptr](StreamSqliteDB &db) {
							   // 将查询结果赋值给 shared_ptr 指向的 map
							   *result_ptr = db.query_objects(aabb);
						   },
			[this, result_ptr]() {
				_on_query_result(*result_ptr);
			} });
}

//  对象管理（由信号触发 不一定）
void StreamManager::add_object(StreamObjectNode *node) {
	// DEBUG: node 必须非空
	ERR_FAIL_COND_MSG(!node, "add_object called with a null node.");

	uuids::uuid uuid = node->get_uuid();
	// DEBUG: UUID 必须有效
	ERR_FAIL_COND_MSG(uuid.is_nil(), "StreamObjectNode without a valid UUID cannot be added.");

	// 设置父 UUID（从节点读取，已经是序列化的结果）
	ObjectData data;
	data.parent_uuid = node->get_parent_uuid();
	data.node_root = node->get_instance_id();

	registry_[uuid] = data;

	// 建立父子映射（基于 parent_uuid）
	if (!data.parent_uuid.is_nil())
		children_map_[data.parent_uuid].insert(uuid);

	to_upsert_uuids_.insert(uuid);
	dirty_aabb_.insert(uuid);

	_connect_node_signals(node);
}

void godot::StreamManager::remove_object(const uuids::uuid &uuid) {
	// DEBUG: 不能移除无效的 UUID
	ERR_FAIL_COND_MSG(uuid.is_nil(), "Attempting to remove object with nil UUID.");
	object_removal_.insert(uuid);
}

// 对象移除但节点需手动删除
void StreamManager::_remove_object(const uuids::uuid &uuid) {
	// DEBUG: 内部调用，确保 UUID 存在于注册表
	DEV_ASSERT(registry_.count(uuid) > 0);

	// 收集所有要删除的 UUID（自身 + 全部子孙）
	a_hashset<uuids::uuid> to_delete = _collect_descendants(uuid);

	// 遍历处理每个对象（顺序无关紧要）
	for (const auto &id : to_delete) {
		auto it = registry_.find(id);
		if (it == registry_.end())
			continue;

		// 删除磁盘上的场景文件
		_delete_object_scene(id);
		cache_.remove_scene(id); // 从资源缓存移除

		// 清理父子关系（仅从父记录中擦除自己）
		if (!it->second.parent_uuid.is_nil())
			children_map_[it->second.parent_uuid].erase(id);

		// 从注册表移除
		registry_.erase(it);

		// 标记数据库删除（下帧批量同步）
		to_remove_.insert(id);
		_erase_registry_entry(id);
	}

	// 清除这些节点的 children_map 条目（它们不可能再作为父节点存在）
	for (const auto &id : to_delete)
		children_map_.erase(id);
}

void StreamManager::update_object(StreamObjectNode *node) {
	// DEBUG: node 不能为空
	ERR_FAIL_COND(!node);

	uuids::uuid uuid = node->get_uuid();
	// DEBUG: uuid 必须已在注册表中
	ERR_FAIL_COND_MSG(!registry_.count(uuid), "update_object called for an unregistered UUID.");

	// 更新 AABB 和 node_root（以防节点重新创建）
	registry_[uuid].node_root = node->get_instance_id();

	// parent_uuid 可能变化，处理父子关系迁移
	uuids::uuid new_parent = node->get_parent_uuid();
	ObjectData &data = registry_[uuid];
	if (new_parent != data.parent_uuid) {
		if (!data.parent_uuid.is_nil())
			children_map_[data.parent_uuid].erase(uuid);
		if (!new_parent.is_nil())
			children_map_[new_parent].insert(uuid);
		data.parent_uuid = new_parent;
	}

	to_upsert_uuids_.insert(uuid);
}

a_hashset<uuids::uuid> StreamManager::_collect_descendants(const uuids::uuid &root) const {
	a_hashset<uuids::uuid> result;
	std::vector<uuids::uuid> stack{ root };
	while (!stack.empty()) {
		uuids::uuid current = stack.back();
		stack.pop_back();
		if (result.insert(current).second) { // 首次插入成功才继续展开子节点（防环）
			auto it = children_map_.find(current);
			if (it != children_map_.end()) {
				for (const auto &child : it->second)
					stack.push_back(child);
			}
		}
	}
	return result;
}

void godot::StreamManager::_query_aabb(std::vector<AABB> &aabbs) {
	// DEBUG: 查询列表不应为空
	DEV_ASSERT(!aabbs.empty());
	auto result_ptr = std::make_shared<a_hashmap<uuids::uuid, ObjectData>>();
	db_worker_->push_task({ [aabbs, result_ptr](StreamSqliteDB &db) {
							   // 将查询结果赋值给 shared_ptr 指向的 map
							   *result_ptr = db.query_objects(aabbs);
						   },
			[this, result_ptr]() {
				_on_query_result(*result_ptr);
			} });
}

uuids::uuid godot::StreamManager::_generate_uuid() {
	static thread_local std::random_device rd;
	static thread_local std::mt19937 gen(rd());
	static thread_local uuids::uuid_random_generator uuid_gen(gen);
	return uuid_gen();
}

void StreamManager::_connect_node_signals(StreamObjectNode *node) {
	// DEBUG: 节点必须有效
	ERR_FAIL_COND(!node);
	if (!node->is_connected("object_aabb_changed", callable_mp(this, &StreamManager::_on_object_aabb_changed)))
		node->connect("object_aabb_changed", callable_mp(this, &StreamManager::_on_object_aabb_changed), CONNECT_APPEND_SOURCE_OBJECT);
}

String StreamManager::_derive_object_dir(const String &db_path) const {
	String base = db_path.get_base_dir();
	String name = "." + db_path.get_file().get_basename();
	return base.path_join(name) + "/";
}

// 内部槽：节点进入树
void StreamManager::_on_object_entered(Node *node) {
	// DEBUG: node 不能为空
	ERR_FAIL_COND(!node);

	// 过滤节点
	auto obj = Object::cast_to<StreamObjectNode>(node);
	if (!obj)
		return; // 非目标类型，静默忽略

	uuids::uuid uuid = obj->get_uuid();
	if (uuid.is_nil()) {
		uuid = _generate_uuid();
		obj->set_uuid(uuid);
		add_object(obj);
		return;
	}

	// 否则只需更新引用
	registry_[uuid].node_root = node->get_instance_id();
	_connect_node_signals(obj);
}

// 槽函数
void StreamManager::_on_object_exited(Node *node) {
	ERR_FAIL_COND(!node);

	auto obj = Object::cast_to<StreamObjectNode>(node);
	if (!obj)
		return;
	uuids::uuid uuid = obj->get_uuid();

	if (pending_removal_.count(uuid) || node->is_queued_for_deletion() || is_queued_for_deletion()) {
		_save_object_to_file(uuid, node);
		return;
	}

	// 否则是用户手动删除，执行完整移除逻辑
	remove_object(uuid);
}

void godot::StreamManager::_on_load_probe(StreamWorldProbe *probe) {
	ERR_FAIL_COND(!probe);
	registered_probes_.insert(probe->get_instance_id());
}

void godot::StreamManager::_on_unload_probe(StreamWorldProbe *probe) {
	ERR_FAIL_COND(!probe);
	registered_probes_.erase(probe->get_instance_id());
}

void StreamManager::_on_object_aabb_changed(StreamObjectNode *node) {
	ERR_FAIL_COND(!node);
	uuids::uuid uuid = node->get_uuid();
	if (registry_.count(uuid)) {
		dirty_aabb_.insert(uuid); // 延迟至 _process 更新
	} else {
		// DEBUG: 收到未注册节点的信号，可能是逻辑错误
		WARN_PRINT("Received AABB changed signal for unregistered node: " + String(uuids::to_string(uuid).c_str()));
	}
}

// 异步查询结果处理
void StreamManager::_on_query_result(const a_hashmap<uuids::uuid, ObjectData> &db_objects) {
	// 构建新数据集的 UUID 集合
	a_hashset<uuids::uuid> new_set;
	for (const auto &pair : db_objects) {
		new_set.insert(pair.first);
	}

	// 卸载不再需要的对象
	std::vector<uuids::uuid> to_unload;
	for (auto &pair : registry_) {
		if (new_set.count(pair.first) == 0 && pair.second.node_root.is_valid()) {
			to_unload.push_back(pair.first);
		}
	}
	for (const auto &uuid : to_unload) {
		_unload_object(uuid); // 会保存场景并缓存
		_erase_registry_entry(uuid);
	}

	// 合并数据库最新数据到注册表（保留 node_root 等运行时状态）
	for (const auto &pair : db_objects) {
		const uuids::uuid &uuid = pair.first;
		const ObjectData &db_data = pair.second;

		if (registry_.count(uuid)) {
			// 已存在，更新静态数据，保持运行时状态
			ObjectData &local = registry_[uuid];
			local.parent_uuid = db_data.parent_uuid;
		} else {
			registry_[uuid] = db_data;
		}
	}

	// 重建 children_map
	children_map_.clear();
	for (auto &pair : registry_) {
		const uuids::uuid &parent = pair.second.parent_uuid;
		if (!parent.is_nil())
			children_map_[parent].insert(pair.first);
	}

	// 标记需要加载的根对象（父为空且未加载）
	for (const auto &pair : registry_) {
		if (pair.second.parent_uuid.is_nil() && !pair.second.node_root.is_valid()) {
			load_queue_.push(pair.first);
		}
	}
}

void StreamManager::_load_object_scene(const uuids::uuid &uuid) {
	// DEBUG: 加载前确保 UUID 有效且已注册
	ERR_FAIL_COND(uuid.is_nil());
	if (!registry_.count(uuid)) {
		WARN_PRINT("_load_object_scene called for unknown UUID: " + String(uuids::to_string(uuid).c_str()));
		return;
	}

	ObjectData &data = registry_[uuid];
	if (data.node_root.is_valid())
		return; // 已加载

	String scene_path = _object_scene_path(uuid);
	// DEBUG: 路径必须非空
	ERR_FAIL_COND_MSG(scene_path.is_empty(), "Scene path is empty for UUID: " + String(uuids::to_string(uuid).c_str()));

	Node *node = cache_.acquire(uuid, scene_path);

	if (!node) {
		// 资源尚未缓存：发起异步加载请求，稍后再试
		if (cache_.request_scene(uuid, scene_path)) {
			loaded_queue_.push(uuid); // 重新入队等待加载完成
		}
		return;
	}

	// 成功获取实例
	StreamObjectNode *stream_node = Object::cast_to<StreamObjectNode>(node);
	if (!stream_node) {
		// DEBUG: 获取到的节点不是预期类型，属于严重错误
		ERR_PRINT("Loaded scene is not a StreamObjectNode: " + scene_path);
		memdelete(node);
		return;
	}

	// 设置所有者并挂载
	add_child(stream_node, true);
	stream_node->set_owner(get_owner() ? get_owner() : get_parent());
	notify_property_list_changed();

	_connect_node_signals(stream_node);
}

void StreamManager::_unload_object(const uuids::uuid &uuid) {
	// DEBUG: UUID 必须有效且已注册
	ERR_FAIL_COND(uuid.is_nil());
	if (!registry_.count(uuid)) {
		WARN_PRINT("_unload_object called for unknown UUID: " + String(uuids::to_string(uuid).c_str()));
		return;
	}

	// 收集所有子孙 UUID（包括自身），统一插入 pending_removal_
	a_hashset<uuids::uuid> all_ids = _collect_descendants(uuid);
	for (const auto &id : all_ids) {
		pending_removal_.insert(id);
	}

	for (auto obj_id : all_ids) {
		// 处理自身节点
		ObjectData &data = registry_[obj_id];
		if (data.node_root.is_valid()) {
			StreamObjectNode *obj_node = Object::cast_to<StreamObjectNode>(
					ObjectDB::get_instance(data.node_root));
			if (obj_node) {
				_save_object_to_file(obj_id, obj_node);
				remove_child(obj_node);
				obj_node->set_owner(nullptr);
				cache_.release(obj_id, obj_node);
				registry_[obj_id].node_root = ObjectID(); // 清空所有已卸载的 node_root
			} else {
				// DEBUG: 注册表中的 node_root 指向了无效对象
				WARN_PRINT("Object node for UUID " + String(uuids::to_string(obj_id).c_str()) + " is no longer valid during unload.");
			}
		}
	}

	// 移除 pending 标记
	for (const auto &id : all_ids)
		pending_removal_.erase(id);
}

void StreamManager::_save_object_to_file(const uuids::uuid &uuid, Node *node) {
	// DEBUG: 节点不能为空
	ERR_FAIL_COND(!node);

	// 打包整个 node 树
	Ref<PackedScene> scene;
	scene.instantiate();
	// DEBUG: PackedScene 实例化必须成功
	ERR_FAIL_COND(!scene.is_valid());

	scene->pack(node);

	String path = _object_scene_path(uuid);
	ERR_FAIL_COND_MSG(path.is_empty(), "Cannot save object, derived path is empty for UUID: " + String(uuids::to_string(uuid).c_str()));

	// FIXME 涉及 i/o 可能会引起主线程卡顿
	WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &StreamManager::_async_save_object).bind(scene, path));
}

void godot::StreamManager::_async_save_object(const Ref<godot::PackedScene> scene, String path) {
	// DEBUG: 场景资源必须有效
	ERR_FAIL_COND_MSG(!scene.is_valid(), "Cannot save invalid PackedScene to " + path);

	Error err = ResourceSaver::get_singleton()->save(scene, path, ResourceSaver::FLAG_COMPRESS);

	if (err != OK) {
		ERR_PRINT("Failed to save scene to " + path);
	}
}

void StreamManager::_erase_registry_entry(const uuids::uuid &id) {
	registry_.erase(id);
	dirty_aabb_.erase(id); // ← 新增
	to_upsert_uuids_.erase(id); // 原有逻辑，移至此处统一管理
}

void StreamManager::_delete_object_scene(const uuids::uuid &uuid) {
	String path = _object_scene_path(uuid);
	if (FileAccess::file_exists(path)) {
		DirAccess::remove_absolute(path);
	}
	cache_.remove_scene(uuid);
}

String StreamManager::_object_scene_path(const uuids::uuid &uuid) const {
	return object_scene_dir_ + String(uuids::to_string(uuid).c_str()) + ".tscn";
}

// 数据库同步
void StreamManager::_flush_pending_db_ops() {
	if (!db_worker_.is_valid())
		return;
	if (to_upsert_uuids_.empty() && to_remove_.empty() && dirty_aabb_.empty())
		return;

	// 正常 upsert：在主线程捕获 ObjectData（避免 DB 线程访问 registry_）
	auto upsert_data = std::make_shared<std::vector<std::pair<uuids::uuid, ObjectData>>>();
	for (const auto &uuid : to_upsert_uuids_) {
		auto it = registry_.find(uuid);
		if (it != registry_.end())
			upsert_data->emplace_back(uuid, it->second);
		else
			// DEBUG: 标记为 upsert 的 UUID 在注册表中不存在，数据不一致
			WARN_PRINT("UUID in to_upsert_uuids_ not found in registry: " + String(uuids::to_string(uuid).c_str()));
	}
	to_upsert_uuids_.clear();

	// 交换remove容器
	auto remove = std::make_shared<a_hashset<uuids::uuid>>();
	remove->swap(to_remove_);

	// 脏 AABB：在主线程捕获 AABB 快照
	auto dirty_data = std::make_shared<a_hashmap<uuids::uuid, godot::AABB>>();
	for (const auto &uuid : dirty_aabb_) {
		auto it = registry_.find(uuid);
		if (it == registry_.end()) {
			WARN_PRINT("Dirty AABB UUID not found in registry: " + String(uuids::to_string(uuid).c_str()));
			continue;
		}
		const ObjectID &node_id = it->second.node_root;
		if (node_id.is_valid()) {
			StreamObjectNode *node = Object::cast_to<StreamObjectNode>(
					ObjectDB::get_instance(node_id));
			if (node && node->is_inside_tree())
				(*dirty_data)[uuid] = node->get_aabb();
			else
				// DEBUG: 节点无效或不在树中，无法获取 AABB
				WARN_PRINT("Cannot retrieve AABB for UUID " + String(uuids::to_string(uuid).c_str()) + ": node invalid or not in tree.");
		}
	}
	dirty_aabb_.clear();

	if (upsert_data->empty() && remove->empty() && dirty_data->empty())
		return;

	auto dirty_result = std::make_shared<a_hashmap<uuids::uuid, int>>();

	db_worker_->push_task({ [upsert_data, remove, dirty_data, dirty_result](StreamSqliteDB &db) {
							   db.db.exec("BEGIN TRANSACTION;");

							   // 删除
							   for (const auto &uuid : *remove)
								   db.remove_object(uuid);

							   // 正常 upsert uuid
							   for (const auto &[uuid, data] : *upsert_data)
								   db.upsert_object_uuids(uuid, data);

							   // 批量更新所有脏 AABB
							   for (const auto &[uuid, aabb] : *dirty_data)
								   db.set_object_aabb(uuid, aabb);

							   // 基于已更新 AABB 计算聚合包围盒并生成 chunk
							   for (const auto &[uuid, aabb] : *dirty_data) {
								   // 聚合自身与所有子对象的 AABB
								   godot::AABB agg_aabb = aabb;
								   for (const auto &[child, ca] : db.query_children_aabb(uuid)) {
									   agg_aabb = agg_aabb.merge(ca);
								   }

								   Chunk chunk = Chunk::compute_chunk(agg_aabb);
								   int new_chunk_id = db.query_chunk(chunk);
								   db.set_object_chunk(uuid, new_chunk_id);
								   (*dirty_result)[uuid] = new_chunk_id;

								   // TODO 不去获取整个字段，精确到parent_uuid
								   ObjectData obj_data = db.query_object(uuid);

								   // 沿父链向上更新祖先 chunk_id
								   uuids::uuid parent = obj_data.parent_uuid;
								   while (!parent.is_nil()) {
									   ObjectData parent_data = db.query_object(parent);

									   // 聚合祖先的 AABB（自身 + 所有子对象）
									   godot::AABB parent_agg = db.get_object_aabb(parent);
									   for (const auto &[child, ca] : db.query_children_aabb(parent)) {
										   parent_agg = parent_agg.merge(ca);
									   }

									   Chunk parent_chunk = Chunk::compute_chunk(parent_agg);
									   int parent_chunk_id = db.query_chunk(parent_chunk);

									   db.set_object_chunk(parent, parent_chunk_id);
									   (*dirty_result)[parent] = parent_chunk_id;

									   parent = parent_data.parent_uuid;
								   }
							   }
							   db.db.exec("COMMIT;");
						   },
			[]() {
			} });
}

// 属性绑定
void StreamManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_database_path", "path"), &StreamManager::set_database_path);
	ClassDB::bind_method(D_METHOD("get_database_path"), &StreamManager::get_database_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "database_path", PROPERTY_HINT_FILE, "*.db"), "set_database_path", "get_database_path");

	// 内部槽注册（供信号连接使用）
	ClassDB::bind_method(D_METHOD("_on_object_entered"), &StreamManager::_on_object_entered);
	ClassDB::bind_method(D_METHOD("_on_object_exited"), &StreamManager::_on_object_exited);
	ClassDB::bind_method(D_METHOD("_on_object_aabb_changed"), &StreamManager::_on_object_aabb_changed);
	ClassDB::bind_method(D_METHOD("_on_load_probe"), &StreamManager::_on_load_probe);
	ClassDB::bind_method(D_METHOD("_on_unload_probe"), &StreamManager::_on_unload_probe);

	ClassDB::bind_method(D_METHOD("_async_save_object"), &StreamManager::_async_save_object);
}
