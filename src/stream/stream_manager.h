#pragma once

#include <cstdint>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <memory>
#include <queue>
#include <vector>

#include "debug_def.h"
#include "lru/LRUCache11.hpp"
#include "sqlitedb/sqlitedb.h"
#include "stream_object.h"
#include "unordered_dense-4.8.1/unordered_dense.h"

// 先定义 Chunk（独立结构体）
struct Chunk {
	int32_t x, y, z;
	uint8_t level;

	bool operator==(const Chunk &other) const {
		return x == other.x && y == other.y && z == other.z && level == other.level;
	}
};

// 立即特化 std::hash
namespace std {
template <>
struct hash<Chunk> {
	size_t operator()(const Chunk &chunk) const noexcept {
		size_t h1 = hash<int32_t>()(chunk.x);
		size_t h2 = hash<int32_t>()(chunk.y);
		size_t h3 = hash<int32_t>()(chunk.z);
		size_t h4 = hash<uint8_t>()(chunk.level);
		size_t seed = h1;
		seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= h4 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		return seed;
	}
};

template <>
struct hash<unsigned __int128> {
	size_t operator()(unsigned __int128 val) const noexcept {
		// 组合高低64位，简单但足够
		return static_cast<size_t>(val) ^ static_cast<size_t>(val >> 64);
	}
};
} //namespace std

class StreamManager : public Node3D {
	GDCLASS(StreamManager, Node3D)
public:
	StreamManager() = default;
	~StreamManager() = default;

	void _ready() override;
	void _process(double delta) override;

	String database_path;
	String object_dir;

	// 已经实例化的对象列表
	// 用于序列化存储
	void set_objects(const PackedStringArray objects);
	PackedStringArray get_objects();

	void set_database_path(String path);
	String get_database_path();

	void connect_database(String path);
	void disconnect_database();

	void aabb_query(AABB aabb);

	bool add_object(StreamObject *object);
	bool remove_object(String ulid_str);
	bool update_object(StreamObject *object_node);

	void update_database();

protected:
	static void _bind_methods();

	String scene_file = ".tscn";

	struct Object {
		NodePath path; //存储stream_object的path
		ulid::ULID parent{};
		int chunk_id = -1;

		bool operator==(const Object &other) const {
			return path == other.path && parent == other.parent && chunk_id == other.chunk_id;
		}
	};

	class StreamSqliteDB {
	public:
		StreamSqliteDB() = default;
		StreamSqliteDB(const std::string &path, bool read_only = false);
		~StreamSqliteDB() = default;

		void clear_and_create_table();
		bool is_database();

		ankerl::unordered_dense::map<ulid::ULID, Object> query_objects(const AABB aabb);

		void upsert_object(const ulid::ULID ulid, const Object &object);
		int query_chunk(const Chunk chunk);
		Object query_object(const ulid::ULID ulid);
		void remove_object(const ulid::ULID ulid);

		SQLiteDB db;

	private:
		lru11::Cache<Chunk, int> chunk_cache;

		SQLiteDB::Stmt sql_upsert_chunk;
		SQLiteDB::Stmt sql_upsert_object;

		SQLiteDB::Stmt sql_query_chunk;
		SQLiteDB::Stmt sql_query_object;
		SQLiteDB::Stmt sql_query_ulid_of_aabb;

		SQLiteDB::Stmt sql_remove_object;
		SQLiteDB::Stmt sql_id_remove_chunk;
		SQLiteDB::Stmt sql_info_remove_chunk;

		void update_info(std::string version);

		// ── 重复操作小函数 ──────────────────────────────────────
		// reset + clear_bindings 合并
		static void stmt_finish(SQLiteDB::Stmt &stmt);
		// 绑定 chunk 的四个字段（level/x/y/z）到语句的 ?1~?4
		static void bind_chunk(SQLiteDB::Stmt &stmt, const Chunk &chunk);
		// 从语句中读取可空的 ULID blob 字段，null 返回 ULID(0)
		static ulid::ULID read_nullable_ulid(SQLiteDB::Stmt &stmt, int col);
	};

	ankerl::unordered_dense::map<ulid::ULID, Object> ulid_object_table;
	ankerl::unordered_dense::map<ulid::ULID, ankerl::unordered_dense::set<ulid::ULID>> object_child_table;

	ankerl::unordered_dense::set<ulid::ULID> upsert_object_set;
	ankerl::unordered_dense::set<ulid::ULID> remove_object_set;
	void upsert_object_list(ankerl::unordered_dense::set<ulid::ULID> &upsert_object_set);
	void remove_object_list(ankerl::unordered_dense::set<ulid::ULID> &remove_object_set);

	bool save_object(const StreamObject &stream_object, ulid::ULID &ulid);
	static inline uint8_t compute_level_from_size(const AABB &aabb);
	Chunk compute_chunk_from_aabb(const AABB &aabb);

	std::unique_ptr<StreamSqliteDB> db = nullptr;

	// 处理场景加载的函数
	void load_object_list();
	size_t load_vector_size = 5;
	// 待加载队列 (ULID)
	std::queue<ulid::ULID> load_queue;
	// 等待获取场景的队列
	std::queue<ulid::ULID> loading_queue;

	// 构造 object 场景文件路径：object_dir + marshal(ulid) + scene_file
	String object_scene_path(const ulid::ULID &ulid) const;
	// 执行 BEGIN / remove_object_list / upsert_object_list / COMMIT
	void flush_pending_db_ops();
	// 标记 stream_remove 并 queue_free 父节点
	void free_stream_node(StreamObject *node);
};
