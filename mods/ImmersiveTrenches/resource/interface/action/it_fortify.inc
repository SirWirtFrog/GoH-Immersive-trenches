(define "it_trench_ground"
	{Action "%name"
		{Cursor "dig"}
		{Button}
		{fx "flashred"}
		{Entity "%preview"}
		{item "%item"}
		{item_in_box}
		{RequireSelect}
		{NotifySelect}
		{Spacing %spacing}
		{Cost %cost}
		{Rotation %rotation}
		{AbleRotate}
		{AbleEnqueue}
		{IgnoreEntities}
		{target ground}
		{ShowCount}
		{TerrainRestrictions %restrictions}
		{Key "actions/%name"}
		{Instructions
			{bind range 1}
			{disable_action_restart}
			{free_hands}
			{take_item}
			{reserve_hands}
			{animations "%animation %animation" random loop totaltime %time paint 1}
			{install "%entity" %install_rotation}
			{unreserve_hands}
			{free_hands}
			{fsm_request nocheck "hold-nothing"}
		}
	}
)

("it_trench_ground" name(it_trench_dirt_straight) preview(it_trench_dirt_straight_preview) entity(it_trench_dirt_straight) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_straight_short) preview(it_trench_dirt_straight_short_preview) entity(it_trench_dirt_straight_short) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_turn_20) preview(it_trench_dirt_turn_20_preview) entity(it_trench_dirt_turn_20) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_turn_30) preview(it_trench_dirt_turn_30_preview) entity(it_trench_dirt_turn_30) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_turn_60) preview(it_trench_dirt_turn_60_preview) entity(it_trench_dirt_turn_60) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_turn_90) preview(it_trench_dirt_turn_90_preview) entity(it_trench_dirt_turn_90) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_intersection_of_three) preview(it_trench_dirt_intersection_of_three_preview) entity(it_trench_dirt_intersection_of_three) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_intersection_of_four) preview(it_trench_dirt_intersection_of_four_preview) entity(it_trench_dirt_intersection_of_four) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_straight_end) preview(it_trench_dirt_straight_end_preview) entity(it_trench_dirt_straight_end) animation(dig_pickaxe_01) time(9) rotation(90.0) install_rotation(90.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_exit) preview(it_trench_dirt_exit_preview) entity(it_trench_dirt_exit) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(270.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_straight_bridge) preview(it_trench_dirt_straight_bridge_preview) entity(it_trench_dirt_straight_bridge) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_fire_position_through) preview(it_trench_dirt_fire_position_through_preview) entity(it_trench_dirt_fire_position_through) animation(dig_pickaxe_01) time(9) rotation(270.0) install_rotation(270.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_fire_position_dead_end) preview(it_trench_dirt_fire_position_dead_end_preview) entity(it_trench_dirt_fire_position_dead_end) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_mg_nest) preview(it_trench_dirt_mg_nest_preview) entity(it_trench_dirt_mg_nest) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_emplacement) preview(it_trench_dirt_emplacement_preview) entity(it_trench_dirt_emplacement) animation(dig_pickaxe_01) time(9) rotation(90.0) install_rotation(90.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_pillbox) preview(it_trench_dirt_pillbox_preview) entity(it_trench_dirt_pillbox) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_dirt_depot) preview(it_trench_dirt_depot_preview) entity(it_trench_dirt_depot) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
