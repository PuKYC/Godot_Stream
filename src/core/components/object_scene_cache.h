#pragma once

#include <functional>
#include <vector>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/string.hpp>

#include "src/caches/include/caches/cache.hpp"
#include "src/caches/include/caches/lru_cache_policy.hpp"
#include "src/unordered_dense/include/ankerl/unordered_dense.h"

using namespace godot;

class ObjectSceneCache {
public:
    using NodeEvictCallback = std::function<void(Node*)>;

    explicit ObjectSceneCache(size_t scene_capacity = 16, size_t node_capacity = 32);
    ~ObjectSceneCache();

    // 禁止拷贝，允许移动
    ObjectSceneCache(const ObjectSceneCache&) = delete;
    ObjectSceneCache& operator=(const ObjectSceneCache&) = delete;
    ObjectSceneCache(ObjectSceneCache&&) = default;
    ObjectSceneCache& operator=(ObjectSceneCache&&) = default;

    // 非阻塞
    // 获取可挂载节点。若所需资源仍在异步加载中，返回 nullptr。
    Node* acquire(const String& uuid, const String& scene_path);

    // 卸载节点到缓存
    // 会更新场景文件缓存
    void release(const String& uuid, Node* node);

	// 发起异步加载请求（若已缓存或正在加载则忽略）
    void request_scene(const String& uuid, const String& scene_path);

    // 每帧调用，轮询异步加载完成状态，将结果存入资源缓存
    void update();

    // 查询某个资源是否正在异步加载
    bool is_loading(const String& uuid) const;

    // 场景资源缓存手动控制
    void store_scene(const String& uuid, const Ref<PackedScene>& scene);
    Ref<PackedScene> get_scene(const String& uuid) const;
    void remove_scene(const String& uuid);

    // 淘汰回调
    void set_node_evict_callback(NodeEvictCallback callback);

    // 缓存清理
    void clear();
    void clear_nodes();
    void clear_scenes();

    size_t node_cache_size() const noexcept;
    size_t scene_cache_size() const noexcept;
    void set_node_capacity(size_t cap);
    void set_scene_capacity(size_t cap);

private:
    static Ref<PackedScene> load_scene_from_disk_sync(const String& path);
    void evict_node(Node* node);

    size_t node_cache_max = 0;
    size_t scene_cache_max = 0;

    // 异步加载相关
    struct LoadingEntry {
        String uuid;
        String path;
    };
    std::vector<LoadingEntry> loading_requests_;       // 正在进行的异步请求
    ankerl::unordered_dense::set<String> loading_set_; // 快速查询 UUID 是否在加载中

    // 缓存类型定义
    template <typename Key, typename Value>
    using lru_cache_t = typename caches::fixed_sized_cache<Key, Value, caches::LRUCachePolicy, ankerl::unordered_dense::map<Key, Value>>;

    lru_cache_t<String, Node*> node_cache_;
    lru_cache_t<String, Ref<PackedScene>> scene_cache_;
    NodeEvictCallback node_evict_;
};