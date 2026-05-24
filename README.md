# Godot Stream (GDExtension)

A C++ GDExtension for [Godot Engine](https://godotengine.org) that implements a streaming world management system.
It efficiently loads and unloads scene objects based on **AABB (Axis-Aligned Bounding Box) queries**, backed by an **SQLite database**, an **asynchronous worker thread**, and a **chunk-based spatial index**.

## Features

- **AABB‑based spatial queries**
  Load only the objects that intersect a given bounding box – ideal for chunk‑based or open‑world streaming.

- **StreamWorldProbe – automatic streaming**
  Attach `StreamWorldProbe` nodes (e.g. to the player camera) and the `StreamManager` automatically polls their AABBs every few frames to determine which objects to load/unload. No manual scripting required.

- **Chunk‑based spatial partitioning**
  Objects are indexed by a hierarchical chunk system (`level` / `x` / `y` / `z`) computed from their world AABB. The chunk level doubles in size every two levels, providing efficient spatial grouping from fine‑grained (1 unit) to coarse (millions of units).

- **UUID‑identified persistent objects**
  Every streamable object is identified by a `uuid` and stored in the database together with its world AABB and parent‑child relationships.

- **Automatic scene serialization & dual LRU caching**
  When unloaded, node hierarchies are saved as `.tscn` files into a dedicated directory. Two independent LRU caches (node instances and `PackedScene` resources, with configurable capacities) keep hot objects in memory for instant re‑instantiation. Scene resources are loaded asynchronously via Godot's `ResourceLoader`.

- **Asynchronous database operations**
  Heavy SQLite reads/writes run on a separate thread, keeping the main thread responsive. Callbacks are dispatched on the main thread during `_process`.

- **Parent‑child hierarchy management**
  Loading/unloading respects the object tree; child objects are automatically loaded when the parent enters the scene and unloaded together with it.

- **Editor integration ready**
  `StreamObjectNode` exposes configurable `aabb_sources` paths so that designers can assign which visual meshes contribute to the bounding box.
  The `StreamManager` can be placed as a Node3D in the scene tree and configured via the inspector.

- **Batch update and removal**
  Changes to AABB or parent relationships are collected over a frame and flushed to the database in a single batch to reduce overhead.

## Requirements

- Godot 4.6 (built with GDExtension support)
- C++20 compatible compiler
- [stduuid](https://github.com/mariusbancila/stduuid) (header‑only UUID library) MIT
- [SQLite3](https://www.sqlite.org/) (amalgamation — bundled in `src/gdsqlite/sqlite/`)
- [ankerl::unordered_dense](https://github.com/martinus/unordered_dense) (A fast & densely stored hashmap) MIT
- `godot-cpp`

### Built‑in dependencies (in‑tree)

| Directory | Description |
|-----------|-------------|
| `src/gdsqlite/` | Godot‑native SQLite wrapper with custom VFS. Bundles SQLite3 amalgamation (`sqlite3.c`/`sqlite3.h`). |
| `src/cpp_caches/` | Generic cache policy library (LRU / LFU / FIFO). |
| `src/ankerl/` | `ankerl::unordered_dense` – high‑performance hashmap. |
| `godot-cpp/` | Godot C++ bindings (git submodule). |

## Building

1. Clone this repository, including submodules:
   ```bash
   git clone --recurse-submodules <repo-url> stream_manager
   cd stream_manager
   ```

2. Make sure you have Godot 4.x and its `godot-cpp` bindings.
   If you placed `godot-cpp` manually, adjust `SConstruct` accordingly.

3. Build with SCons:
   ```bash
   scons platform=<your_platform>
   # Example: scons platform=linux
   ```

4. Copy or symlink the resulting `.gdextension` file and the compiled libraries into your Godot project's `res://` directory.
   See the official GDExtension documentation for detailed steps.

## Usage

### 1. Register the extension in your project

Make sure your Godot project contains the `.gdextension` file and the binaries.

### 2. Add a StreamManager to your scene

- In the editor, add a `StreamManager` node to your main scene.
- Set the **Database Path** property to a writable file location, e.g.:
  ```
  res://world_stream.db
  ```
  The manager will automatically create the database and an associated directory for `.tscn` files (e.g. `res://.world_stream/`).

### 3. Create streamable objects

Design your object as a `StreamObjectNode` scene:

- Add `StreamObjectNode` as the root node.
- Add child visual nodes (like `MeshInstance3D`).
- In the **aabb_sources** array property, add the `NodePath`s of the children that contribute to the bounding box (e.g. `./MeshInstance3D`).
- The `uuid` and `parent_uuid` are automatically set during loading, so leave them empty in the saved scene.

### 4. Choose a loading strategy

#### Automatic mode (recommended) – StreamWorldProbe

Place one or more `StreamWorldProbe` nodes in the scene (e.g. as a child of the player camera). The probe is a lightweight Node3D with a configurable AABB:

- **AABB**: the bounding box to query around the probe.
- **Stream Manager Path**: NodePath to the `StreamManager` that should handle this probe.

When the probe enters the tree, it registers itself with the `StreamManager`. The manager automatically polls all registered probes every 3 frames and loads/unloads objects accordingly. When the probe leaves the tree (or is hidden), it deregisters.

```gdscript
# Attach a probe to the player camera
var probe = StreamWorldProbe.new()
probe.aabb = AABB(Vector3(0, 0, 0), Vector3(200, 200, 200))
probe.stream_manager_path = NodePath("/root/Main/StreamManager")
camera.add_child(probe)
```

### 5. Automatic object lifecycle

- When a `StreamObjectNode` enters the scene tree, it reports its UUID to the manager. If no UUID is assigned yet, the manager generates one automatically.
- When it leaves (e.g. manually removed or unloaded), the manager saves its current state to a `.tscn` file and caches it.
- AABB changes are batched and flushed to the database every frame.
- Parent‑child hierarchy is respected: unloading a parent automatically unloads all descendants.

## API Overview

### StreamManager (Node3D)

| Method / Property | Description |
|-------------------|-------------|
| `set_database_path(path: String)` | Set the SQLite database file path. |
| `get_database_path() -> String` | Returns the current database path. |
| `add_object(node: StreamObjectNode)` | Register a streamable object. |
| `remove_object(uuid: String)` | Unload and remove an object and all its descendants. |
| `update_object(node: StreamObjectNode)` | Update an object's metadata (AABB, parent). |

**Signals** — connected automatically. You should not need to call internal methods unless extending the system.

### StreamObjectNode (Node3D)

| Property | Type | Description |
|----------|------|-------------|
| `uuid` | String | String representation of the object's UUID. Set internally. |
| `parent_uuid` | String | UUID of the parent stream object (for hierarchy). |
| `aabb_sources` | Array[NodePath] | NodePaths to VisualInstance3D children used to calculate the total AABB. |
| `aabb` | AABB (read‑only) | The computed world‑space bounding box from `aabb_sources`. |

**Signals:**

| Signal | Description |
|--------|-------------|
| `object_aabb_changed` | Emitted when the computed AABB changes. |

### StreamWorldProbe (Node3D)

| Property | Type | Description |
|----------|------|-------------|
| `aabb` | AABB | The bounding box to query around this probe. |
| `stream_manager_path` | NodePath | Path to the `StreamManager` that handles this probe. |

When a `StreamWorldProbe` enters the scene tree, it connects to the `StreamManager` and its AABB is automatically polled for streaming decisions. When it exits the tree (or becomes hidden), it deregisters.

## Project structure

```
Godot_Stream/
├── src/
│   ├── core/
│   │   ├── stream/           # Public API nodes
│   │   │   ├── stream_manager    # Manager node (Node3D)
│   │   │   ├── stream_object     # Streamable object node (Node3D)
│   │   │   └── stream_world_probe # Auto-streaming probe node (Node3D)
│   │   └── components/       # Infrastructure
│   │       ├── async_db_worker   # Async SQLite worker thread
│   │       ├── object_scene_cache # Dual LRU cache (node + scene)
│   │       ├── stream_sqlite_db  # Spatial query + chunk index
│   │       ├── sqlite_db         # Low-level SQLite wrapper
│   │       ├── chunk             # Hierarchical spatial chunk
│   │       └── object_data       # Object metadata struct
│   ├── gdsqlite/             # SQLite integration + VFS
│   │   ├── sqlite/           # SQLite3 amalgamation
│   │   └── vfs/              # Custom virtual file system
│   ├── cpp_caches/           # Cache policy library (LRU/LFU/FIFO)
│   ├── ankerl/               # ankerl::unordered_dense
│   └── uuid.h                # stduuid header
├── godot-cpp/                # Godot C++ bindings (submodule)
├── project/                  # Example Godot project
├── build_profile.json        # Class enable/disable profile
├── SConstruct                # SCons build
└── CMakeLists.txt            # CMake build (alternative)
```

## License

MIT
