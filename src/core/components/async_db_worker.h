#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "stream_sqlite_db.h"

class AsyncDbWorker : public godot::RefCounted {
	GDCLASS(AsyncDbWorker, godot::RefCounted)

public:
	// 任务定义：work 在后台线程执行，on_complete 在主线程执行
	struct Task {
		std::function<void(StreamSqliteDB &)> work;
		std::function<void()> on_complete;
	};

	AsyncDbWorker() = default;
	AsyncDbWorker(const godot::String &db_path, bool read_only = false);
	~AsyncDbWorker();

	// 主线程调用，投递任务
	void push_task(Task task);

	// 主线程调用（通常在 _process 中），执行已完成回调
	void flush_callbacks();

private:
	void thread_loop();

	std::unique_ptr<StreamSqliteDB> db_; // 工作线程独享
	std::thread worker_;

	std::queue<Task> task_queue_;
	std::queue<std::function<void()>> completed_callbacks_;

	std::mutex queue_mutex_;
	std::condition_variable cv_;
	std::atomic<bool> stop_{ false };

protected:
	static void _bind_methods();
};
