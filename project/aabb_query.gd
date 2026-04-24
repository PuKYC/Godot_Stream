@tool
extends Node3D

@export var stream_path :StreamManager
@export var size : float
var dab := false


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.

func aabb_from_center_and_half_size(center: Vector3, lsize: Vector3) -> AABB:
	return AABB(center - lsize/2, lsize)

# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	if dab :
		stream_path.aabb_query(aabb_from_center_and_half_size(global_position, Vector3(size,size,size)))
		
	dab = !dab
	pass
