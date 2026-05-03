# Godot Stream (GDExtension)

A C++ GDExtension for [Godot Engine](https://godotengine.org) that implements a streaming world management system.  
It efficiently loads and unloads scene objects based on **AABB (Axis-Aligned Bounding Box) queries** or visibility, backed by an **SQLite database** and an asynchronous worker thread.

## Features

- **AABB‑based spatial queries**  
  Load only the objects that intersect a given bounding box – ideal for chunk‑based or open‑world streaming.

- **UUID‑identified persistent objects**  
  Every streamable object is identified by a `uuid` and can be stored in a database together with its world AABB and parent‑child relationships.

- **Automatic scene serialization & caching**  
  When unloaded, node hierarchies are saved as `.tscn` files into a dedicated directory and optionally kept in an in‑memory cache for faster re‑instantiation.

- **Asynchronous database operations**  
  Heavy SQLite reads/writes run on a separate thread, keeping the main thread responsive.

- **Parent‑child hierarchy management**  
  Loading/unloading respects the object tree; child objects are automatically loaded when the parent enters the scene and unloaded together with it.

- **Editor integration ready**  
  `StreamObjectNode` exposes configurable `aabb_sources` paths so that designers can assign which visual meshes contribute to the bounding box.  
  The `StreamManager` can be placed as a Node3D in the scene tree and configured via the inspector.

- **Batch update and removal**  
  Changes to AABB or parent relationships are collected over a frame and flushed to the database in a single batch to reduce overhead.

## Requirements

- Godot 4.x (built with GDExtension support)
- C++17 compatible compiler
- [stduuid](https://github.com/mariusbancila/stduuid) (header‑only UUID library)
- [SQLite3](https://www.sqlite.org/) (amalgamation or system library)
- `godot-cpp` (same version as your Godot 4.x build)


## Building

1. Clone this repository, including submodules:
   ```bash
   git clone --recurse-submodules <repo-url> stream_manager
   cd stream_manager
```

1. Make sure you have Godot 4.x and its godot-cpp bindings.
      If you placed godot-cpp manually, adjust SConstruct accordingly.
2. Build with SCons:
   ```bash
   scons platform=<your_platform>
   # Example: scons platform=linux
   ```
3. Copy or symlink the resulting .gdextension file and the compiled libraries into your Godot project’s res:// directory.
      See the official GDExtension documentation for detailed steps.

Usage

1. Register the extension in your project

Make sure your Godot project contains the .gdextension file and the binaries.

2. Add a StreamManager to your scene

· In the editor, add a StreamManager node to your main scene.
· Set the Database Path property to a writable file location, e.g.:
  ```
  res://world_stream.db
  ```
  The manager will automatically create the database and an associated directory for .tscn files (e.g. res://.world_stream/).

3. Create streamable objects

Design your object as a StreamObjectNode scene:

· Add StreamObjectNode as the root node.
· Add child visual nodes (like MeshInstance3D).
· In the aabb_sources array property, add the NodePaths of the children that contribute to the bounding box (e.g. ./MeshInstance3D).
· The uuid and parent_uuid are automatically set during loading, so leave them empty in the saved scene.

4. Load objects dynamically

From GDScript or C++, call query_aabb(aabb) on the StreamManager.
The manager will:

· Query the database for objects whose bounding box intersects the given AABB.
· Unload objects that are no longer relevant.
· Load new objects by instantiating the saved .tscn files and adding them as children of the manager.

```gdscript
# Example GDScript
extends Node3D

func _ready():
    var stream_mgr = get_node("/root/Main/StreamManager")
    # Request objects inside a 100x100x100 box around origin
    stream_mgr.query_aabb(AABB(Vector3(0,0,0), Vector3(100,100,100)))
```

5. Automatic object lifecycle

· When a StreamObjectNode enters the scene tree, it reports its UUID to the manager.
· When it leaves (e.g. manually removed or unloaded), the manager saves its current state to a .tscn file and optionally caches it.
· AABB changes are batched and flushed to the database every frame.

API Overview

StreamManager (Node3D)

Method / Property Description
set_database_path(path: String) Set the SQLite database file path.
get_database_path() -> String Returns the current database path.
query_aabb(aabb: AABB) Initiate an asynchronous AABB query. The result is used to load/unload objects.

Signals are connected automatically. You should not need to call the internal methods (add_object, remove_object, etc.) unless you are extending the system.

StreamObjectNode (Node3D)

Property Description
uuid (read‑only) String representation of the object’s UUID. Set internally.
parent_uuid (read‑only) UUID of the parent stream object (for hierarchy).
aabb_sources Array of NodePaths to VisualInstance3D children used to calculate the total AABB.

License

This project is licensed under the GNU General Public License v3.0 or later (GPL‑3.0‑or‑later).
