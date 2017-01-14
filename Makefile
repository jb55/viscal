
all: calendar

CFLAGS=-Wall \
       -Wextra \
			 -std=c99 \
       `pkg-config --cflags --libs gtk+-3.0`

calendar: calendar.c
	$(CC) $(CFLAGS) -o $@ $<
