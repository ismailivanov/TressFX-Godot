extends Node3D
## RatBoy demo scene, mirrors upstream TressFXSample::LoadScene.
## Flags (after `--`): --no-fur, --no-mohawk, --no-collision (keep SDFs, skip the response),
## --no-sdf (drop the collision meshes entirely), --hide-hair (simulate only), --no-anim, --sdf-check,
## --bench (run with `--disable-vsync`, prints average FPS over frames 60-300 and quits).

@onready var anim: AnimationPlayer = $Ratboy/AnimationPlayer

var _bench := false
var _frames := 0
var _bench_t0 := 0
var _gpu_ms := 0.0
var _cpu_ms := 0.0
var _jitter_prev := PackedFloat32Array()
var _jitter_prev2 := PackedFloat32Array()
var _jitter_samples := 0
var _jitter_moving := 0.0
var _jitter_max := 0.0
var _jitter_worst := 0
var _jitter_by_index := PackedInt32Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])


# --jitter-check: reads the fur positions back over frames 150-200 (wind forced to 0) and
# reports how many vertices jitter (per-frame acceleration > 1 mm). Add --no-anim for rest.
func _jitter_check() -> void:
	if "--jitter-check" not in OS.get_cmdline_user_args():
		return
	if _frames == 1:
		for arg in OS.get_cmdline_user_args():
			if arg.begins_with("--sdf-cells="):
				$CollisionBody.num_cells_x = int(arg.get_slice("=", 1))
			elif arg.begins_with("--push-limit="):
				$CollisionBody.push_limit = float(arg.get_slice("=", 1))
		for hair in [$Mohawk, $Fur]:
			hair.wind_magnitude = 0.0
			for arg in OS.get_cmdline_user_args():
				if arg == "--amd-sim":
					hair.gravity = 0.09
				elif arg.begins_with("--damping="):
					hair.damping = float(arg.get_slice("=", 1))
				elif arg.begins_with("--gravity="):
					hair.gravity = float(arg.get_slice("=", 1))
	if _frames < 150 or _frames > 200:
		return
	var cur: PackedFloat32Array = $Fur.get_captured_positions()
	$Fur.capture_positions()
	var vps: int = $Fur.get_vertices_per_strand()
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--track=") and not cur.is_empty() and _frames < 162:
			var i := int(arg.get_slice("=", 1))
			print("  frame %d vertex %d: %s" % [_frames, i, Vector3(cur[i * 4], cur[i * 4 + 1], cur[i * 4 + 2])])
	# Jitter = second difference (acceleration) per frame: smooth animation is small, a vertex
	# flipping back and forth is large.
	if not _jitter_prev2.is_empty() and cur.size() == _jitter_prev.size() and cur.size() == _jitter_prev2.size():
		var jittering := 0
		var n := cur.size() / 4
		for i in n:
			var o := i * 4
			var acc := Vector3(cur[o] - 2.0 * _jitter_prev[o] + _jitter_prev2[o],
				cur[o + 1] - 2.0 * _jitter_prev[o + 1] + _jitter_prev2[o + 1],
				cur[o + 2] - 2.0 * _jitter_prev[o + 2] + _jitter_prev2[o + 2]).length()
			if acc > _jitter_max:
				_jitter_max = acc
				_jitter_worst = i
			if acc > 0.001:
				jittering += 1
				_jitter_by_index[i % vps] += 1
		_jitter_moving += jittering / float(n)
		_jitter_samples += 1
	_jitter_prev2 = _jitter_prev
	_jitter_prev = cur
	if _frames == 200:
		print("JITTER: %.2f%% of fur vertices accelerate > 1 mm per frame², max %.1f mm" % [
			_jitter_moving / _jitter_samples * 100.0, _jitter_max * 1000.0])
		print("  counts by vertex index in strand: ", _jitter_by_index)
		var st := _jitter_worst / vps
		print("  worst vertex: strand %d (guide=%s) index %d pos %s" % [st, (st % ($Fur.num_follow_hairs + 1)) == 0,
			_jitter_worst % vps, Vector3(cur[_jitter_worst * 4], cur[_jitter_worst * 4 + 1], cur[_jitter_worst * 4 + 2])])
		get_tree().quit()


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	if "--no-fur" in args:
		$Fur.queue_free()
	if "--no-mohawk" in args:
		$Mohawk.queue_free()
	if "--no-collision" in args or "--no-sdf" in args:
		for hair in [$Mohawk, $Fur]:
			hair.collision_meshes.clear()
	if "--no-sdf" in args:
		for mesh in [$CollisionBody, $CollisionLeftHand, $CollisionRightHand]:
			mesh.queue_free()
	if "--gpu-timing" in args:
		$StatsLayer/Stats.measure_gpu = true
	if "--hide-hair" in args: # simulate but don't draw
		for hair in [$Mohawk, $Fur]:
			hair.visible = false
	for arg in args:
		if arg.begins_with("--wind="):
			for hair in [$Mohawk, $Fur]:
				hair.wind_magnitude = float(arg.get_slice("=", 1))
	_bench = "--bench" in args
	if _bench:
		RenderingServer.viewport_set_measure_render_time(get_viewport().get_viewport_rid(), true)
	$Camera3D.look_at_from_position(Vector3(0.6, 1.1, 1.2), Vector3(0, 0.8, 0.1))
	if "--no-anim" in args:
		anim.stop()
		($Ratboy/Skeleton3D as Skeleton3D).reset_bone_poses()
	else:
		anim.play("All Animations")
		anim.seek(2.3) # upstream startOffset


func _process(_delta: float) -> void:
	_frames += 1
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--shot=") and _frames in [60, 120, 180, 240]:
			get_viewport().get_texture().get_image().save_png("%s/shot_%d.png" % [arg.get_slice("=", 1), _frames])
			if _frames == 240:
				get_tree().quit()
	if _frames == 10 and "--sdf-check" in OS.get_cmdline_user_args():
		var skel: Skeleton3D = $Ratboy/Skeleton3D
		var head := skel.global_transform * skel.get_bone_global_pose(skel.find_bone("frenchHornMonster_head_JNT")).origin
		$CollisionBody.debug_print_distances(PackedVector3Array([head, head + Vector3.UP * 0.3, head + Vector3.UP * 1.0]))
	_jitter_check()
	if not _bench:
		return
	var vp := get_viewport().get_viewport_rid()
	if _frames == 60:
		_bench_t0 = Time.get_ticks_usec()
	if _frames > 60:
		_gpu_ms += RenderingServer.viewport_get_measured_render_time_gpu(vp)
		_cpu_ms += RenderingServer.viewport_get_measured_render_time_cpu(vp)
	if _frames == 300:
		var frame_ms := (Time.get_ticks_usec() - _bench_t0) / 240.0 / 1000.0
		print("BENCH: %.2f ms/frame (%.0f fps) | viewport draw gpu %.2f ms, cpu %.2f ms" % [
			frame_ms, 1000.0 / frame_ms, _gpu_ms / 240.0, _cpu_ms / 240.0])
		get_tree().quit()
