#include "stream_sqlite_db.h"

using namespace godot;

StreamSqliteDB::StreamSqliteDB(const std::string &path, bool read_only) : db(path, read_only),
																		  chunk_cache(500, caches::LRUCachePolicy<Chunk>{}),
																		  aabb_cache(1000, caches::LRUCachePolicy<uuids::uuid>{}) {
	if (!is_database()) {
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
        INSERT INTO object_uuid (uuid, chunk_id, parent_uuid)
        VALUES (?1, ?2, ?3)
        ON CONFLICT(uuid) DO UPDATE SET
            chunk_id = excluded.chunk_id,
            parent_uuid = excluded.parent_uuid
    )");

	sql_query_chunk = SQLiteDB::Stmt(db, R"(
        SELECT id FROM chunk_metadata
        WHERE level = ? AND tile_x = ? AND tile_y = ? AND tile_z = ?;
    )");

	sql_query_object = SQLiteDB::Stmt(db, R"(
        SELECT * FROM object_uuid
        WHERE uuid = ?;
    )");

	sql_query_uuid_of_aabb = SQLiteDB::Stmt(db, R"(
        SELECT o.*
        FROM object_uuid o
        JOIN chunk_metadata c ON o.chunk_id = c.id
        JOIN chunk_rtree r ON c.id = r.id
        WHERE r.minX <= ? AND r.maxX >= ?
        AND r.minY <= ? AND r.maxY >= ?
        AND r.minZ <= ? AND r.maxZ >= ?;
    )");

	sql_remove_object = SQLiteDB::Stmt(db, R"(
        DELETE FROM object_uuid WHERE uuid = ?;
    )");

	sql_id_remove_chunk = SQLiteDB::Stmt(db, R"(
        DELETE FROM chunk_metadata WHERE id = ?;
    )");

	sql_info_remove_chunk = SQLiteDB::Stmt(db, R"(
        DELETE FROM chunk_metadata
        WHERE (level, tile_x, tile_y, tile_z) = (?1, ?2, ?3, ?4);
    )");

	// 新增：更新对象 AABB 的语句
	sql_set_object_aabb = SQLiteDB::Stmt(db, R"(
        UPDATE object_uuid
        SET minX = ?1, maxX = ?2, minY = ?3, maxY = ?4, minZ = ?5, maxZ = ?6
        WHERE uuid = ?7;
    )");

	// 新增：查询对象 AABB 的语句
	sql_get_object_aabb = SQLiteDB::Stmt(db, R"(
        SELECT minX, maxX, minY, maxY, minZ, maxZ
        FROM object_uuid
        WHERE uuid = ?;
    )");

	// 按 parent_uuid 查询所有子对象的 uuid 和 AABB
	sql_query_children_aabb = SQLiteDB::Stmt(db, R"(
        SELECT uuid, minX, maxX, minY, maxY, minZ, maxZ
        FROM object_uuid
        WHERE parent_uuid = ?;
    )");

	db.exec("PRAGMA foreign_keys = ON;");
}

