@ PS-071: region transit helper. Lets a ferry / train / any NPC in any region offer the
@ destinations the player has unlocked (one entry per hub in sRegionTransitDestinations,
@ src/region.c; an entry is offered when its flag is set and it lies in another region than
@ the player's, or in any region when VAR_0x8004 = 1) and warp there.
@
@ Example NPC script (the template town's sailor, data/maps/TemplateTown/scripts.inc, uses it):
@
@ MyTown_EventScript_Sailor::
@ 	lock
@ 	faceplayer
@ 	setvar VAR_0x8004, 0
@ 	special RegionTransit_CountDestinations
@ 	compare VAR_RESULT, 0
@ 	goto_if_eq MyTown_EventScript_SailorNowhere
@ 	msgbox MyTown_Text_WhereTo, MSGBOX_DEFAULT
@ 	call RegionTransit_EventScript_ChooseDestination
@ 	compare VAR_RESULT, REGION_TRANSIT_CANCEL
@ 	goto_if_eq MyTown_EventScript_SailorCancel
@ 	closemessage
@ 	call RegionTransit_EventScript_Warp
@ 	release
@ 	end
@
@ No Hoenn NPC uses it; Hoenn is unchanged.

@ Shows the destination menu (plus EXIT). Afterwards VAR_RESULT is the chosen destination
@ (an index into the table, to be passed to RegionTransit_EventScript_Warp) or
@ REGION_TRANSIT_CANCEL when EXIT or B was chosen.
RegionTransit_EventScript_ChooseDestination::
	special ScriptMenu_CreateRegionTransitMultichoice
	waitstate
	special GetRegionTransitSelection
	return

@ Warps to destination VAR_RESULT (fade out, load the map, fade in), like the warp command.
RegionTransit_EventScript_Warp::
	special RegionTransit_Warp
	waitstate
	return
