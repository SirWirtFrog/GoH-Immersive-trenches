; Applied only to trench-specific copies of the stock MG definitions.
; Preserve gunner/commander, weapons, ammunition and damage properties.
{props "-carried"}
{placer
	{place "driver1" remove}
	{place "driver2" remove}
}
{extender "cannon"
	{animation
		{switch ""}
		{move "" ""}
	}
	{move {workers clear} {obstacle "open"}}
	{trailing {workers clear}}
	{rotate_ik_Limit 0}
}
; Override speed directly: mobility_cannon(speed(0)) divides by speed.
{Chassis "track"
	{Locomotion
		{Slow 0}
		{Normal 0}
		{Fast 0}
		{MaxSpeed 0}
	}
}
