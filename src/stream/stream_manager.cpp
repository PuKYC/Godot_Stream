#include "stream_manager.h"
#include "ulid/ulid_generator.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

Ref<PackedScene> pack_filtered(Node *p_root, const TypedArray<NodePath> &p_exclude) {
	// 1. 先整棵树 duplicate
	Node *clone = Object::cast_to<Node>(p_root->duplicate());

	// 2. 收集要删除的节点（先收集，不能边遍历边删）
	Vector<Node *> to_remove;
	for (int i = 0; i < p_exclude.size(); i++) {
		Node *target = clone->get_node_or_null(p_exclude[i]);
		if (target)
			to_remove.push_back(target);
	}

	// 3. 从树上摘除（注意要先从父节点 remove，再 queue_free）
	for (Node *node : to_remove) {
		if (node->get_parent())
			node->get_parent()->remove_child(node);
		memdelete(node);
	}

	// 4. 打包
	Ref<PackedScene> packed;
	packed.instantiate();
	packed->pack(clone);

	// 5. 清理临时克隆
	memdelete(clone);

	return packed;
}

NodePath get_parent_node_path(const NodePath &path) {
	String s = String(path);
	int idx = s.rfind("/");
	if (idx <= 0) {
		return NodePath(".");
	}
	return NodePath(s.substr(0, idx));
}

// ════════════════════════════════════════════════════════════════════════════
//  StreamManager 小函数
// ════════════════════════════════════════════════════════════════════════════

// 构造 object 场景文件完整路径
String StreamManager::object_scene_path(const ulid::ULID &ulid) const {
	return object_dir + String(ulid::Marshal(ulid).c_str()) + scene_file;
}

// 执行一次带事务的批量写入
void StreamManager::flush_pending_db_ops() {
	db->db.exec("BEGIN TRANSACTION;");
	remove_object_list(remove_object_set);
	upsert_object_list(upsert_object_set);
	db->db.exec("COMMIT;");
}

// 标记节点为流式移除并释放父节点
void StreamManager::free_stream_node(StreamObject *node) {
	node->stream_remove();
	auto node_parent  = node->get_parent();
	node_parent->get_parent()->remove_child(node_parent);
	node_parent->queue_free();
}

// ════════════════════════════════════════════════════════════════════════════
//  StreamSqliteDB 小函数
// ════════════════════════════════════════════════════════════════════════════

// reset + clear_bindings 合并，避免每次都写两行
void StreamManager::StreamSqliteDB::stmt_finish(SQLiteDB::Stmt &stmt) {
	stmt.reset();
	stmt.clear_bindings();
}

// 将 chunk 的四个字段绑定到语句的 ?1~?4
void StreamManager::StreamSqliteDB::bind_chunk(SQLiteDB::Stmt &stmt, const Chunk &chunk) {
	stmt.bind_int(1, chunk.level);
	stmt.bind_int(2, chunk.x);
	stmt.bind_int(3, chunk.y);
	stmt.bind_int(4, chunk.z);
}

// 从语句中读取可空的 ULID blob 字段，null 时返回 ULID(0)
ulid::ULID StreamManager::StreamSqliteDB::read_nullable_ulid(SQLiteDB::Stmt &stmt, int col) {
	if (stmt.is_null(col)) {
		return ulid::ULID(0);
	}
	const void *blob = stmt.get_blob(col);
	return blob ? *static_cast<const ulid::ULID *>(blob) : ulid::ULID(0);
}

// ════════════════════════════════════════════════════════════════════════════
//  StreamManager 主体
// ════════════════════════════════════════════════════════════════════════════
void StreamManager::_ready() {
	if (!database_path.is_empty()) {
		connect_database(database_path);
	}
}

void StreamManager::_process(double delta) {
	update_database();
	load_object_list();
}

void StreamManager::connect_database(String path) {
	disconnect_database();

	db = std::make_unique<StreamSqliteDB>(path.utf8().ptr());

	// 创建同名的.文件夹存放object的packedsence场景文件
	String base_dir = path.get_base_dir();
	String name = "." + path.get_file().get_basename();
	object_dir = base_dir.path_join(name) + "/";

	// 创建文件夹（如果不存在）
	if (!DirAccess::dir_exists_absolute(object_dir)) {
		Error err = DirAccess::make_dir_absolute(object_dir);
		if (err != OK) {
			ERR_PRINT("Failed to create directory: " + object_dir);
		}
	}
}

