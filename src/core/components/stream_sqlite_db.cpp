#include "stream_sqlite_db.h"

StreamSqliteDB::StreamSqliteDB(const std::string &path, bool read_only) : db(path, read_only), chunk_cache(500) {
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
		INSERT INTO object_uuid (uuid, chunk_id, parent_uuid, nodepath)
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

	db.exec(R"(
    CREATE TABLE object_uuid (
			uuid BLOB PRIMARY KEY,            -- 对象 uuid（128 位二进制）
			chunk_id INTEGER NOT NULL,         -- 所属 chunk ID
			parent_uuid BLOB,                  -- 父对象 uuid（允许 NULL）
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
		uuids::uuid uuid_key = *static_cast<const uuids::uuid *>(blob); // 修复重复变量

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
	if (result.second){
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

// 如果不为有效的 uuid，返回空chunk，可用 != 判断
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
				result.parent_uuid = uuids::uuid(); // 读取失败时视为根
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
}

void StreamSqliteDB::update_info(std::string version) {
	auto sql_upsert_info = SQLiteDB::Stmt(db, "INSERT OR REPLACE INTO db_info (key, value) VALUES (?, ?);");

	sql_upsert_info.bind_text(1, "version");
	sql_upsert_info.bind_text(2, version);

	sql_upsert_info.step();

	stmt_finish(sql_upsert_info);
}

// reset + clear_bindings 合并，避免每次都写两行
void StreamSqliteDB::stmt_finish(SQLiteDB::Stmt &stmt) {
	stmt.reset();
	stmt.clear_bindings();
}

// 将 chunk 的四个字段绑定到语句的 ?1~?4
void StreamSqliteDB::bind_chunk(SQLiteDB::Stmt &stmt, const Chunk &chunk) {
	stmt.bind_int(1, chunk.level);
	stmt.bind_int(2, chunk.x);
	stmt.bind_int(3, chunk.y);
	stmt.bind_int(4, chunk.z);
}

// 从语句中读取可空的 uuid blob 字段，null 时返回 uuid(0)
uuids::uuid StreamSqliteDB::read_nullable_uuid(SQLiteDB::Stmt &stmt, int col) {
	if (stmt.is_null(col)) {
		return uuids::uuid();
	}
	const void *blob = stmt.get_blob(col);
	return blob ? *static_cast<const uuids::uuid *>(blob) : uuids::uuid();
}