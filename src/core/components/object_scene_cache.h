#pragma once

#include <functional>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/string.hpp>
#include <vector>

#include "core/ankerl.h"
#include "core/caches.h"
#include "stduuid/include/uuid.h"

using namespace godot;

class ObjectSceneCache {
public:
	using NodeEvictCallback = std::function<void(Node *)>;

	explicit ObjectSceneCache(size_t scene_capacity = 16, size_t node_capacity = 32);
	~ObjectSceneCache();

	// 禁止拷贝，允许移动
	ObjectSceneCache(const ObjectSceneCache &) = delete;
	ObjectSceneCache &operator=(const ObjectSceneCache &) = delete;
	ObjectSceneCache(ObjectSceneCache &&) = delete;
	ObjectSceneCache &operator=(ObjectSceneCache &&) = delete;

	// 非阻塞
	// 获取可挂载节点。若所需资源仍在异步加载中，返回 nullptr。
	Node *acquire(const uuids::uuid &uuid, const String &scene_path);

	// 卸载节点到缓存
	// 不会自动更新场景文件缓存
	void release(const uuids::uuid &uuid, Node *node);

	// 发起异步加载请求（若已缓存或正在加载则忽略）
	void request_scene(const uuids::uuid &uuid, const String &scene_path);

	// 每帧调用，轮询异步加载完成状态，将结果存入资源缓存
	void update();

	// 查询某个资源是否正在异步加载
	bool is_loading(const uuids::uuid &uuid) const;

	// 场景资源缓存手动控制
	void store_scene(const uuids::uuid &uuid, const Ref<PackedScene> &scene);
	Ref<PackedScene> get_scene(const uuids::uuid &uuid) const;
	void remove_scene(const uuids::uuid &uuid);

	// 淘汰回调
	void set_node_evict_callback(NodeEvictCallback callback);

	// 缓存清理
	void clear();
	void clear_nodes();
	void clear_scenes();

	size_t node_cache_size() const noexcept;
	size_t scene_cache_size() const noexcept;

private:
	static Ref<PackedScene> load_scene_from_disk_sync(const String &path);
	void evict_node(Node *node);

	size_t node_cache_capacity = 0;
	size_t scene_cache_capacity = 0;

	// 异步加载相关
	struct LoadingEntry {
		uuids::uuid uuid;
		String path;
	};
	std::vector<LoadingEntry> loading_requests_; // 正在进行的异步请求
	a_hashset<uuids::uuid> loading_set_; // 快速查询 UUID 是否在加载中

	lru_cache_t<uuids::uuid, Node *> node_cache_;
	lru_cache_t<uuids::uuid, Ref<PackedScene>> scene_cache_;
	NodeEvictCallback node_evict_;
};