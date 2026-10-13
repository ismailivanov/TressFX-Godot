extends SceneTree
# Loads the project with the TressFX GDExtension listed in .godot/extension_list.cfg and
# checks that its classes were registered. Exit code 0 = loaded, 1 = not loaded.
#   godot --headless --path <project> --script res://smoke.gd

func _initialize() -> void:
	var ok := true
	for cls in [&"TressFXHair", &"TressFXCollisionMesh", &"TressFXStats"]:
		var exists := ClassDB.class_exists(cls)
		print("TRESSFX_SMOKE class_exists(%s)=%s" % [cls, exists])
		ok = ok and exists
	if ok:
		var hair: Node = ClassDB.instantiate(&"TressFXHair")
		print("TRESSFX_SMOKE instantiate(TressFXHair)=%s" % hair.get_class())
		hair.free()
	print("TRESSFX_SMOKE %s" % ("PASS" if ok else "FAIL"))
	quit(0 if ok else 1)
