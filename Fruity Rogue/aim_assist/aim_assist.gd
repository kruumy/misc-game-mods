extends Node

const TRIGGER_THRESHOLD := 0.3


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_PAUSABLE
	process_priority = 1000 # run after the hooks' own _process so our raycast direction wins
	print("[AimAssist] loaded")


func _process(_delta: float) -> void:
	var global := get_node_or_null("/root/Global")
	if global == null or not is_instance_valid(global.player) or global.player.dead:
		return
	_aim_hooks_along_movement(global.player)
	if not _trigger_held():
		return
	if Input.get_vector("r_joystick_left", "r_joystick_right", "r_joystick_up", "r_joystick_down") != Vector2.ZERO:
		return
	var target = _nearest_enemy(global.player.global_position)
	if target != null:
		get_viewport().warp_mouse(target.get_global_transform_with_canvas().origin)


# With no movement input the hooks keep their default aim direction.
func _aim_hooks_along_movement(player: Node) -> void:
	var dir := Input.get_vector("left", "right", "up", "down").normalized()
	if dir == Vector2.ZERO:
		return
	for hook in player.find_children("*", "Node2D"):
		if not "VEL_PUSH" in hook or not hook.has_node("Raycast"): # grappling_hook.gd / tp_hook.gd
			continue
		var raycast: RayCast2D = hook.get_node("Raycast")
		raycast.target_position = dir * raycast.target_position.length()
		var helper = hook.get_node_or_null("Helper")
		if helper:
			helper.position = raycast.target_position


func _trigger_held() -> bool:
	for device in Input.get_connected_joypads():
		if Input.get_joy_axis(device, JOY_AXIS_TRIGGER_RIGHT) > TRIGGER_THRESHOLD:
			return true
	return false


func _nearest_enemy(from: Vector2) -> Node2D:
	var screen := get_viewport().get_visible_rect()
	var best: Node2D = null
	var best_dist := INF
	for enemy in get_tree().get_nodes_in_group("Ennemy"):
		if not enemy is Node2D or not enemy.is_visible_in_tree() or enemy.get("is_dead"):
			continue
		if not screen.has_point(enemy.get_global_transform_with_canvas().origin):
			continue
		var dist := from.distance_squared_to(enemy.global_position)
		if dist < best_dist:
			best_dist = dist
			best = enemy
	return best
