@ PS-095: the boat back to Hoenn. The sailor on the pier by the S.S. ANNE dock gate offers the hubs of
@ the other regions (sRegionTransitDestinations in src/region.c: Slateport City's harbour) through
@ data/scripts/region_transit.inc. Always open; the Hoenn side is gated on the Hall of Fame.
KantoVermilionCity_EventScript_HoennSailor::
	lock
	faceplayer
	setvar VAR_0x8004, 0
	special RegionTransit_CountDestinations
	goto_if_eq VAR_RESULT, 0, KantoVermilionCity_EventScript_HoennSailorNowhere
	message KantoVermilionCity_Text_HoennSailorWhereTo
	waitmessage
	call RegionTransit_EventScript_ChooseDestination
	goto_if_eq VAR_RESULT, REGION_TRANSIT_CANCEL, KantoVermilionCity_EventScript_HoennSailorCancel
	closemessage
	call RegionTransit_EventScript_Warp
	release
	end

KantoVermilionCity_EventScript_HoennSailorNowhere::
	msgbox KantoVermilionCity_Text_HoennSailorNowhere, MSGBOX_DEFAULT
	release
	end

KantoVermilionCity_EventScript_HoennSailorCancel::
	msgbox KantoVermilionCity_Text_HoennSailorCancel, MSGBOX_DEFAULT
	release
	end

KantoVermilionCity_Text_HoennSailorWhereTo:
	.string "Ahoy! This boat sails to the HOENN\n"
	.string "region, way off to the south.\p"
	.string "Where would you like to go?$"

KantoVermilionCity_Text_HoennSailorNowhere:
	.string "Ahoy! This boat sails to the HOENN\n"
	.string "region… but not today.\p"
	.string "Come back another time!$"

KantoVermilionCity_Text_HoennSailorCancel:
	.string "Come back anytime!$"