void StreamSqliteDB::clear_and_create_table() {
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

	// 修改：增加 AABB 的 6 个字段，可为 NULL
	db.exec(R"(
        CREATE TABLE object_uuid (
            uuid BLOB PRIMARY KEY,            -- 对象 uuid（128 位二进制）
            chunk_id INTEGER NOT NULL,         -- 所属 chunk ID
            parent_uuid BLOB,                  -- 父对象 uuid（允许 NULL）
            minX INTEGER,                      -- AABB 最小 X
            maxX INTEGER,                      -- AABB 最大 X
            minY INTEGER,                      -- AABB 最小 Y
            maxY INTEGER,                      -- AABB 最大 Y
            minZ INTEGER,                      -- AABB 最小 Z
            maxZ INTEGER,                      -- AABB 最大 Z
            FOREIGN KEY (chunk_id) REFERENCES chunk_metadata(id),
            FOREIGN KEY (parent_uuid) REFERENCES object_uuid(uuid) ON DELETE CASCADE
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

	db.exec("CREATE INDEX idx_object_chunk ON object_uuid(chunk_id);");
	db.exec("CREATE INDEX idx_object_parent ON object_uuid(parent_uuid);");

	db.exec("COMMIT;");
}

bool StreamSqliteDB::is_database() {
	auto sql_check_table = SQLiteDB::Stmt(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?;");
	sql_check_table.bind_text(1, "db_info");
	if (!sql_check_table.step()) {
		return false;
	}
	int count = sql_check_table.get_int(0);
	return count > 0;
}

a_hashmap<uuids::uuid, ObjectData> StreamSqliteDB::query_objects(const std::vector<godot::AABB> &aabbs) {
	a_hashmap<uuids::uuid, ObjectData> result;
	if (aabbs.empty())
		return result;

	// 1. 动态生成 UNION ALL 的 SQL
	std::stringstream sql;
	sql << "SELECT o.uuid, o.chunk_id, o.parent_uuid "
		   "FROM object_uuid o "
		   "JOIN chunk_metadata c ON o.chunk_id = c.id "
		   "JOIN chunk_rtree r ON c.id = r.id "
		   "WHERE ";

	for (size_t i = 0; i < aabbs.size(); ++i) {
		if (i > 0)
			sql << " UNION ALL ";
		sql << "SELECT o.uuid, o.chunk_id, o.parent_uuid "
			   "FROM object_uuid o "
			   "JOIN chunk_metadata c ON o.chunk_id = c.id "
			   "JOIN chunk_rtree r ON c.id = r.id "
			   "WHERE r.minX <= ?"
			<< (6 * i + 2) << " AND r.maxX >= ?" << (6 * i + 1)
			<< " AND r.minY <= ?" << (6 * i + 4) << " AND r.maxY >= ?" << (6 * i + 3)
			<< " AND r.minZ <= ?" << (6 * i + 6) << " AND r.maxZ >= ?" << (6 * i + 5);
	}

	// 2. 准备语句并绑定所有矩形的参数
	SQLiteDB::Stmt stmt(db, sql.str());
	for (size_t i = 0; i < aabbs.size(); ++i) {
		bind_aabb(stmt, aabbs[i], 6 * i + 1); // start_idx = 6*i + 1
	}

	// 3. 执行并收集结果，map 自动去重
	while (stmt.step()) {
		const void *blob = stmt.get_blob(0);
		if (!blob)
			continue;
		uuids::uuid uuid_key = *static_cast<const uuids::uuid *>(blob);

		int chunk_id = stmt.get_int(1);
		uuids::uuid parent = read_nullable_uuid(stmt, 2);
		result[uuid_key] = ObjectData{ parent, chunk_id };
	}

	stmt_finish(stmt);
	return result;
}

a_hashmap<uuids::uuid, ObjectData> StreamSqliteDB::query_objects(const AABB aabb) {
	a_hashmap<uuids::uuid, ObjectData> result;
	Vector3i min_pos = aabb.position;
	Vector3i max_pos = aabb.get_end();

	sql_query_uuid_of_aabb.bind_int(1, max_pos.x);
	sql_query_uuid_of_aabb.bind_int(2, min_pos.x);
	sql_query_uuid_of_aabb.bind_int(3, max_pos.y);
	sql_query_uuid_of_aabb.bind_int(4, min_pos.y);
	sql_query_uuid_of_aabb.bind_int(5, max_pos.z);
	sql_query_uuid_of_aabb.bind_int(6, min_pos.z);

	while (sql_query_uuid_of_aabb.step()) {
		const void *blob = sql_query_uuid_of_aabb.get_blob(0);
		if (!blob)
			continue;
		uuids::uuid uuid_key = *static_cast<const uuids::uuid *>(blob);

		int chunk_id = sql_query_uuid_of_aabb.get_int(1);

		uuids::uuid parent;
		if (sql_query_uuid_of_aabb.is_null(2)) {
			parent = uuids::uuid();
		} else {
			const void *parent_blob = sql_query_uuid_of_aabb.get_blob(2);
			parent = *static_cast<const uuids::uuid *>(parent_blob);
		}

		result[uuid_key] = ObjectData{ parent, chunk_id };
	}

	stmt_finish(sql_query_uuid_of_aabb);
	return result;
}

void StreamSqliteDB::upsert_object(const uuids::uuid uuid, const ObjectData &objectdata) {
	sql_upsert_object.bind_blob(1, &uuid, sizeof(uuids::uuid));
	sql_upsert_object.bind_int(2, objectdata.chunk_id);

	if (objectdata.parent_uuid == uuids::uuid()) {
		sql_upsert_object.bind_null(3);
	} else {
		sql_upsert_object.bind_blob(3, &objectdata.parent_uuid, sizeof(uuids::uuid));
	}

	sql_upsert_object.step();
	stmt_finish(sql_upsert_object);
}

int StreamSqliteDB::query_chunk(const Chunk chunk) {
	auto result = chunk_cache.TryGet(chunk);
	if (result.second) {
		return result.first;
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

	chunk_cache.Put(chunk, id);
	return id;
}

ObjectData StreamSqliteDB::query_object(const uuids::uuid uuid) {
	sql_query_object.bind_blob(1, &uuid, sizeof(uuids::uuid));
	ObjectData result;
	if (sql_query_object.step()) {
		result.chunk_id = sql_query_object.get_int(1);
		if (sql_query_object.is_null(2)) {
			result.parent_uuid = uuids::uuid();
		} else {
			const void *blob = sql_query_object.get_blob(2);
			if (blob) {
				result.parent_uuid = *static_cast<const uuids::uuid *>(blob);
			} else {
				result.parent_uuid = uuids::uuid();
			}
		}
	}
	stmt_finish(sql_query_object);
	return result;
}

void StreamSqliteDB::remove_object(const uuids::uuid uuid) {
	sql_remove_object.bind_blob(1, &uuid, sizeof(uuids::uuid));
	sql_remove_object.step();
	stmt_finish(sql_remove_object);

	// 同步清除 AABB 缓存
	aabb_cache.Remove(uuid);
}

// --------------------- 新增实现 ---------------------

void StreamSqliteDB::set_object_aabb(const uuids::uuid &uuid, const godot::AABB &aabb) {
	// 更新数据库
	bind_aabb(sql_set_object_aabb, aabb, 1);
	sql_set_object_aabb.bind_blob(7, &uuid, sizeof(uuids::uuid));

	sql_set_object_aabb.step();
	stmt_finish(sql_set_object_aabb);

	// 更新缓存
	aabb_cache.Put(uuid, aabb);
}

godot::AABB StreamSqliteDB::get_object_aabb(const uuids::uuid &uuid) {
	// 1. 尝试从缓存获取
	auto cache_result = aabb_cache.TryGet(uuid);
	if (cache_result.second) {
		return cache_result.first;
	}

	// 2. 查询数据库
	sql_get_object_aabb.bind_blob(1, &uuid, sizeof(uuids::uuid));
	godot::AABB aabb;
	bool found = false;

	if (sql_get_object_aabb.step()) {
		// 检查是否所有 AABB 字段均为 NULL（即未设置 AABB）
		if (!sql_get_object_aabb.is_null(0)) {
			aabb = read_aabb_from_stmt(sql_get_object_aabb, 0);
			found = true;
		}
	}

	stmt_finish(sql_get_object_aabb);

	// 3. 无论是否有效，都存入缓存（默认 AABB 也缓存，避免重复 DB 查询）
	aabb_cache.Put(uuid, aabb);

	return aabb;
}

// --------------------- 辅助函数实现 ---------------------

void StreamSqliteDB::update_info(std::string version) {
	auto sql_upsert_info = SQLiteDB::Stmt(db, "INSERT OR REPLACE INTO db_info (key, value) VALUES (?, ?);");
	sql_upsert_info.bind_text(1, "version");
	sql_upsert_info.bind_text(2, version);
	sql_upsert_info.step();
	stmt_finish(sql_upsert_info);
}

void StreamSqliteDB::stmt_finish(SQLiteDB::Stmt &stmt) {
	stmt.reset();
	stmt.clear_bindings();
}

void StreamSqliteDB::bind_chunk(SQLiteDB::Stmt &stmt, const Chunk &chunk) {
	stmt.bind_int(1, chunk.level);
	stmt.bind_int(2, chunk.x);
	stmt.bind_int(3, chunk.y);
	stmt.bind_int(4, chunk.z);
}

uuids::uuid StreamSqliteDB::read_nullable_uuid(SQLiteDB::Stmt &stmt, int col) {
	if (stmt.is_null(col)) {
		return uuids::uuid();
	}
	const void *blob = stmt.get_blob(col);
	return blob ? *static_cast<const uuids::uuid *>(blob) : uuids::uuid();
}

void StreamSqliteDB::bind_aabb(SQLiteDB::Stmt &stmt, const godot::AABB &aabb, int start_idx) {
	Vector3i min_pos = aabb.position;
	Vector3i max_pos = aabb.get_end();
	stmt.bind_int(start_idx, min_pos.x);
	stmt.bind_int(start_idx + 1, max_pos.x);
	stmt.bind_int(start_idx + 2, min_pos.y);
	stmt.bind_int(start_idx + 3, max_pos.y);
	stmt.bind_int(start_idx + 4, min_pos.z);
	stmt.bind_int(start_idx + 5, max_pos.z);
}

godot::AABB StreamSqliteDB::read_aabb_from_stmt(SQLiteDB::Stmt &stmt, int col_start) {
	Vector3i min_pos(
			stmt.get_int(col_start), // minX
			stmt.get_int(col_start + 2), // minY
			stmt.get_int(col_start + 4) // minZ
	);
	Vector3i max_pos(
			stmt.get_int(col_start + 1), // maxX
			stmt.get_int(col_start + 3), // maxY
			stmt.get_int(col_start + 5) // maxZ
	);
	// godot::AABB 使用 position + size 构造，这里转换为尺寸
	return godot::AABB(min_pos, max_pos - min_pos);
}

// --------------------- query_children_aabb ---------------------

std::vector<std::pair<uuids::uuid, godot::AABB>> StreamSqliteDB::query_children_aabb(const uuids::uuid &parent_uuid) {
	std::vector<std::pair<uuids::uuid, godot::AABB>> result;

	sql_query_children_aabb.bind_blob(1, &parent_uuid, sizeof(uuids::uuid));

	while (sql_query_children_aabb.step()) {
		const void *blob = sql_query_children_aabb.get_blob(0);
		if (!blob)
			continue;
		uuids::uuid child_uuid = *static_cast<const uuids::uuid *>(blob);

		godot::AABB child_aabb;
		if (!sql_query_children_aabb.is_null(1))
			child_aabb = read_aabb_from_stmt(sql_query_children_aabb, 1);

		result.emplace_back(child_uuid, child_aabb);
	}

	stmt_finish(sql_query_children_aabb);
	return result;
}
