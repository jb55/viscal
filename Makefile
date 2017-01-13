
all: calendar

calendar: calendar.vala Event.vala
	valac --pkg gtk+-3.0 $^
