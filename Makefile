
all: calendar

CFLAGS=-Wall \
       -Wextra \
       -Wno-unused-parameter \
			 -std=c99 \
			 -g \
       `pkg-config --cflags --libs gtk+-3.0`

calendar: calendar.c Makefile
	$(CC) $(CFLAGS) -o $@ $<
