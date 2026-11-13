@ PS-071: template region route (placeholder, to be replaced; see docs/regions.md).
TemplateRoute_MapScripts::
	.byte 0

TemplateRoute_EventScript_Sign::
	msgbox TemplateRoute_Text_Sign, MSGBOX_SIGN
	end

TemplateRoute_Text_Sign:
	.string "TEMPLATE ROUTE\n"
	.string "Placeholder, to be replaced.$"
