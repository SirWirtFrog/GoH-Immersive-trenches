{"it_trench_upgradeable"
	{on spawn
		{call "it_apply_trench_upgrade"}
	}
	{on "it_finish_trench_upgrade"
		{if not "it_trench_reinforced"
			{set "it_trench_reinforced" 1}
			{call "it_apply_trench_upgrade"}
		}
	}
	{on "it_apply_trench_upgrade"
		{if "it_trench_reinforced"
			{bone show "wood"}
			{volumes "it_trench_upgrade_target" disable touch}
		else
			{bone hide "wood"}
		}
		{update_visibility_patch}
	}
}
