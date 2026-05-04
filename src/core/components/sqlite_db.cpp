#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

#include "gdsqlite/sqlite/sqlite3.h"
#include "sqlite_db.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

SQLiteDB::SQLiteDB(const std::string &db_path, bool read_only) : db_(nullptr) {
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

	int rc = -1;
	const char *vfs_name = "godot";

	if (read_only) {
		flags = SQLITE_OPEN_READONLY;
		rc = sqlite3_open_v2(db_path.c_str(), &db_, flags, vfs_name);
	} else {
		rc = sqlite3_open_v2(godot::ProjectSettings::get_singleton()->globalize_path(godot::String(db_path.c_str()).strip_edges()).utf8().get_data(), &db_, flags, NULL);
	}

	if (rc != SQLITE_OK) {
		const char *errmsg = db_ ? sqlite3_errmsg(db_) : "unknown error";

		if (db_) {
			sqlite3_close(db_);
			db_ = nullptr;
		}
		return;
	}

	exec("PRAGMA journal_mode=WAL;");
	exec("PRAGMA synchronous=NORMAL;");
	exec("PRAGMA cache_size=-20000;");
}

SQLiteDB::~SQLiteDB() {
	if (db_)
		sqlite3_close(db_);
}

bool SQLiteDB::is_connection_valid() {
    if (!db_) {
        return false;
    }

    // 先检查数据库是否处于 autocommit 模式（可选，用于诊断）
    int auto_commit = sqlite3_get_autocommit(db_);

    sqlite3_stmt *stmt = nullptr;
    // 准备简单查询
    int rc = sqlite3_prepare_v2(db_, "SELECT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    // 执行查询
    rc = sqlite3_step(stmt);
    bool valid = false;
    if (rc == SQLITE_ROW) {
        valid = true;
    }

    // 必须 finalize，避免资源泄露
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        // 即使 finalize 失败，也要返回 valid 的状态（但实际可能有问题）
        // 这里可考虑返回 false 表示连接不可用

        // 可根据需要决定是否因 finalize 失败而返回 false
        // 这里暂时保持 valid 原值，但记录错误
    }

    return valid;
}

const void SQLiteDB::exec(const std::string &sql) {
	if (!db_) {
		return;
	}

	char *err = nullptr;
	int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		std::string msg = err ? err : "Unknown error";
		sqlite3_free(err);
	}
}

SQLiteDB::Stmt::Stmt(SQLiteDB &db_obj, const std::string &sql) : db_(db_obj.db_), stmt_(nullptr) {
	if (!db_) {
		return;
	}

	int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
	if (rc != SQLITE_OK) {
		stmt_ = nullptr;
	}
}

SQLiteDB::Stmt::~Stmt() {
	if (stmt_)
		sqlite3_finalize(stmt_);
}

void SQLiteDB::Stmt::bind_int(int idx, int val) {
	if (!stmt_) {
		return;
	}
	sqlite3_bind_int(stmt_, idx, val);
}

void SQLiteDB::Stmt::bind_text(int idx, const std::string &val) {
	if (!stmt_) {
		return;
	}
	sqlite3_bind_text(stmt_, idx, val.c_str(), -1, SQLITE_TRANSIENT);
}

void SQLiteDB::Stmt::bind_double(int idx, double val) {
	if (!stmt_) {
		return;
	}
	sqlite3_bind_double(stmt_, idx, val);
}

void SQLiteDB::Stmt::bind_blob(int idx, const void *val, size_t size) {
	if (!stmt_) {
		return;
	}
	sqlite3_bind_blob(stmt_, idx, val, size, SQLITE_TRANSIENT);
}

void SQLiteDB::Stmt::bind_null(int idx) {
	if (!stmt_) {
		return;
	}
	sqlite3_bind_null(stmt_, idx);
}

bool SQLiteDB::Stmt::step() {
	if (!stmt_) {
		return false;
	}

	int rc = sqlite3_step(stmt_);
	if (rc == SQLITE_ROW)
		return true;
	if (rc == SQLITE_DONE)
		return false;

	return false;
}

void SQLiteDB::Stmt::reset() {
	if (!stmt_) {
		return;
	}

	int rc = sqlite3_reset(stmt_);
}

void SQLiteDB::Stmt::clear_bindings() {
	if (!stmt_) {
		return;
	}

	int rc = sqlite3_clear_bindings(stmt_);
}

std::string SQLiteDB::Stmt::get_text(int col) {
	if (!stmt_) {
		return "";
	}
	return reinterpret_cast<const char *>(sqlite3_column_text(stmt_, col));
}

int SQLiteDB::Stmt::get_int(int col) {
	if (!stmt_) {
		return 0;
	}
	return sqlite3_column_int(stmt_, col);
}

double SQLiteDB::Stmt::get_double(int col) {
	if (!stmt_) {
		return 0.0;
	}
	return sqlite3_column_double(stmt_, col);
}

sqlite3_int64 SQLiteDB::Stmt::get_int64(int col) {
	if (!stmt_) {
		return 0;
	}
	return sqlite3_column_int64(stmt_, col);
}

bool SQLiteDB::Stmt::get_boolean(int col) {
	if (!stmt_) {
		return false;
	}
	return sqlite3_column_int(stmt_, col) != 0;
}

const void *SQLiteDB::Stmt::get_blob(int col) {
	if (!stmt_) {
		return nullptr;
	}
	return sqlite3_column_blob(stmt_, col);
}

bool SQLiteDB::Stmt::is_null(int col) {
	if (!stmt_) {
		return true;
	}
	return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int SQLiteDB::Stmt::column_bytes(int col) {
	if (!stmt_) {
		return 0;
	}
	return sqlite3_column_bytes(stmt_, col);
}

int SQLiteDB::Stmt::column_count() {
	if (!stmt_) {
		return 0;
	}
	return sqlite3_column_count(stmt_);
}

std::string SQLiteDB::Stmt::column_name(int col) {
	if (!stmt_) {
		return "";
	}
	const char *name = sqlite3_column_name(stmt_, col);
	return name ? std::string(name) : std::string();
}

sqlite3 *SQLiteDB::get_handle() { return db_; }