#include "stream_manager.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

// 构造 析构
StreamManager::StreamManager() : cache_(16, 32) {
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
}

void StreamManager::_ready() {
	if (!database_path_.is_empty()) {
		_init_database(database_path_);
	}
}

void StreamManager::_process(double delta) {
	// 异步数据库回调执行
	if (db_worker_.is_valid()) {
		// 批量数据库同步（含脏 AABB 处理）
		_flush_pending_db_ops();

		// 执行上一帧的任务回调
		db_worker_->flush_callbacks();

		// 执行查询
		query_process++;
		if (query_process >= 60) {
			std::vector<AABB> query_aabbs;
			for (auto id : registered_probes_) {
				StreamWorldProbe *probe = Object::cast_to<StreamWorldProbe>(ObjectDB::get_instance(id));
				query_aabbs.push_back(probe->get_global_aabb());
			}

			_query_aabb(query_aabbs);

			query_process = 0;
		}
	};

	// 轮询场景缓存异步加载状态
	cache_.update();

	// 处理待加载队列（分批进行，避免单帧负载过高）
	const int MAX_LOADS_PER_FRAME = 4;
	int loads = 0;
	while (!load_queue_.empty() && loads < MAX_LOADS_PER_FRAME) {
		UtilityFunctions::print("[StreamManager] Loading scene: ", uuids::to_string(load_queue_.front()).c_str());

		uuids::uuid uuid = load_queue_.front();
		load_queue_.pop();
		_load_object_scene(uuid);

		++loads;
	}
}

// 数据库初始化
void StreamManager::_init_database(const String &path) {
	object_scene_dir_ = _derive_object_dir(path);

	// 确保目录存在
	if (!DirAccess::dir_exists_absolute(object_scene_dir_)) {
		Error err = DirAccess::make_dir_absolute(object_scene_dir_);
		if (err != OK) {
			ERR_PRINT("Failed to create object scene directory: " + object_scene_dir_);
		}
	}

	// 创建异步数据库 worker（独立线程）
	db_worker_ = Ref<AsyncDbWorker>(memnew(AsyncDbWorker(path)));
	db_worker_->push_task({
			[](StreamSqliteDB &db) {
				// 可选：执行启动时的数据库维护或读取版本信息
			},
			[]() {} // 无需回调
	});

	// 从数据库加载已持久化对象列表（同步，因为刚启动，轻量操作）
	// 可通过 db_worker_ 提交同步任务或直接在构造函数中读取
	// 此处略，实际可在 worker 创建后提交一个查询任务，在回调中填充 registry_
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
	if (!node)
		return;
	uuids::uuid uuid = node->get_uuid();
	if (uuid.is_nil()) {
		// 理论上不应发生
		WARN_PRINT("StreamObjectNode without valid UUID cannot be added.");
		return;
	}

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

// 对象移除但节点需手动删除
void StreamManager::remove_object(const uuids::uuid &uuid) {
	UtilityFunctions::print("[StreamManager] remove_object: ", uuids::to_string(uuid).c_str());
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
		to_upsert_uuids_.erase(id);
	}

	// 清除这些节点的 children_map 条目（它们不可能再作为父节点存在）
	for (const auto &id : to_delete)
		children_map_.erase(id);
}

