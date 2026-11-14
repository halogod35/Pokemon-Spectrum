@ PS-080: the Eon Ticket's Wonder Card script, modelled on gift_aurora_ticket.inc. The decomp has
@ no such script (Emerald only got the Eon Ticket by record mixing with Ruby/Sapphire). Handed out
@ by the Mystery Gift man in a Pokemon Center 2F once the built-in distributor has sent the card.
MysteryGiftScript_EonTicket::
	setvaddress MysteryGiftScript_EonTicket
	lock
	faceplayer
	vgoto_if_set FLAG_RECEIVED_EON_TICKET, EonTicket_Obtained
	vgoto_if_set FLAG_ENABLE_SHIP_SOUTHERN_ISLAND, EonTicket_Obtained
	checkitem ITEM_EON_TICKET
	vgoto_if_eq VAR_RESULT, TRUE, EonTicket_Obtained
	vmessage sText_EonTicketForYou
	waitmessage
	waitbuttonpress
	checkitemspace ITEM_EON_TICKET
	vgoto_if_eq VAR_RESULT, FALSE, EonTicket_NoBagSpace
	giveitem ITEM_EON_TICKET
	setflag FLAG_ENABLE_SHIP_SOUTHERN_ISLAND
	setflag FLAG_RECEIVED_EON_TICKET
	vmessage sText_EonTicketUseAtPort
	waitmessage
	waitbuttonpress
	release
	end

EonTicket_NoBagSpace:
	vmessage sText_EonTicketBagFull
	waitmessage
	waitbuttonpress
	release
	end

EonTicket_Obtained:
	vmessage sText_EonTicketThankYou
	waitmessage
	waitbuttonpress
	release
	end

sText_EonTicketForYou:
	.string "Thank you for using the MYSTERY\n"
	.string "GIFT System.\p"
	.string "You must be {PLAYER}.\n"
	.string "A ticket has arrived for you.$"

sText_EonTicketUseAtPort:
	.string "It can be used at the LILYCOVE CITY\n"
	.string "port.\p"
	.string "The ship will take you somewhere\n"
	.string "you have never been.$"

sText_EonTicketThankYou:
	.string "Thank you for using the MYSTERY\n"
	.string "GIFT System.$"

sText_EonTicketBagFull:
	.string "Oh, I'm sorry, {PLAYER}.\n"
	.string "Your BAG's KEY ITEMS POCKET is full.\p"
	.string "Please store something on your PC,\n"
	.string "then come back for this.$"

@ PS-080: end label, so C code can send the script with its real size (End - Start)
MysteryGiftScript_EonTicket_End::
