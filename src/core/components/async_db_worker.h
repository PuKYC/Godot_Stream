#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>

#include "core/ankerl.h"
#include "stream_sqlite_db.h"

class AsyncDbWorker : public godot::RefCounted {
	GDCLASS(AsyncDbWorker, godot::RefCounted)

public:
	struct Task {
		std::function<void(StreamSqliteDB &)> work;
		std::function<void()> on_complete;
	};

	AsyncDbWorker() = default;
	AsyncDbWorker(const godot::String &db_path, bool read_only = false);
	~AsyncDbWorker();

	void push_task(Task task);
	void _flush_callbacks();

	// 在合适的时机连接到 SceneTree 的 process_frame，若失败则打印警告
	void connect_flush_to_tree();

	static godot::Ref<AsyncDbWorker> make_db(const godot::String &db_path, bool read_only = false);

private:
	void thread_loop();

	std::unique_ptr<StreamSqliteDB> db_;
	std::thread worker_;

	std::queue<Task> task_queue_;
	std::queue<std::function<void()>> completed_callbacks_;

	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> stop_{ false };

	godot::String db_path_;
	bool read_only_ = false;

	// 缓存键
	struct DbKey {
		godot::String path;
		bool read_only;

		bool operator==(const DbKey &other) const {
			return path == other.path && read_only == other.read_only;
		}
	};

	struct DbKeyHash {
		size_t operator()(const DbKey &k) const {
			// 简单组合哈希
			size_t h1 = std::hash<std::string>()(k.path.utf8().get_data());
			size_t h2 = std::hash<bool>()(k.read_only);
			return h1 ^ (h2 << 1);
		}
	};

	// 缓存：键 → ObjectID（弱引用）
	static a_hashmap<DbKey, uint64_t, DbKeyHash> _db_worker_cache;
	static std::mutex _cache_mutex;

protected:
	static void _bind_methods();
};