void StreamManager::set_objects(const PackedStringArray objects) {
	for (auto object : objects) {
		ulid::ULID ulid = ulid::Unmarshal(object.utf8().get_data());
		Object c = db->query_object(ulid);
		if (!(c == Object())) {
			ulid_object_table[ulid] = c;

			if (c.parent != ulid::ULID(0)) {
				object_child_table[c.parent].insert(ulid);
			}
		};
	}
}

PackedStringArray StreamManager::get_objects() {
	PackedStringArray object_list;
	for (auto objects : ulid_object_table) {
		object_list.append(ulid::Marshal(objects.first).c_str());
	}
	return object_list;
}

void StreamManager::set_database_path(String path) {
	database_path = path;
	connect_database(path);
}

String StreamManager::get_database_path() {
	return database_path;
}

void StreamManager::disconnect_database() {
	// 一个object下可能还有子object，两个循环不能合并！！！！

	// 第一轮：先全部保存
	for (auto &[key, variant] : ulid_object_table) {
		if (has_node(variant.path)) {
			save_object(*get_node<StreamObject>(variant.path), key);
		}
	}

	// 第二轮：再全部释放
	for (auto &[key, variant] : ulid_object_table) {
		if (has_node(variant.path)) {
			free_stream_node(get_node<StreamObject>(variant.path));
		}
	}

	if (db) {
		flush_pending_db_ops();
		db.reset();
	}

	load_queue = std::queue<ulid::ULID>();
	loading_queue = std::queue<ulid::ULID>();

	object_dir = "";
	ulid_object_table.clear();
	object_child_table.clear();
}

void StreamManager::aabb_query(AABB aabb) {
	DEBUG_CODE(UtilityFunctions::print("[StreamManager] aabb_query start: ", aabb););

	update_database();

	// 查询数据库（主线程阻塞，但可考虑异步）
	auto new_objects = db->query_objects(aabb);
	DEBUG_CODE(UtilityFunctions::print("[StreamManager] query_objects returned ", new_objects.size(), " objects"););

	ankerl::unordered_dense::map<ulid::ULID, ankerl::unordered_dense::set<ulid::ULID>> new_child_map;

	// 3. 构建新子关系 map，并标记需要加载的根对象
	{
		int root_count = 0, child_count = 0;
		for (auto &[key, variant] : new_objects) {
			if (ulid_object_table.find(key) != ulid_object_table.end()) {
				continue;
			}
			if (variant.parent == ulid::ULID(0)) {
				load_queue.push(key);
				root_count++;
			} else {
				new_child_map[variant.parent].insert(key);
				child_count++;
			}
		}
		DEBUG_CODE(
				UtilityFunctions::print("[StreamManager] New objects to load: roots=", root_count, ", children=", child_count););
	}

	// 4. 卸载不在新查询结果中的对象
	{
		// 在移动 ulid_object_table 之前，把持久对象的子关系迁移到 new_child_map，
		// 否则替换后这些关系会全部丢失
		for (auto &[key, children] : object_child_table) {
			if (new_objects.find(key) == new_objects.end())
				continue; // 这个父对象本次会被卸载，不需要保留
			for (auto &child : children) {
				if (new_objects.find(child) != new_objects.end()) {
					new_child_map[key].insert(child);
				}
			}
		}

		auto old_table = std::move(ulid_object_table);
		int unload_count = 0;
		for (auto &[key, variant] : old_table) {
			if (new_objects.find(key) != new_objects.end()) {
				continue;
			}
			if (has_node(variant.path)) {
				auto *node = get_node<StreamObject>(variant.path);
				if (node) {
					save_object(*node, key);
				}
			}
		}

		for (auto &[key, variant] : old_table) {
			if (new_objects.find(key) != new_objects.end()) {
				continue;
			}
			if (has_node(variant.path)) {
				auto *node = get_node<StreamObject>(variant.path);
				if (node) {
					free_stream_node(node);
					DEBUG_CODE(
							UtilityFunctions::print("[StreamManager] Unloading object: ", variant.path, " (", ulid::Marshal(key).c_str(), ")"););
					unload_count++;
				}
			}
		}

		DEBUG_CODE(UtilityFunctions::print("[StreamManager] Unloaded objects count: ", unload_count););

		ulid_object_table = std::move(new_objects);
		object_child_table = std::move(new_child_map);
	}

	DEBUG_CODE(UtilityFunctions::print("[StreamManager] aabb_query finished. Total objects in table: ", ulid_object_table.size()););
}

