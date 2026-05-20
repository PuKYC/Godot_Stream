@tool
extends Node3D

var p = 0.0


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	p += 1*delta*0.5
	position = Vector3(0,5,sin(p)*80)
	pass
