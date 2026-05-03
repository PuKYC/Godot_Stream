#include "async_db_worker.h"

using namespace godot;

AsyncDbWorker::AsyncDbWorker(const String& db_path) {
    db_ = std::make_unique<StreamSqliteDB>(db_path.utf8().get_data(), false);
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

void AsyncDbWorker::flush_callbacks() {
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

void AsyncDbWorker::thread_loop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return stop_ || !task_queue_.empty(); });
            if (stop_ && task_queue_.empty()) return;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        try {
            if (task.work) {
                task.work(*db_);
            }
        } catch (const std::exception& e) {
            // 可通过 on_complete 传递错误
        }

        if (task.on_complete) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            completed_callbacks_.emplace(std::move(task.on_complete));
        }
    }
}