bool StreamManager::add_object(StreamObject *object) {
	if (object->get_parent()->get_scene_file_path().is_empty()) {
		DEBUG_CODE("Object must be added to a scene before being added to the stream manager");
		return false;
	}

	auto ulid = UlidGenerator::thread_local_instance().next_raw();

	Chunk chunk = compute_chunk_from_aabb(object->get_object_aabb());

	auto id = object->parent_ulid;

	object->set_ulid(ulid::Marshal(ulid).c_str());

	if (!save_object(*object, ulid)) {
		return false;
	}

	if (id != 0)
		object_child_table[id].insert(ulid);

	ulid_object_table[ulid] = { get_path_to(object), id, db->query_chunk(chunk) };

	upsert_object_set.insert(ulid);

	auto parent_path = get_parent_node_path(get_parent_node_path(get_path_to(object)));
	DEBUG_CODE({
		UtilityFunctions::print("object to parent: ", parent_path);
	});

	free_stream_node(object);

	Ref<PackedScene> scene = ResourceLoader::get_singleton()->load(object_scene_path(ulid));
	auto node = scene->instantiate();
	get_node<Node3D>(parent_path)->add_child(node, true);
	node->set_owner(get_owner());

	DEBUG_CODE({
		UtilityFunctions::print("Adding object: ", get_path_to(node));
		UtilityFunctions::print("ulid: ", String(ulid::Marshal(ulid).c_str()));
	});

	return true;
}

//这个删除函数仅删除stream_manger下的数据，节点需自己删除，会进行级联删除
bool StreamManager::remove_object(String ulid_str) {
	auto ulid = ulid::Unmarshal(ulid_str.utf8().get_data());

	if (!ulid_object_table.contains(ulid) or !is_inside_tree()) {
		return false;
	}

	if (!FileAccess::file_exists(object_dir + ulid_str.utf8().get_data() + scene_file)) {
		UtilityFunctions::print("File does not exist:", object_dir + ulid_str.utf8().get_data());
		return false;
	} else {
		Error err = DirAccess::remove_absolute(object_dir + ulid_str.utf8().get_data() + scene_file);

		if (err == OK) {
			UtilityFunctions::print("File deleted successfully:", object_dir + ulid_str.utf8().get_data());
		} else {
			UtilityFunctions::print("Deletion failed, error code: ", err);
		}
	}

	remove_object_set.insert(ulid);

	auto obj = ulid_object_table[ulid];
	if (obj.parent != ulid::ULID(0))
		object_child_table[obj.parent].erase(ulid);

	// 先拷贝一份，避免递归调用内部 erase 导致迭代器失效（UB）
	auto children_copy = object_child_table[ulid];
	for (auto child : children_copy) {
		remove_object(ulid::Marshal(child).c_str());
	}
	object_child_table.erase(ulid);
	ulid_object_table.erase(ulid);

	DEBUG_CODE(UtilityFunctions::print("Removing object: ", ulid_str.utf8().get_data()));

	return true;
}

bool StreamManager::update_object(StreamObject *object_node) {
	auto ulid = object_node->ulid;
	if (!ulid_object_table.contains(ulid)) {
		DEBUG_CODE(UtilityFunctions::print("Object not found: ", ulid::Marshal(ulid).c_str()););
		return false;
	}

	const auto object = ulid_object_table.at(ulid);

	Chunk chunk = compute_chunk_from_aabb(object_node->get_object_aabb());

	auto id = object_node->parent_ulid;

	Object &obj = ulid_object_table[ulid];

	if (id != obj.parent) {
		// 从旧父移除（旧父为 0 表示原本是根对象，object_child_table 中无对应 entry）
		if (obj.parent != ulid::ULID(0))
			object_child_table[obj.parent].erase(ulid);
		// 加入新父（新父为 0 表示变成根对象，无需插入任何集合）
		if (id != ulid::ULID(0))
			object_child_table[id].insert(ulid);
	}

	upsert_object_set.insert(ulid);
	ulid_object_table[ulid] = { get_path_to(object_node), id, db->query_chunk(chunk) };

	if (!save_object(*get_node<StreamObject>(get_path_to(object_node)), ulid)) {
		DEBUG_CODE(UtilityFunctions::print("Failed to object: ", ulid::Marshal(ulid).c_str()););
		return false;
	}

	DEBUG_CODE(UtilityFunctions::print("Updating object: " + object.path));

	return true;
}

