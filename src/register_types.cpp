#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "gdsqlite/register_types.hpp"
#include "gdsqlite/vfs/gdsqlite_vfs.hpp"
#include "core/stream/stream_manager.h"
#include "core/stream/stream_object.h"
#include "core/stream/stream_world_probe.h"
#include "core/components/async_db_worker.h"

using namespace godot;

void initialize_gdextension_types(ModuleInitializationLevel p_level) {
	initialize_sqlite_module(p_level);

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	sqlite3_vfs_register(gdsqlite_vfs(), 0);

	GDREGISTER_CLASS(StreamObjectNode);
	GDREGISTER_CLASS(StreamManager);
	GDREGISTER_CLASS(StreamWorldProbe);
	GDREGISTER_ABSTRACT_CLASS(AsyncDbWorker);
}

void uninitialize_gdextension_types(ModuleInitializationLevel p_level) {
	uninitialize_sqlite_module(p_level);

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
// Initialization
GDExtensionBool GDE_EXPORT stream_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_gdextension_types);
	init_obj.register_terminator(uninitialize_gdextension_types);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}