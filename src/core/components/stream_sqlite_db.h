#pragma once

#include "chunk.h"
#include "object_data.h"
#include "sqlite_db.h"
#include "core/ankerl.h"
#include "core/caches.h"

#include <uuid.h>
#include <godot_cpp/variant/aabb.hpp>
#include <string>

class StreamSqliteDB {
public:
    StreamSqliteDB(const std::string &path, bool read_only = false);
    ~StreamSqliteDB() = default;

    void clear_and_create_table();
    bool is_database();

    a_hashmap<uuids::uuid, ObjectData> query_objects(const godot::AABB aabb);

    void upsert_object(const uuids::uuid uuid, const ObjectData &object);
    int query_chunk(const Chunk chunk);
    ObjectData query_object(const uuids::uuid uuid);
    void remove_object(const uuids::uuid uuid);

    // 新增：设置 / 获取对象的 AABB（带 LRU 缓存）
    void set_object_aabb(const uuids::uuid &uuid, const godot::AABB &aabb);
    godot::AABB get_object_aabb(const uuids::uuid &uuid);

    SQLiteDB db;

private:
    lru_cache_t<Chunk, int> chunk_cache;
    lru_cache_t<uuids::uuid, godot::AABB> aabb_cache;

    SQLiteDB::Stmt sql_upsert_chunk;
    SQLiteDB::Stmt sql_upsert_object;

    SQLiteDB::Stmt sql_query_chunk;
    SQLiteDB::Stmt sql_query_object;
    SQLiteDB::Stmt sql_query_uuid_of_aabb;

    SQLiteDB::Stmt sql_remove_object;
    SQLiteDB::Stmt sql_id_remove_chunk;
    SQLiteDB::Stmt sql_info_remove_chunk;

    SQLiteDB::Stmt sql_set_object_aabb;
    SQLiteDB::Stmt sql_get_object_aabb;

    void update_info(std::string version);

    // reset + clear_bindings 合并
    static void stmt_finish(SQLiteDB::Stmt &stmt);
    // 绑定 chunk 的四个字段（level/x/y/z）到语句的 ?1~?4
    static void bind_chunk(SQLiteDB::Stmt &stmt, const Chunk &chunk);
    // 从语句中读取可空的 uuid blob 字段，null 返回 uuid(0)
    static uuids::uuid read_nullable_uuid(SQLiteDB::Stmt &stmt, int col);

    // 绑定 AABB 到语句（从起始参数位置 start_idx 开始，共 6 个绑定）
    static void bind_aabb(SQLiteDB::Stmt &stmt, const godot::AABB &aabb, int start_idx = 1);
    // 从语句当前行读取 AABB（列索引 col_start 开始的 6 个整数）
    static godot::AABB read_aabb_from_stmt(SQLiteDB::Stmt &stmt, int col_start = 0);
};