void StreamManager::update_database() {
	if (!db || (remove_object_set.empty() && upsert_object_set.empty()))
		return;

	flush_pending_db_ops();
}

void StreamManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_database_path", "path"), &StreamManager::set_database_path);
	ClassDB::bind_method(D_METHOD("get_database_path"), &StreamManager::get_database_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "database_path", PROPERTY_HINT_FILE, "*.db"), "set_database_path", "get_database_path");

	ClassDB::bind_method(D_METHOD("set_objects", "objects"), &StreamManager::set_objects);
	ClassDB::bind_method(D_METHOD("get_objects"), &StreamManager::get_objects);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "Objects", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_STORAGE),
				 "set_objects", "get_objects");

	ClassDB::bind_method(D_METHOD("aabb_query", "aabb"), &StreamManager::aabb_query);
	ClassDB::bind_method(D_METHOD("add_object", "object"), &StreamManager::add_object);
	ClassDB::bind_method(D_METHOD("remove_object", "ulid_str"), &StreamManager::remove_object);
	ClassDB::bind_method(D_METHOD("update_object", "object_node"), &StreamManager::update_object);
	ClassDB::bind_method(D_METHOD("update_database"), &StreamManager::update_database);
}

void StreamManager::upsert_object_list(ankerl::unordered_dense::set<ulid::ULID> &upsert_object_set) {
	for (auto &ulid : upsert_object_set) {
		if (ulid_object_table.contains(ulid)) {
			auto &object = ulid_object_table[ulid];

			db->upsert_object(ulid, object);

			DEBUG_CODE(UtilityFunctions::print("Updating object data: ", ulid::Marshal(ulid).c_str()));
		}
	}

	upsert_object_set.clear();
}

void StreamManager::remove_object_list(ankerl::unordered_dense::set<ulid::ULID> &remove_object_set) {
	for (auto &ulid : remove_object_set) {
		db->remove_object(ulid);

		DEBUG_CODE(UtilityFunctions::print("Removing object data: ", ulid::Marshal(ulid).c_str()));
	}

	remove_object_set.clear();
}

bool StreamManager::save_object(const StreamObject &stream_object, ulid::ULID &ulid) {
	Node *parent = stream_object.get_parent();

	TypedArray<NodePath> stream_list;
	auto keys = stream_object.get_aabb_stream().keys();
	for (int i = 0; i < keys.size(); ++i) {
		const NodePath path = keys[i];
		stream_list.append(get_parent_node_path(path));
	}

	Error save_error = ResourceSaver::get_singleton()->save(
			pack_filtered(parent, stream_list),
			object_scene_path(ulid),
			ResourceSaver::FLAG_COMPRESS);

	DEBUG_CODE(
			if (save_error == OK) {
				UtilityFunctions::print("Scene saved successfully to: ", object_scene_path(ulid));
			} else {
				UtilityFunctions::print("Failed to save scene. Error: ", save_error);
			});

	return save_error == OK;
}

uint8_t StreamManager::compute_level_from_size(const AABB &aabb) {
	float m = std::max({ aabb.size.x, aabb.size.y, aabb.size.z, 0.0f });
	if (m <= 1.0f) {
		return 0;
	}

	int k = static_cast<int>(std::ceil(std::log2(m) / 2.0));

	if (k >= 0 && k < 32 && ((1ULL << (2 * k)) < static_cast<uint64_t>(m))) {
		++k;
	}

	return (k < 0 || k > 31) ? 0 : k;
}

Chunk StreamManager::compute_chunk_from_aabb(const AABB &aabb) {
	auto level = compute_level_from_size(aabb);

	int32_t x, y, z;
	// 用无符号掩码，避免 (-1 << n) 的负数左移 UB
	uint32_t mask = (2 * level < 32) ? ~((1u << (2 * level)) - 1u) : 0u;
	x = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.x)) & mask);
	y = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.y)) & mask);
	z = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(aabb.position.z)) & mask);

	return { x, y, z, level };
}

