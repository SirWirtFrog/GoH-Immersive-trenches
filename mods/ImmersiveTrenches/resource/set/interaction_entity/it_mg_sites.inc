; Spawn once for the finished entity, never for its placement preview.

; The fortification actor remains alive as the native controller of its
; completed level. Hide the crate, rather than deleting its owner.
{"it_mg_build_site"
	{on "it_mg_site_completed"
		{tags add "it_mg_built"}
		{bone hide "it_mg_crate"}
		{volumes "body" disable all}
	}
	{on spawn
		{if tagged "it_mg_built"
			{call "it_mg_site_completed"}
		}
	}
}

{"it_mg_nest_ger"
	{on spawn
		{if not tagged "it_mg_site_created"
			{tags add "it_mg_site_created"}
			{delay 0.2
				{spawn "it_mg_site_ger" "it_mg_mount" x}
			}
		}
	}
}

{"it_mg_nest_rus"
	{on spawn
		{if not tagged "it_mg_site_created"
			{tags add "it_mg_site_created"}
			{delay 0.2
				{spawn "it_mg_site_rus" "it_mg_mount_maxim" x}
			}
		}
	}
}

{"it_mg_nest_fin"
	{on spawn
		{if not tagged "it_mg_site_created"
			{tags add "it_mg_site_created"}
			{delay 0.2
				{spawn "it_mg_site_fin" "it_mg_mount" x}
			}
		}
	}
}

{"it_mg_nest_usa"
	{on spawn
		{if not tagged "it_mg_site_created"
			{tags add "it_mg_site_created"}
			{delay 0.2
				{spawn "it_mg_site_usa" "it_mg_mount" x}
			}
		}
	}
}

{"it_mg_nest_eng"
	{on spawn
		{if not tagged "it_mg_site_created"
			{tags add "it_mg_site_created"}
			{delay 0.2
				{spawn "it_mg_site_eng" "it_mg_mount" x}
			}
		}
	}
}
