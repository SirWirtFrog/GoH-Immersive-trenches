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

("it_trench_ground" name(it_trench_straight) preview(it_trench_straight_preview) entity(it_trench_straight_with_exits) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_straight_short) preview(it_trench_straight_short_preview) entity(it_trench_straight_short) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_turn) preview(it_trench_turn_preview) entity(it_trench_turn) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_turn2) preview(it_trench_turn2_preview) entity(it_trench_turn2) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_turn3) preview(it_trench_turn3_preview) entity(it_trench_turn3) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_turn_90) preview(it_trench_turn_90_preview) entity(it_trench_turn_90) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_intersection) preview(it_trench_intersection_preview) entity(it_trench_intersection) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(180.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_end) preview(it_trench_end_preview) entity(it_trench_end) animation(dig_pickaxe_01) time(9) rotation(90.0) install_rotation(90.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_exit) preview(it_trench_exit_preview) entity(it_trench_exit) animation(dig_pickaxe_01) time(9) rotation(180.0) install_rotation(270.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_bridge) preview(it_trench_bridge_preview) entity(it_trench_bridge) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_fire_position1) preview(it_trench_fire_position1_preview) entity(it_trench_fire_position1) animation(dig_pickaxe_01) time(9) rotation(270.0) install_rotation(270.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_fire_position2) preview(it_trench_fire_position2_preview) entity(it_trench_fire_position2) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_mg_nest) preview(it_trench_mg_nest_preview) entity(it_trench_mg_nest) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_emplacement) preview(it_trench_emplacement_preview) entity(it_trench_emplacement) animation(dig_pickaxe_01) time(9) rotation(90.0) install_rotation(90.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_pillbox) preview(it_trench_pillbox_preview) entity(it_trench_pillbox) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
("it_trench_ground" name(it_trench_depot) preview(it_trench_depot_preview) entity(it_trench_depot) animation(dig_pickaxe_01) time(9) rotation(0.0) install_rotation(0.0) spacing(180) cost(20) item(trench) restrictions("bridge bridge_wood bridge_steel stone rock shallow_water ice water"))