void StreamManager::load_object_list() {
	// 先检查场景是否加载完成，再从待加载队列窃取

	if (!loading_queue.empty()) {
		std::queue<ulid::ULID> new_loading_queue;
		while (loading_queue.size() != 0) {
			auto ulid = loading_queue.front();

			ResourceLoader::ThreadLoadStatus status = ResourceLoader::get_singleton()->load_threaded_get_status(object_scene_path(ulid));

			switch (status) {
				case ResourceLoader::THREAD_LOAD_LOADED: {
					Ref<PackedScene> load_res = ResourceLoader::get_singleton()->load_threaded_get(object_scene_path(ulid));
					auto it = ulid_object_table.find(ulid);
					if (load_res.is_valid() && it != ulid_object_table.end()) {
						Node *instance = load_res->instantiate();
						auto mount_path = get_parent_node_path(get_parent_node_path(it->second.path));
						get_node<Node>(mount_path)->add_child(instance);
						instance->set_owner(get_owner());
						for (auto child : object_child_table[ulid])
							load_queue.push(child);
					}
					loading_queue.pop();
				} break;
				case ResourceLoader::THREAD_LOAD_IN_PROGRESS:
					new_loading_queue.push(ulid);
					loading_queue.pop();
					break;
				default:
					UtilityFunctions::print("[StreamManager] Failed to load object: ", ulid::Marshal(ulid).c_str(), " Error: ", status);
					loading_queue.pop();
					break;
			}
		}
		loading_queue = std::move(new_loading_queue);
	}

	if (!load_queue.empty()) {
		int size = MIN(load_vector_size - loading_queue.size(), load_queue.size());
		for (int i = 0; i < size; i++) {
			auto ulid = load_queue.front();
			auto status = ResourceLoader::get_singleton()->load_threaded_get_status(object_scene_path(ulid));
			if (status != ResourceLoader::THREAD_LOAD_IN_PROGRESS && status != ResourceLoader::THREAD_LOAD_LOADED) {
				loading_queue.push(ulid);
				ResourceLoader::get_singleton()->load_threaded_request(object_scene_path(ulid));
			}
			load_queue.pop();
		}
	}
}

// ════════════════════════════════════════════════════════════════════════════
//  StreamSqliteDB
// ════════════════════════════════════════════════════════════════════════════

StreamManager::StreamSqliteDB::StreamSqliteDB(const std::string &path, bool read_only) {
	db = SQLiteDB(path, read_only);

	DEBUG_CODE(
			UtilityFunctions::print("[StreamManager] Opening database: ", db.is_connection_valid()););

	if (!is_database()) {
		DEBUG_CODE(godot::print_line("Creating new database..."));
		clear_and_create_table();

		update_info("1");
	}

	sql_upsert_chunk = SQLiteDB::Stmt(db, R"(
		INSERT INTO chunk_metadata (level, tile_x, tile_y, tile_z)
		VALUES (?1, ?2, ?3, ?4)
		ON CONFLICT(level, tile_x, tile_y, tile_z) DO NOTHING
		RETURNING id;
	)");

	sql_upsert_object = SQLiteDB::Stmt(db, R"(
		INSERT INTO object_ulid (ulid, chunk_id, parent_ulid, nodepath)
		VALUES (?1, ?2, ?3, ?4)
		ON CONFLICT(ulid) DO UPDATE SET
			chunk_id = excluded.chunk_id,
			parent_ulid = excluded.parent_ulid,
			nodepath = excluded.nodepath
	)");

	sql_query_chunk = SQLiteDB::Stmt(db, R"(
		SELECT id FROM chunk_metadata
		WHERE level = ? AND tile_x = ? AND tile_y = ? AND tile_z = ?;
	)");

	sql_query_object = SQLiteDB::Stmt(db, R"(
		SELECT * FROM object_ulid
		WHERE ulid = ?;
	)");

	sql_query_ulid_of_aabb = SQLiteDB::Stmt(db, R"(
		SELECT o.*
		FROM object_ulid o
		JOIN chunk_metadata c ON o.chunk_id = c.id
		JOIN chunk_rtree r ON c.id = r.id
		WHERE r.minX <= ? AND r.maxX >= ?
		AND r.minY <= ? AND r.maxY >= ?
		AND r.minZ <= ? AND r.maxZ >= ?;
	)");

	sql_remove_object = SQLiteDB::Stmt(db, R"(
		DELETE FROM object_ulid WHERE ulid = ?;
	)");

	sql_id_remove_chunk = SQLiteDB::Stmt(db, R"(
		DELETE FROM chunk_metadata WHERE id = ?;
	)");

	sql_info_remove_chunk = SQLiteDB::Stmt(db, R"(
		DELETE FROM chunk_metadata
		WHERE (level, tile_x, tile_y, tile_z) = (?1, ?2, ?3, ?4);
	)");

	db.exec("PRAGMA foreign_keys = ON;");
}

