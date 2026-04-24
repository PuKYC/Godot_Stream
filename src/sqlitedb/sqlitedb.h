#pragma once

#include "godot_sqlite/sqlite/sqlite3.h"
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <godot_cpp/classes/file_access.hpp>

class SQLiteDB {
public:
	SQLiteDB() = default;
	SQLiteDB(const std::string &db_path, bool read_only = false);
	~SQLiteDB();

	// 禁止拷贝
	SQLiteDB(const SQLiteDB &) = delete;
	SQLiteDB &operator=(const SQLiteDB &) = delete;

	// 允许移动
	SQLiteDB(SQLiteDB &&other) noexcept : db_(other.db_) {
		other.db_ = nullptr; // 转移所有权，原对象置空
	}
	SQLiteDB &operator=(SQLiteDB &&other) noexcept {
		if (this != &other) {
			if (db_)
				sqlite3_close(db_);
			db_ = other.db_;
			other.db_ = nullptr;
		}
		return *this;
	}

	bool is_connection_valid();
	const void exec(const std::string &sql);

	class Stmt {
	public:
		Stmt() = default;
		Stmt(SQLiteDB &db_obj, const std::string &sql);
		~Stmt();

		// 禁止拷贝
		Stmt(const Stmt &) = delete;
		Stmt &operator=(const Stmt &) = delete;

		// 允许移动
		Stmt(Stmt &&other) noexcept : db_(other.db_), stmt_(other.stmt_) {
			other.stmt_ = nullptr;
			other.db_ = nullptr;
		}
		Stmt &operator=(Stmt &&other) noexcept {
			if (this != &other) {
				if (stmt_)
					sqlite3_finalize(stmt_);
				stmt_ = other.stmt_;
				db_ = other.db_;
				other.stmt_ = nullptr;
				other.db_ = nullptr;
			}
			return *this;
		}

		void bind_int(int idx, int val);
		void bind_text(int idx, const std::string &val);
		void bind_double(int idx, double val);
		void bind_blob(int idx, const void *val, uint size);
		void bind_null(int idx);

		bool step();
		void reset();
		void clear_bindings();

		std::string get_text(int col);
		int get_int(int col);
		double get_double(int col);
		sqlite3_int64 get_int64(int col);
		bool get_boolean(int col);
		const void *get_blob(int col);
		bool is_null(int col);

		int column_bytes(int col);
		int column_count();
		std::string column_name(int col);

	private:
		sqlite3 *db_ = nullptr;
		sqlite3_stmt *stmt_ = nullptr;
	};

	sqlite3 *get_handle();

private:
	sqlite3 *db_ = nullptr;
};
