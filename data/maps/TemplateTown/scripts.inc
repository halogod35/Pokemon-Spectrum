@ PS-071: template region (placeholder made from Emerald's own tilesets, to be replaced by the
@ user's own region; see docs/regions.md). Not reachable from Hoenn in normal play.
TemplateTown_MapScripts::
	map_script MAP_SCRIPT_ON_TRANSITION, TemplateTown_OnTransition
	.byte 0

TemplateTown_OnTransition:
	setflag FLAG_VISITED_TEMPLATE_TOWN
	end

@ Transit back to Hoenn through the region transit helper (data/scripts/region_transit.inc).
TemplateTown_EventScript_Sailor::
	lock
	faceplayer
	setvar VAR_0x8004, 0
	special RegionTransit_CountDestinations
	compare VAR_RESULT, 0
	goto_if_eq TemplateTown_EventScript_SailorNowhere
	msgbox TemplateTown_Text_WhereTo, MSGBOX_DEFAULT
	call RegionTransit_EventScript_ChooseDestination
	compare VAR_RESULT, REGION_TRANSIT_CANCEL
	goto_if_eq TemplateTown_EventScript_SailorCancel
	closemessage
	call RegionTransit_EventScript_Warp
	release
	end

TemplateTown_EventScript_SailorNowhere::
	msgbox TemplateTown_Text_Nowhere, MSGBOX_DEFAULT
	release
	end

TemplateTown_EventScript_SailorCancel::
	msgbox TemplateTown_Text_ComeBack, MSGBOX_DEFAULT
	release
	end

@ Placeholder Gym Leader: hands out the region's first badge (FLAG_BADGE09_GET) so the
@ trainer card's badge row can be checked here.
TemplateTown_EventScript_Leader::
	lock
	faceplayer
	goto_if_set FLAG_BADGE09_GET, TemplateTown_EventScript_LeaderDone
	msgbox TemplateTown_Text_TakeBadge, MSGBOX_DEFAULT
	setflag FLAG_BADGE09_GET
	release
	end

TemplateTown_EventScript_LeaderDone::
	msgbox TemplateTown_Text_LeaderDone, MSGBOX_DEFAULT
	release
	end

TemplateTown_EventScript_Sign::
	msgbox TemplateTown_Text_Sign, MSGBOX_SIGN
	end

TemplateTown_Text_WhereTo:
	.string "Placeholder ferry.\n"
	.string "Where to?$"

TemplateTown_Text_Nowhere:
	.string "No destination is unlocked yet.$"

TemplateTown_Text_ComeBack:
	.string "Come back any time.$"

TemplateTown_Text_TakeBadge:
	.string "Placeholder GYM LEADER.\p"
	.string "Take this region's first BADGE\n"
	.string "and check your TRAINER CARD.$"

TemplateTown_Text_LeaderDone:
	.string "You already have this region's\n"
	.string "first BADGE.$"

TemplateTown_Text_Sign:
	.string "TEMPLATE TOWN\n"
	.string "Placeholder, to be replaced.$"