void StreamManager::StreamSqliteDB::clear_and_create_table() {
	db.exec("BEGIN TRANSACTION;");

	db.exec(R"(
		CREATE TABLE db_info (
			key TEXT PRIMARY KEY,
			value TEXT,
			description TEXT
		);
	)");

	db.exec(R"(
		CREATE VIRTUAL TABLE chunk_rtree USING rtree_i32(
			id,               -- 对应 chunk_metadata.id
			minX, maxX,       -- X 轴范围
			minY, maxY,       -- Y 轴范围
			minZ, maxZ        -- Z 轴范围
		);
	)");

	db.exec(R"(
		CREATE TABLE chunk_metadata (
			id INTEGER PRIMARY KEY,          -- 主键，与 R-Tree 的 id 一致
			level INTEGER,                   -- 层级（LOD 等级）
			tile_x INTEGER,                   -- 瓦片 X 坐标
			tile_y INTEGER,                   -- 瓦片 Y 坐标
			tile_z INTEGER,                   -- 瓦片 Z 坐标
			UNIQUE(level, tile_x, tile_y, tile_z)  -- 确保坐标组合唯一
		);
	)");

	db.exec(R"(
	CREATE TRIGGER after_delete_chunk_metadata 
		AFTER DELETE ON chunk_metadata
		BEGIN
			DELETE FROM chunk_rtree WHERE id = OLD.id;
		END;
	)");

	db.exec(R"(
    CREATE TABLE object_ulid (
			ulid BLOB PRIMARY KEY,            -- 对象 ULID（128 位二进制）
			chunk_id INTEGER NOT NULL,         -- 所属 chunk ID
			parent_ulid BLOB,                  -- 父对象 ULID（允许 NULL）
			nodepath TEXT,                       -- 对象的字符串属性
			FOREIGN KEY (chunk_id) REFERENCES chunk_metadata(id),
			FOREIGN KEY (parent_ulid) REFERENCES object_ulid(ulid) ON DELETE CASCADE
		);
	)");

	db.exec(R"(
		CREATE TRIGGER after_insert_chunk_metadata
		AFTER INSERT ON chunk_metadata
		BEGIN
			INSERT OR IGNORE INTO chunk_rtree (id, minX, maxX, minY, maxY, minZ, maxZ)
			VALUES (
				NEW.id,
				NEW.tile_x, NEW.tile_x + (1 << (2 * NEW.level)),
				NEW.tile_y, NEW.tile_y + (1 << (2 * NEW.level)),
				NEW.tile_z, NEW.tile_z + (1 << (2 * NEW.level))
			);
		END;
	)");

	db.exec("CREATE INDEX idx_object_chunk ON object_ulid(chunk_id);");
	db.exec("CREATE INDEX idx_object_parent ON object_ulid(parent_ulid);");
	db.exec("CREATE INDEX idx_object_path ON object_ulid(nodepath);");

	db.exec("COMMIT;");
}

bool StreamManager::StreamSqliteDB::is_database() {
	auto sql_check_table = SQLiteDB::Stmt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?;");
	sql_check_table.bind_text(1, "db_info");
	if (!sql_check_table.step()) {
		return false;
	}
	int count = sql_check_table.get_int(0);
	return count > 0;
}