void StreamManager::update_object(StreamObjectNode *node) {
	uuids::uuid uuid = node->get_uuid();
	if (!registry_.count(uuid))
		return;

	UtilityFunctions::print("[StreamManager] update_object: ", uuids::to_string(uuid).c_str());

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
	auto result_ptr = std::make_shared<a_hashmap<uuids::uuid, ObjectData>>();
	db_worker_->push_task({ [aabbs, result_ptr](StreamSqliteDB &db) {
							   // 将查询结果赋值给 shared_ptr 指向的 map
							   *result_ptr = db.query_objects(aabbs);
							   UtilityFunctions::print("[StreamManager] query_aabb: ", aabbs.size(), " find: ", result_ptr->size(), " objects");
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
	// 过滤节点
	if (!node->is_class("StreamObjectNode"))
		return;

	auto obj = Object::cast_to<StreamObjectNode>(node);
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
	if (!node->is_class("StreamObjectNode"))
		return;

	auto obj = Object::cast_to<StreamObjectNode>(node);

	uuids::uuid uuid = obj->get_uuid();
	// TODO 有概率绕过
	if (pending_removal_.count(uuid) || node->is_queued_for_deletion() || is_queued_for_deletion()) {
		return;
	}
	// 否则是用户手动删除，执行完整移除逻辑
	remove_object(uuid);
}

void godot::StreamManager::_on_load_probe(StreamWorldProbe *probe) {
	registered_probes_.insert(probe->get_instance_id());
}

void godot::StreamManager::_on_unload_probe(StreamWorldProbe *probe) {
	registered_probes_.erase(probe->get_instance_id());
}

void StreamManager::_on_object_aabb_changed(StreamObjectNode *node) {
	uuids::uuid uuid = node->get_uuid();
	if (registry_.count(uuid)) {
		UtilityFunctions::print("[StreamManager] object_aabb_changed: ", uuids::to_string(uuid).c_str());
		dirty_aabb_.insert(uuid); // 延迟至 _process 更新
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
		UtilityFunctions::print("[StreamManager] Unload object: ", uuids::to_string(uuid).c_str());
		_unload_object(uuid); // 会保存场景并缓存
		registry_.erase(uuid);
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
	if (!registry_.count(uuid))
		return;
	ObjectData &data = registry_[uuid];
	if (data.node_root.is_valid())
		return;

	String scene_path = _object_scene_path(uuid);
	Node *node = cache_.acquire(uuid, scene_path);

	// TODO 场景文件损坏会出现问题
	if (!node) {
		// 资源尚未缓存：发起异步加载请求，稍后再试
		if (cache_.request_scene(uuid, scene_path))
			load_queue_.push(uuid); // 重新入队等待加载完成
		return;
	}

	// 成功获取实例
	StreamObjectNode *stream_node = Object::cast_to<StreamObjectNode>(node);
	if (!stream_node) {
		// 不是期望的节点类型，清理
		memdelete(node);
		return;
	}

	// 设置所有者并挂载
	add_child(node, true);
	node->set_owner(get_owner());

	_connect_node_signals(stream_node);

	UtilityFunctions::print("[StreamManager] load object successful: ", uuids::to_string(uuid).c_str());
}

void StreamManager::_unload_object(const uuids::uuid &uuid) {
	if (!registry_.count(uuid))
		return;

	// 收集所有子孙 UUID（包括自身），统一插入 pending_removal_
	a_hashset<uuids::uuid> all_ids = _collect_descendants(uuid);
	for (const auto &id : all_ids) {
		pending_removal_.insert(id);
	}

	// TODO 应该有更好的方法实现
	// 递归卸载子对象（深度优先，保存并缓存子节点）
	if (children_map_.count(uuid)) {
		auto children = children_map_[uuid]; // 拷贝，避免迭代中修改
		for (const auto &child : children)
			_unload_object(child);
	}

	// 处理自身节点
	ObjectData &data = registry_[uuid];
	if (data.node_root.is_valid()) {
		StreamObjectNode *obj_node = Object::cast_to<StreamObjectNode>(
				ObjectDB::get_instance(data.node_root));
		if (obj_node) {
			_save_object_to_file(uuid, obj_node);
			remove_child(obj_node);
			cache_.release(uuid, obj_node);
		}
	}

	// 清空所有已卸载的 node_root（统一操作，避免遗漏）
	for (const auto &id : all_ids) {
		if (registry_.count(id))
			registry_[id].node_root = ObjectID();
	}

	// 移除 pending 标记
	for (const auto &id : all_ids)
		pending_removal_.erase(id);
}

void StreamManager::_save_object_to_file(const uuids::uuid &uuid, Node *node) {
	UtilityFunctions::print("[StreamManager] save_object: ", uuids::to_string(uuid).c_str());

	// 打包整个 node 树
	Ref<PackedScene> scene;
	scene.instantiate();

	scene->pack(node);

	String path = _object_scene_path(uuid);
	Error err = ResourceSaver::get_singleton()->save(scene, path, ResourceSaver::FLAG_COMPRESS);
	if (err != OK) {
		ERR_PRINT("Failed to save scene to " + path);
	}
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
	}
	to_upsert_uuids_.clear();

	// 交换remove容器
	auto remove = std::make_shared<a_hashset<uuids::uuid>>();
	remove->swap(to_remove_);

	// 脏 AABB：在主线程捕获 AABB 快照
	auto dirty_data = std::make_shared<a_hashmap<uuids::uuid, godot::AABB>>();
	for (const auto &uuid : dirty_aabb_) {
		auto it = registry_.find(uuid);
		if (it == registry_.end())
			continue;
		const ObjectID &node_id = it->second.node_root;
		if (node_id.is_valid()) {
			StreamObjectNode *node = Object::cast_to<StreamObjectNode>(
					ObjectDB::get_instance(node_id));
			if (node && node->is_inside_tree())
				(*dirty_data)[uuid] = node->get_aabb();
		}
	}
	dirty_aabb_.clear();

	if (upsert_data->empty() && remove->empty() && dirty_data->empty())
		return;

	auto dirty_result = std::make_shared<a_hashmap<uuids::uuid, int>>();

	db_worker_->push_task({ [upsert_data, remove, dirty_data, dirty_result](StreamSqliteDB &db) {
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
									   if (!(ca.size.x == 0 && ca.size.y == 0 && ca.size.z == 0 && ca.position.x == 0 && ca.position.y == 0 && ca.position.z == 0))
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
										   if (!(ca.size.x == 0 && ca.size.y == 0 && ca.size.z == 0 && ca.position.x == 0 && ca.position.y == 0 && ca.position.z == 0))
											   parent_agg = parent_agg.merge(ca);
									   }

									   // 父对象 AABB 从未写入（所有字段 NULL），跳过整个父链
									   if (parent_agg.size.x == 0 && parent_agg.size.y == 0 && parent_agg.size.z == 0 && parent_agg.position.x == 0 && parent_agg.position.y == 0 && parent_agg.position.z == 0)
										   break;
									   Chunk parent_chunk = Chunk::compute_chunk(parent_agg);
									   int parent_chunk_id = db.query_chunk(parent_chunk);

									   db.set_object_chunk(parent, parent_chunk_id);
									   (*dirty_result)[parent] = parent_chunk_id;

									   parent = parent_data.parent_uuid;
								   }
							   }
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
}