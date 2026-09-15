extends Node3D
## Bust demos (Sintel, Ruby ponytail) with TressFX hair. No skeleton: the bust node moves
## and the hair follows it as a single rigid bone.
## Idle: a slow look-around. Drag with the left mouse button to turn the bust yourself.
## Flags (after `--`): --still, --no-collision, --gpu-timing, --jitter-check, --bench, --shot=<dir>,
## --yaw=<degrees> (turn the bust, e.g. 90 for a profile view), --cam-dist=<m>, --no-aa (MSAA and
## TAA off)

@onready var bust: Node3D = $Bust

## Idle look-around amplitude (radians) and speed.
@export var idle_yaw := 0.35
@export var idle_speed := 0.5

var _t := 0.0
var _frames := 0
var _drag_yaw := 0.0
var _dragging := false
var _jp := PackedFloat32Array()
var _jp2 := PackedFloat32Array()
var _jsum := 0.0
var _jmax := 0.0
var _jn := 0
var _bench_t0 := 0


func _ready() -> void:
	_drag_yaw = bust.rotation.y # keep the yaw set in the scene
	var args := OS.get_cmdline_user_args()
	if "--no-aa" in args: # the hair does not need MSAA or TAA
		get_viewport().msaa_3d = Viewport.MSAA_DISABLED
		get_viewport().use_taa = false
	if "--no-collision" in args:
		for hair in get_tree().get_nodes_in_group("tressfx_hair"):
			hair.collision_meshes.clear()
	if "--gpu-timing" in args:
		$StatsLayer/Stats.measure_gpu = true
	for arg in args:
		if arg.begins_with("--yaw="):
			_drag_yaw = deg_to_rad(float(arg.get_slice("=", 1)))
			bust.rotation.y = _drag_yaw
		elif arg.begins_with("--cam-dist="): # push the camera back along its view axis (LOD tests)
			var cam: Camera3D = $Camera3D
			cam.position += -cam.global_transform.basis.z * -(float(arg.get_slice("=", 1)) - cam.position.z)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		_dragging = event.pressed
	elif event is InputEventMouseMotion and _dragging:
		_drag_yaw += event.relative.x * 0.01


func _process(delta: float) -> void:
	_frames += 1
	var args := OS.get_cmdline_user_args()
	for arg in args:
		if arg.begins_with("--shot=") and _frames in [60, 120, 180, 240]:
			get_viewport().get_texture().get_image().save_png("%s/shot_%d.png" % [arg.get_slice("=", 1), _frames])
			if _frames == 240:
				get_tree().quit()
	if _frames == 10 and "--sdf-check" in args:
		$Bust/HeadCollider.debug_print_distances(PackedVector3Array([
			Vector3(0, 0.40, 0.0), Vector3(0, 0.40, 0.12), Vector3(0, 0.40, 0.20)]))
	_jitter_check()
	if "--bench" in args: # run with --disable-vsync
		if _frames == 60:
			_bench_t0 = Time.get_ticks_usec()
		elif _frames == 300:
			var ms := (Time.get_ticks_usec() - _bench_t0) / 240.0 / 1000.0
			print("BENCH: %.2f ms/frame (%.0f fps)" % [ms, 1000.0 / ms])
			get_tree().quit()
	if "--still" in args:
		return
	# Gentle idle: slow yaw and a small nod, plus whatever the mouse added.
	_t += delta
	bust.rotation.y = sin(_t * idle_speed) * idle_yaw + _drag_yaw
	bust.rotation.x = sin(_t * idle_speed * 1.6) * 0.06
	bust.position.x = sin(_t * idle_speed * 1.2) * 0.03


# --jitter-check: frames 150-200, share of hair vertices accelerating > 1 mm per frame².
func _jitter_check() -> void:
	if "--jitter-check" not in OS.get_cmdline_user_args() or _frames < 150 or _frames > 200:
		return
	var hair: TressFXHair = get_tree().get_first_node_in_group("tressfx_hair")
	var cur: PackedFloat32Array = hair.get_captured_positions()
	hair.capture_positions()
	if not _jp2.is_empty() and cur.size() == _jp.size() and cur.size() == _jp2.size():
		var n := cur.size() / 4
		var moving := 0
		for i in n:
			var o := i * 4
			var acc := Vector3(cur[o] - 2.0 * _jp[o] + _jp2[o], cur[o + 1] - 2.0 * _jp[o + 1] + _jp2[o + 1],
				cur[o + 2] - 2.0 * _jp[o + 2] + _jp2[o + 2]).length()
			_jmax = maxf(_jmax, acc)
			if acc > 0.001:
				moving += 1
		_jsum += moving / float(n)
		_jn += 1
	_jp2 = _jp
	_jp = cur
	if _frames == 200:
		print("JITTER: %.2f%% of hair vertices accelerate > 1 mm per frame², max %.1f mm" % [
			_jsum / _jn * 100.0, _jmax * 1000.0])
		get_tree().quit()
