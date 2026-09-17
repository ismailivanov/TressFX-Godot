extends CanvasLayer
## Demo switcher: a button in the bottom-right corner (or the N key) opens the next demo.
## Add it to a demo scene as a child of the root.

const DEMOS := [
	["Ponytail", "res://addons/tressfx/demo/ponytail.tscn"],
	["Blender hair", "res://addons/tressfx/demo/blender_hair.tscn"],
	["RatBoy", "res://addons/tressfx/demo/main.tscn"],
]

var _next := 0


func _ready() -> void:
	var current: String = owner.scene_file_path if owner != null else ""
	for i in DEMOS.size():
		if DEMOS[i][1] == current:
			_next = (i + 1) % DEMOS.size()
	var button := Button.new()
	button.text = "Next demo: %s  [N]" % DEMOS[_next][0]
	button.focus_mode = Control.FOCUS_NONE
	button.pressed.connect(_open_next)
	add_child(button)
	button.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT, Control.PRESET_MODE_MINSIZE, 16)
	button.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	button.grow_vertical = Control.GROW_DIRECTION_BEGIN


func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key != null and key.pressed and not key.echo and key.keycode == KEY_N:
		_open_next()


func _open_next() -> void:
	get_tree().change_scene_to_file(DEMOS[_next][1])
