#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>

#include "godot-sqlist/src/sqlite/sqlite3.h"

using namespace godot;

class AsyncDbWorker : public RefCounted {
    GDCLASS(AsyncDbWorker, RefCounted)

public:
    using Callback = std::function<void()>; // 主线程回调

    // 任务：在后台执行的函数 + 完成后在主线程调用的回调
    struct Task {
        std::function<void(sqlite3&)> work; // 参数是 SQLite 实例（注意线程安全）
        Callback on_complete;
    };

    AsyncDbWorker(const String& db_path);
    ~AsyncDbWorker();

    // 提交异步任务（主线程调用）
    void push_task(Task task);

    // 处理已完成任务的主线程回调（需在 _process 或 _physics_process 中调用）
    void flush_callbacks();

private:
    void thread_loop();

    std::thread worker;
    std::queue<Task> task_queue;
    std::queue<Callback> completed_callbacks;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop {false};

};