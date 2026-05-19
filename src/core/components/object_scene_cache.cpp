#include "object_scene_cache.h"
#include <godot_cpp/classes/resource_loader.hpp>

using namespace godot;

ObjectSceneCache::ObjectSceneCache(size_t scene_cap, size_t node_cap) :
		node_cache_(node_cap, caches::LRUCachePolicy<uuids::uuid>{}, [this](uuids::uuid, Node *node) { this->evict_node(node); }),
		scene_cache_(scene_cap, caches::LRUCachePolicy<uuids::uuid>{}) {
	node_cache_capacity = node_cap;
	scene_cache_capacity = scene_cap;
	node_evict_ = [](Node *node) {
		UtilityFunctions::print("[ObjectSceneCache] Evict node: " + node->get_name());
		if (node)
			node->queue_free();
	};
}

void ObjectSceneCache::set_node_evict_callback(NodeEvictCallback callback) {
	node_evict_ = std::move(callback);
}

size_t ObjectSceneCache::node_cache_size() const noexcept {
	return node_cache_.Size();
}

size_t ObjectSceneCache::scene_cache_size() const noexcept {
	return scene_cache_.Size();
}

Node *ObjectSceneCache::acquire(const uuids::uuid &uuid, const String &scene_path) {
	// 节点缓存优先
	auto result = node_cache_.TryGet(uuid); // 返回 std::pair<Node*, bool>
	if (result.second) { // 如果找到了节点
		UtilityFunctions::print("[ObjectSceneCache] find node cache: ", uuids::to_string(uuid).c_str());
		return result.first; // 返回节点指针
	}

	// 场景资源缓存命中
	Ref<PackedScene> scene = get_scene(uuid);
	if (scene.is_valid()) {
		UtilityFunctions::print("[ObjectSceneCache] find scene cache: ", uuids::to_string(uuid).c_str());
		return scene->instantiate(); // 同步实例化，开销很小
	}

	// 资源未缓存：尝试同步加载（降级方案）或返回 nullptr 让异步接管
	// 这里可根据策略选择同步加载，但为了非阻塞返回 nullptr
	return nullptr;
}

void ObjectSceneCache::release(const uuids::uuid &uuid, Node *node) {
	if (!node)
		return;

	auto old = node_cache_.TryGet(uuid);

	node_cache_.Put(uuid, node);
}

// 异步加载管理
bool ObjectSceneCache::request_scene(const uuids::uuid &uuid, const String &scene_path) {
	// 如果目标文件损坏
	if (remove_loading_set_.count(uuid)) {
		remove_loading_set_.erase(uuid);
		return false;
	}

	// 已缓存或正在加载则忽略
	if (get_scene(uuid).is_valid() || loading_set_.count(uuid)) {
		return true;
	}

	// 发起后台加载
	ResourceLoader *loader = ResourceLoader::get_singleton();
	Error err = loader->load_threaded_request(scene_path);
	if (err == OK) {
		loading_requests_.push_back({ uuid, scene_path });
		loading_set_.insert(uuid);

		return true;
	}

	print_error("[ObjectSceneCache] Failed to load scene, code: " + err);
	return false;
}

void ObjectSceneCache::update() {
	ResourceLoader *loader = ResourceLoader::get_singleton();
	auto it = loading_requests_.begin();
	while (it != loading_requests_.end()) {
		ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status(it->path);
		if (status == ResourceLoader::ThreadLoadStatus::THREAD_LOAD_LOADED) {
			Ref<PackedScene> scene = loader->load_threaded_get(it->path);
			if (scene.is_valid()) {
				store_scene(it->uuid, scene);
			}
			loading_set_.erase(it->uuid);
			it = loading_requests_.erase(it);
		} else if (status == ResourceLoader::ThreadLoadStatus::THREAD_LOAD_FAILED) {
			loading_set_.erase(it->uuid);
			it = loading_requests_.erase(it);
			remove_loading_set_.insert(it->uuid);
			// 可选错误日志
		} else {
			++it;
		}
	}
}

bool ObjectSceneCache::is_loading(const uuids::uuid &uuid) const {
	return loading_set_.count(uuid);
}

// 场景资源缓存手动控制
void ObjectSceneCache::store_scene(const uuids::uuid &uuid, const Ref<PackedScene> &scene) {
	scene_cache_.Put(uuid, scene);
}

Ref<PackedScene> ObjectSceneCache::get_scene(const uuids::uuid &uuid) const {
	return scene_cache_.TryGet(uuid).first;
}

void ObjectSceneCache::remove_scene(const uuids::uuid &uuid) {
	scene_cache_.Remove(uuid);
}

Ref<PackedScene> ObjectSceneCache::load_scene_from_disk_sync(const String &path) {
	return ResourceLoader::get_singleton()->load(path);
}

void ObjectSceneCache::evict_node(Node *node) {
	node_evict_(node);
}
