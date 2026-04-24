#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "debug_def.h"
#include "godot_sqlite/sqlite/sqlite3.h"
#include "sqlitedb.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp> // 添加 Godot 输出函数

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
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("Cannot open database: ", errmsg, " (Path: ", db_path.c_str(), ")");
		});
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
        DEBUG_CODE({
            godot::UtilityFunctions::printerr("[SQLiteDB] db_ is null");
        });
        return false;
    }

    // 先检查数据库是否处于 autocommit 模式（可选，用于诊断）
    int auto_commit = sqlite3_get_autocommit(db_);
    DEBUG_CODE({
        godot::UtilityFunctions::printerr("[SQLiteDB] Autocommit status: ", auto_commit);
    });

    sqlite3_stmt *stmt = nullptr;
    // 准备简单查询
    int rc = sqlite3_prepare_v2(db_, "SELECT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DEBUG_CODE({
            godot::UtilityFunctions::printerr("[SQLiteDB] Prepare failed: error code=", rc,
                                              " msg=", sqlite3_errmsg(db_));
        });
        return false;
    }

    // 执行查询
    rc = sqlite3_step(stmt);
    bool valid = false;
    if (rc == SQLITE_ROW) {
        valid = true;
    } else {
        DEBUG_CODE({
            godot::UtilityFunctions::printerr("[SQLiteDB] Step failed: error code=", rc,
                                              " msg=", sqlite3_errmsg(db_));
        });
    }

    // 必须 finalize，避免资源泄露
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        DEBUG_CODE({
            godot::UtilityFunctions::printerr("[SQLiteDB] Finalize failed: error code=", rc,
                                              " msg=", sqlite3_errmsg(db_));
        });
        // 即使 finalize 失败，也要返回 valid 的状态（但实际可能有问题）
        // 这里可考虑返回 false 表示连接不可用
        if (valid) {
            DEBUG_CODE({
                godot::UtilityFunctions::printerr("[SQLiteDB] Connection might be unstable due to finalize error");
            });
        }
        // 可根据需要决定是否因 finalize 失败而返回 false
        // 这里暂时保持 valid 原值，但记录错误
    }

    return valid;
}

const void SQLiteDB::exec(const std::string &sql) {
	if (!db_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("exec called on invalid database");
		});
		return;
	}

	char *err = nullptr;
	int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		std::string msg = err ? err : "Unknown error";
		sqlite3_free(err);
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("SQL error: ", msg.c_str(), "\nQuery: ", sql.c_str());
		});
	}
}

SQLiteDB::Stmt::Stmt(SQLiteDB &db_obj, const std::string &sql) : db_(db_obj.db_), stmt_(nullptr) {
	if (!db_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("Stmt: database handle is null");
		});
		return;
	}

	int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
	if (rc != SQLITE_OK) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("Prepare failed: ", sqlite3_errmsg(db_), "\nSQL: ", sql.c_str());
		});
		stmt_ = nullptr;
	}
}

SQLiteDB::Stmt::~Stmt() {
	if (stmt_)
		sqlite3_finalize(stmt_);
}

void SQLiteDB::Stmt::bind_int(int idx, int val) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("bind_int called on invalid statement");
		});
		return;
	}
	sqlite3_bind_int(stmt_, idx, val);
}

void SQLiteDB::Stmt::bind_text(int idx, const std::string &val) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("bind_text called on invalid statement");
		});
		return;
	}
	sqlite3_bind_text(stmt_, idx, val.c_str(), -1, SQLITE_TRANSIENT);
}

void SQLiteDB::Stmt::bind_double(int idx, double val) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("bind_double called on invalid statement");
		});
		return;
	}
	sqlite3_bind_double(stmt_, idx, val);
}

void SQLiteDB::Stmt::bind_blob(int idx, const void *val, uint size) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("bind_blob called on invalid statement");
		});
		return;
	}
	sqlite3_bind_blob(stmt_, idx, val, size, SQLITE_TRANSIENT);
}

void SQLiteDB::Stmt::bind_null(int idx) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("bind_null called on invalid statement");
		});
		return;
	}
	sqlite3_bind_null(stmt_, idx);
}

bool SQLiteDB::Stmt::step() {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("step called on invalid statement");
		});
		return false;
	}

	int rc = sqlite3_step(stmt_);
	if (rc == SQLITE_ROW)
		return true;
	if (rc == SQLITE_DONE)
		return false;

	DEBUG_CODE({
		godot::UtilityFunctions::printerr("Step failed: ", sqlite3_errmsg(db_));
	});
	return false;
}

void SQLiteDB::Stmt::reset() {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("reset called on invalid statement");
		});
		return;
	}

	int rc = sqlite3_reset(stmt_);
	if (rc != SQLITE_OK) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("Reset failed: ", sqlite3_errmsg(db_));
		});
	}
}

void SQLiteDB::Stmt::clear_bindings() {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("clear_bindings called on invalid statement");
		});
		return;
	}

	int rc = sqlite3_clear_bindings(stmt_);
	if (rc != SQLITE_OK) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("Clear bindings failed: ", sqlite3_errmsg(db_));
		});
	}
}

std::string SQLiteDB::Stmt::get_text(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_text called on invalid statement");
		});
		return "";
	}
	return reinterpret_cast<const char *>(sqlite3_column_text(stmt_, col));
}

int SQLiteDB::Stmt::get_int(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_int called on invalid statement");
		});
		return 0;
	}
	return sqlite3_column_int(stmt_, col);
}

double SQLiteDB::Stmt::get_double(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_double called on invalid statement");
		});
		return 0.0;
	}
	return sqlite3_column_double(stmt_, col);
}

sqlite3_int64 SQLiteDB::Stmt::get_int64(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_int64 called on invalid statement");
		});
		return 0;
	}
	return sqlite3_column_int64(stmt_, col);
}

bool SQLiteDB::Stmt::get_boolean(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_boolean called on invalid statement");
		});
		return false;
	}
	return sqlite3_column_int(stmt_, col) != 0;
}

const void *SQLiteDB::Stmt::get_blob(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("get_blob called on invalid statement");
		});
		return nullptr;
	}
	return sqlite3_column_blob(stmt_, col);
}

bool SQLiteDB::Stmt::is_null(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("is_null called on invalid statement");
		});
		return true;
	}
	return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int SQLiteDB::Stmt::column_bytes(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("column_bytes called on invalid statement");
		});
		return 0;
	}
	return sqlite3_column_bytes(stmt_, col);
}

int SQLiteDB::Stmt::column_count() {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("column_count called on invalid statement");
		});
		return 0;
	}
	return sqlite3_column_count(stmt_);
}

std::string SQLiteDB::Stmt::column_name(int col) {
	if (!stmt_) {
		DEBUG_CODE({
			godot::UtilityFunctions::printerr("column_name called on invalid statement");
		});
		return "";
	}
	const char *name = sqlite3_column_name(stmt_, col);
	return name ? std::string(name) : std::string();
}

sqlite3 *SQLiteDB::get_handle() { return db_; }