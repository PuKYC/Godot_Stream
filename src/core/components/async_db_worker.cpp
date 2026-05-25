#include "async_db_worker.h"

#include "godot_cpp/variant/callable_method_pointer.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/scene_tree.hpp>

using namespace godot;

// 静态缓存定义
a_hashmap<AsyncDbWorker::DbKey, uint64_t, AsyncDbWorker::DbKeyHash> AsyncDbWorker::_db_worker_cache;
std::mutex AsyncDbWorker::_cache_mutex;

godot::Ref<AsyncDbWorker> AsyncDbWorker::make_db(const godot::String &db_path, bool read_only) {
	if (db_path.is_empty()) {
		return nullptr;
	}

	DbKey key{ db_path, read_only };

	std::lock_guard<std::mutex> lock(_cache_mutex);

	// 尝试复用已有 worker（弱引用探测）
	if (_db_worker_cache.count(key)) {
		uint64_t id = _db_worker_cache[key];
		Object *obj = ObjectDB::get_instance(id);
		AsyncDbWorker *existing = Object::cast_to<AsyncDbWorker>(obj);
		if (existing) {
			return Ref<AsyncDbWorker>(existing);
		}
		// 对象已销毁，清理残留记录
		_db_worker_cache.erase(key);
	}

	// 创建新 worker
	Ref<AsyncDbWorker> worker = Ref<AsyncDbWorker>(memnew(AsyncDbWorker(db_path, read_only)));
	if (worker.is_valid()) {
		_db_worker_cache[key] = worker->get_instance_id();
	}
	// 尝试连接帧刷新（即使失败也不影响返回）
	worker->connect_flush_to_tree();
	return worker;
}

AsyncDbWorker::AsyncDbWorker(const String &db_path, bool read_only)
	: db_path_(db_path), read_only_(read_only) {
	db_ = std::make_unique<StreamSqliteDB>(db_path.utf8().get_data(), read_only);
	worker_ = std::thread(&AsyncDbWorker::thread_loop, this);
}

AsyncDbWorker::~AsyncDbWorker() {
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		stop_ = true;
	}
	cv_.notify_one();
	if (worker_.joinable()) {
		worker_.join();
	}
}

void AsyncDbWorker::push_task(Task task) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		task_queue_.emplace(std::move(task));
	}
	cv_.notify_one();
}

void AsyncDbWorker::_flush_callbacks() {
	std::queue<std::function<void()>> local;
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		local.swap(completed_callbacks_);
	}
	while (!local.empty()) {
		auto cb = std::move(local.front());
		local.pop();
		cb();
	}
}

void AsyncDbWorker::connect_flush_to_tree() {
	auto *main_loop = Engine::get_singleton()->get_main_loop();
	SceneTree *tree = Object::cast_to<SceneTree>(main_loop);
	if (tree) {
		tree->connect("process_frame", callable_mp(this, &AsyncDbWorker::_flush_callbacks));
	} else {
		WARN_PRINT("AsyncDbWorker: No SceneTree available to connect process_frame. "
				   "You must manually call _flush_callbacks() on the main thread.");
	}
}

void AsyncDbWorker::thread_loop() {
	while (true) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			cv_.wait(lock, [this] { return stop_ || !task_queue_.empty(); });
			if (stop_ && task_queue_.empty())
				return;
			task = std::move(task_queue_.front());
			task_queue_.pop();
		}

		if (task.work) {
			task.work(*db_);
		}

		if (task.on_complete) {
			std::lock_guard<std::mutex> lock(queue_mutex_);
			completed_callbacks_.emplace(std::move(task.on_complete));
		}
	}
}

void AsyncDbWorker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_flush_callbacks"), &AsyncDbWorker::_flush_callbacks);
}