ankerl::unordered_dense::map<ulid::ULID, StreamManager::Object> StreamManager::StreamSqliteDB::query_objects(const AABB aabb) {
	ankerl::unordered_dense::map<ulid::ULID, StreamManager::Object> result;

	Vector3i min_pos = aabb.position;
	Vector3i max_pos = aabb.get_end();

	sql_query_ulid_of_aabb.bind_int(1, max_pos.x);
	sql_query_ulid_of_aabb.bind_int(2, min_pos.x);
	sql_query_ulid_of_aabb.bind_int(3, max_pos.y);
	sql_query_ulid_of_aabb.bind_int(4, min_pos.y);
	sql_query_ulid_of_aabb.bind_int(5, max_pos.z);
	sql_query_ulid_of_aabb.bind_int(6, min_pos.z);

	while (sql_query_ulid_of_aabb.step()) {
		const void *blob = sql_query_ulid_of_aabb.get_blob(0);
		if (!blob) {
			UtilityFunctions::print("get_blob(0) failed");
			continue;
		}
		ulid::ULID ulid = *static_cast<const ulid::ULID *>(blob);
		DEBUG_CODE(UtilityFunctions::print("get_ulid ", ulid::Marshal(ulid).c_str()));

		int chunk_id = sql_query_ulid_of_aabb.get_int(1);

		ulid::ULID parent_blob = read_nullable_ulid(sql_query_ulid_of_aabb, 2);
		if (parent_blob == ulid::ULID(0) && !sql_query_ulid_of_aabb.is_null(2)) {
			// get_blob(2) 失败
			UtilityFunctions::print("get_blob(2) failed");
			continue;
		}

		const std::string nodepath = sql_query_ulid_of_aabb.get_text(3);
		if (nodepath.empty()) {
			UtilityFunctions::print("get_text(3) failed");
			continue;
		}

		result[ulid] = Object{ NodePath(String(nodepath.c_str())), parent_blob, chunk_id };
	}

	stmt_finish(sql_query_ulid_of_aabb);

	return result;
}

void StreamManager::StreamSqliteDB::upsert_object(const ulid::ULID ulid, const Object &object) {
	sql_upsert_object.bind_blob(1, &ulid, sizeof(ulid::ULID));
	sql_upsert_object.bind_int(2, object.chunk_id);
	if (object.parent == ulid::ULID(0)) {
		sql_upsert_object.bind_null(3);
	} else {
		sql_upsert_object.bind_blob(3, &object.parent, sizeof(ulid::ULID));
	}
	sql_upsert_object.bind_text(4, godot::String(object.path).utf8().get_data());

	sql_upsert_object.step();

	stmt_finish(sql_upsert_object);
}

int StreamManager::StreamSqliteDB::query_chunk(const Chunk chunk) {
	int cached_id;
	if (chunk_cache.tryGet(chunk, cached_id)) {
		return cached_id;
	}

	bind_chunk(sql_upsert_chunk, chunk);

	int id = -1;
	if (sql_upsert_chunk.step()) {
		id = sql_upsert_chunk.get_int(0); // 新插入，RETURNING 返回 id
	} else {
		// 已存在，查询 id
		bind_chunk(sql_query_chunk, chunk);
		if (sql_query_chunk.step()) {
			id = sql_query_chunk.get_int(0);
		}
		stmt_finish(sql_query_chunk);
	}

	stmt_finish(sql_upsert_chunk);

	chunk_cache.insert(chunk, id);
	return id;
}

// 如果不为有效的 ULID，返回空chunk，可用 != 判断
StreamManager::Object StreamManager::StreamSqliteDB::query_object(const ulid::ULID ulid) {
	sql_query_object.bind_blob(1, &ulid, sizeof(ulid::ULID));

	Object result;
	if (sql_query_object.step()) {
		int chunk_id = sql_query_object.get_int(1);
		ulid::ULID parent_ulid = read_nullable_ulid(sql_query_object, 2);
		std::string nodepath_str = sql_query_object.get_text(3);
		NodePath nodepath = String(nodepath_str.c_str());

		result = Object{ nodepath, parent_ulid, chunk_id };
	}

	stmt_finish(sql_query_object);
	return result;
}

void StreamManager::StreamSqliteDB::remove_object(const ulid::ULID ulid) {
	sql_remove_object.bind_blob(1, &ulid, sizeof(ulid::ULID));
	sql_remove_object.step();
	stmt_finish(sql_remove_object);
}

void StreamManager::StreamSqliteDB::update_info(std::string version) {
	auto sql_upsert_info = SQLiteDB::Stmt(db, "INSERT OR REPLACE INTO db_info (key, value) VALUES (?, ?);");

	sql_upsert_info.bind_text(1, "version");
	sql_upsert_info.bind_text(2, version);

	sql_upsert_info.step();

	stmt_finish(sql_upsert_info);
}
