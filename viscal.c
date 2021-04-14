
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <libical/ical.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <locale.h>
#include <stdbool.h>

#define ARRAY_SIZE(array) (sizeof((array))/sizeof((array)[0]))

#define sgn(x) (((x) > 0) - ((x) < 0))

#define clamp(val, low, high) (val < low ? low : (val > high ? high : val))

#define max(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a > _b ? _a : _b; })

#define min(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a < _b ? _a : _b; })

#define EDITBUF_MAX 32768

// TODO: heap-realloc events array
#define MAX_EVENTS 1024
#define SMALLEST_TIMEBLOCK 5

static icaltimezone *tz_utc;
static const double BGCOLOR = 0.35;
static const int DAY_SECONDS = 86400;
static const int TXTPAD = 11;
static const int EVPAD = 2;
static const int EVMARGIN = 1;
static const double ZOOM_MAX = 10;
static const double ZOOM_MIN = 1;
/* static const int DEF_LMARGIN = 20; */


enum event_flags {
    EV_SELECTED    = 1 << 0
  , EV_HIGHLIGHTED = 1 << 1
  , EV_DRAGGING    = 1 << 2
};

enum cal_flags {
    CAL_MDOWN      = 1 << 0
  , CAL_DRAGGING   = 1 << 1
  , CAL_SPLIT      = 1 << 2
  , CAL_CHANGING   = 1 << 3
  , CAL_INSERTING  = 1 << 4
};

union rgba {
  double rgba[4];
  struct {
    double r, g, b, a;
  };
};

enum source {
	SOURCE_CALDAV,
	SOURCE_FILE
};

struct ical {
	icalcomponent * calendar;
	enum source source;
	const char *source_location;
	union rgba color;
	bool visible;
};

struct event {
	icalcomponent *vevent;
	struct ical *ical;

	enum event_flags flags;
	// set on draw
	double width, height;
	double x, y;
	double dragx, dragy;
	double dragx_off, dragy_off;
	time_t drag_time;
};

// used for temporary storage when editing summaries, descriptions, etc
static char g_editbuf[EDITBUF_MAX] = {0};
static int g_editbuf_pos = 0;

// TODO: move or remove g_cal_tz
static icaltimezone *g_cal_tz;

struct cal {
	GtkWidget *widget;
	struct ical calendars[128];
	int ncalendars;

	struct event events[MAX_EVENTS];
	int nevents;
	char chord;
	int repeat;

	icalcomponent *select_after_sort;
	// TODO: make multiple target selection
	int target;
	int selected_event_ind;
	int selected_calendar_ind;

	enum cal_flags flags;
	int timeblock_size;
	int timeblock_step;
	int refresh_events;
	int x, y, mx, my;
	int gutter_height;
	int font_size;
	double zoom, zoom_at;
	icaltimezone *tz;

	time_t current; // current highlighted position
	time_t today, start_at, scroll;

	int height, width;
};

typedef void (chord_cmd)(struct cal *);

struct chord {
	char *keys;
	chord_cmd *cmd;
};

static void align_hour(struct cal *);
static void align_up(struct cal *);
static void align_down(struct cal *);
static void center_view(struct cal *);
static void top_view(struct cal *);
static void bottom_view(struct cal *);
static void zoom_in(struct cal *);
static void zoom_out(struct cal *);
static void select_down(struct cal *);
static void select_up(struct cal *);
static void delete_timeblock(struct cal *);

static struct chord chords[] = {
	{ "ah", align_hour },
	{ "ak", align_up },
	{ "aj", align_down },
	{ "zz", center_view },
	{ "zt", top_view },
	{ "zb", bottom_view },
	{ "zi", zoom_in },
	{ "zo", zoom_out },
	{ "gj", select_down },
	{ "gk", select_up },
	{ "dd", delete_timeblock },
};

struct extra_data {
  GtkWindow *win;
  struct cal *cal;
};

static GdkCursor *cursor_default;
static GdkCursor *cursor_pointer;

static int g_lmargin = 18;
static int g_margin_time_w = 0;
static int margin_calculated = 0;

static union rgba g_text_color;
/* static union rgba g_timeline_color; */

static const double dashed[] = {1.0};

static void
calendar_create(struct cal *cal) {
	time_t now;
	time_t today, nowh;
	struct tm nowtm;

	now = time(NULL);
	nowtm = *localtime(&now);
	nowtm.tm_min = 0;
	nowh = mktime(&nowtm);
	nowtm.tm_hour = 0;
	nowtm.tm_sec = 0;
	today = mktime(&nowtm);

	cal->selected_calendar_ind = 0;
	cal->selected_event_ind = -1;
	cal->select_after_sort = NULL;
	cal->target = -1;
	cal->chord = 0;
	cal->gutter_height = 40;
	cal->font_size = 16;
	cal->timeblock_step = 15;
	cal->timeblock_size = 30;
	cal->flags = 0;
	cal->ncalendars = 0;
	cal->nevents = 0;
	cal->start_at = nowh - today - 4*60*60;
	cal->scroll = 0;
	cal->current = nowh;
	cal->repeat = 1;
	cal->today = today;
	cal->x = g_lmargin;
	cal->y = cal->gutter_height;
	cal->zoom = 5.0;
}

static void warn(const char *msg) {
	printf("WARN %s\n", msg);
}


static void set_current_calendar(struct cal *cal, struct ical *ical)
{
	for (int i = 0; i < cal->ncalendars; i++) {
		if (&cal->calendars[i] == ical)
			cal->selected_calendar_ind = i;
	}
}

static struct ical *current_calendar(struct cal *cal) {
	if (cal->ncalendars == 0)
		return NULL;

	return &cal->calendars[cal->selected_calendar_ind];
}

static time_t calendar_view_end(struct cal *cal)
{
	return cal->today + cal->start_at + cal->scroll + DAY_SECONDS;
}


static time_t calendar_view_start(struct cal *cal)
{
	return cal->today + cal->start_at + cal->scroll;
}



static int
span_overlaps(time_t start1, time_t end1, time_t start2, time_t end2) {
	return max(0, min(end1, end2) - max(start1, start2));
}


static void vevent_span_timet(icalcomponent *vevent, time_t *st, time_t *et)
{
	icaltimetype dtstart, dtend;

	if (st) {
		dtstart = icalcomponent_get_dtstart(vevent);
		*st = icaltime_as_timet_with_zone(dtstart, g_cal_tz);
	}

	if (et) {
		dtend = icalcomponent_get_dtend(vevent);
		*et = icaltime_as_timet_with_zone(dtend, g_cal_tz);
	}
}

static void select_event(struct cal *cal, int ind)
{
	time_t start ,end;
	struct event *ev;

	cal->selected_event_ind = ind;

	if (ind != -1) {
		ev = &cal->events[ind];
		vevent_span_timet(ev->vevent, &start, NULL);
		cal->current = start;
	}
}

/* static int */
/* vevent_in_span(icalcomponent *vevent, time_t start, time_t end) { */
/* 	/\* printf("vevent_in_span span.start %d span.end %d start %d end %d\n", *\/ */
/* 	/\* 	span.start, span.end, start, end); *\/ */
/* 	time_t st, et; */
/* 	vevent_span_timet(vevent, &st, &et); */
/* 	return span_overlaps(st, et, start, end); */

/* } */

static int sort_event(const void *a, const void*b) {
	time_t st_a, st_b;
	icaltimetype dtstart;
	struct event *ea = (struct event *)a;
	struct event *eb = (struct event *)b;

	dtstart = icalcomponent_get_dtstart(ea->vevent);
	st_a = icaltime_as_timet_with_zone(dtstart, dtstart.zone);

	dtstart = icalcomponent_get_dtstart(eb->vevent);
	st_b = icaltime_as_timet_with_zone(dtstart, dtstart.zone);

	if (st_a < st_b)
		return -1;
	else if (st_a == st_b)
		return 0;
	else
		return 1;
}

static time_t get_vevent_start(icalcomponent *vevent)
{
	icaltimetype dtstart = icalcomponent_get_dtstart(vevent);
	return icaltime_as_timet_with_zone(dtstart, dtstart.zone);
}

static int first_event_starting_at(struct cal *cal, time_t starting_at)
{
	time_t st;

	if (cal->nevents == 0)
		return -1;

	for (int i = cal->nevents - 1; i >= 0; i--) {
		vevent_span_timet(cal->events[i].vevent, &st, NULL);

		if (st >= starting_at)
			continue;
		else if (i == cal->nevents - 1)
			return -1;

		return i + 1;
	}

	assert(!"unpossible");
}

// seconds_range = 0 implies: do something reasonable (DAY_SECONDS/4)
static int find_event_within(struct cal *cal, time_t target, int seconds_range)
{
	struct event *ev;
	time_t evtime, diff, prev;

	if (seconds_range == 0)
		seconds_range = DAY_SECONDS/4;

	prev = target;

	if (cal->nevents == 0)
		return -1;
	else if (cal->nevents == 1)
		return 0;

	for (int i = cal->nevents-1; i >= 0; i--) {

		ev = &cal->events[i];
		vevent_span_timet(ev->vevent, &evtime, NULL);

		diff = abs(target - evtime);

		if (diff > prev) {
			if (prev > seconds_range)
				return -1;
			return i+1;
		}

		prev = diff;
	}

	assert(!"shouldn't get here");
}

/* static void select_closest_to_now(struct cal *cal) */
/* { */
/* 	time_t now = time(NULL); */
/* 	cal->selected_event_ind = find_event_within(cal, now, 0); */
/* } */


static void set_edit_buffer(const char *src)
{
	char *dst = g_editbuf;
	int n = 0;
	int c = EDITBUF_MAX;

	while (c-- && (*dst++ = *src++))
		n++;

	g_editbuf_pos = n;
}

static struct ical *get_selected_calendar(struct cal *cal)
{
	if (cal->ncalendars == 0 || cal->selected_calendar_ind == -1)
		return NULL;
	return &cal->calendars[cal->selected_calendar_ind];
}

static struct event *get_selected_event(struct cal *cal)
{
	if (cal->nevents == 0 || cal->selected_event_ind == -1)
		return NULL;
	return &cal->events[cal->selected_event_ind];
}

enum edit_mode_flags {
	EDIT_CLEAR = 1 << 1,
};

static void edit_mode(struct cal *cal, int flags)
{
	// TODO: STATUS BAR for edit mode

	struct event *event =
		get_selected_event(cal);

	// don't enter edit mode if we're not selecting any event
	if (!event)
		return;

	cal->flags |= CAL_CHANGING;

	if (flags & EDIT_CLEAR)
		return set_edit_buffer("");

	const char *summary =
		icalcomponent_get_summary(event->vevent);

	// TODO: what are we editing? for now assume summary
	// copy current summary to edit buffer
	set_edit_buffer(summary);
}



static void events_for_view(struct cal *cal, time_t start, time_t end)
{
	int i;
	struct event *event;
	icalcomponent *vevent;
	struct ical *calendar;
	icalcomponent *ical;

	cal->nevents = 0;

	for (i = 0; i < cal->ncalendars; ++i) {
		calendar = &cal->calendars[i];
		ical = calendar->calendar;
		for (vevent = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
		     vevent != NULL && cal->nevents < MAX_EVENTS;
		     vevent = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT))
		{

			// NOTE: re-add me when we care about filtering
			/* if (vevent_in_span(vevent, start, end)) { */
			event = &cal->events[cal->nevents++];
			/* printf("event in view %s\n", icalcomponent_get_summary(vevent)); */
			event->vevent = vevent;
			event->ical = calendar;
			/* } */
		}
	}

	printf("DEBUG sorting\n");
	qsort(cal->events, cal->nevents, sizeof(*cal->events), sort_event);

	// useful for selecting a new event after insertion
	if (cal->select_after_sort) {
		for (i = 0; i < cal->nevents; i++) {
			if (cal->events[i].vevent == cal->select_after_sort) {
				select_event(cal, i);
				// HACK: we might not always want to do this...
				edit_mode(cal, 0);
				break;
			}
		}
	}

	cal->select_after_sort = NULL;
}


static void on_change_view(struct cal *cal) {
	events_for_view(cal, calendar_view_start(cal), calendar_view_end(cal));
}


/* static void */
/* calendar_print_state(struct cal *cal) { */
/* 	static int c = 0; */
/* 	printf("%f %d %d %s %s %d\r", */
/* 	       cal->zoom, cal->mx, cal->my, */
/* 	       (cal->flags & CAL_DRAGGING) != 0 ? "D " : "  ", */
/* 	       (cal->flags & CAL_MDOWN)    != 0 ? "M " : "  ", */
/* 	       c++ */
/* 		); */
/* 	fflush(stdout); */
/* } */

static void calendar_refresh_events(struct cal *cal) {
	cal->refresh_events = 1;
	gtk_widget_queue_draw(cal->widget);
}


static int on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data) {
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;

		/* calendar_refresh_events(cal); */
	gtk_widget_queue_draw(cal->widget);
	/* calendar_print_state(cal); */

	return 1;
}


static char * file_load(char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *string = malloc(fsize);
	int res = fread(string, fsize, 1, f);
	if (!res) return NULL;
	fclose(f);
	return string;
}

static struct ical * calendar_load_ical(struct cal *cal, char *path) {
	// TODO: don't load duplicate calendars
	struct ical* ical;

	// TODO: free icalcomponent somewhere
	const char *str = file_load(path);
	if (str == NULL)
		return NULL;

	icalcomponent *calendar = icalparser_parse_string(str);
	if (!calendar)
		return NULL;

	// TODO: support >128 calendars
	if (ARRAY_SIZE(cal->calendars) == cal->ncalendars)
		return NULL;

	ical = &cal->calendars[cal->ncalendars++];
	ical->calendar = calendar;
	ical->visible = true;

	free((void*)str);
	return ical;
}


/* static void */
/* event_set_start(struct event *ev, time_t time, const icaltimezone *zone) { */
/*   if (zone == NULL) */
/*     zone = g_timezone; */
/*   icaltimetype ictime = icaltime_from_timet_with_zone_with_zone(time, 1, zone); */
/*   icalcomponent_set_dtstart(ev->vevent, ictime); */
/* } */

/* static void */
/* event_set_end(struct event *ev, time_t time, const icaltimezone *zone) { */
/*   if (zone == NULL) */
/*     zone = g_timezone; */
/*   icaltimetype ictime = icaltime_from_timet_with_zone_with_zone(time, 1, zone); */
/*   icalcomponent_set_dtend(ev->vevent, ictime); */
/* } */


static struct event *get_target(struct cal *cal) {
	if (cal->target == -1)
		return NULL;

	return &cal->events[cal->target];
}

static icaltimetype icaltime_from_timet_ours(time_t time, int is_date,
					     struct cal *cal)
{
	icaltimezone *tz;
	tz = cal == NULL ? g_cal_tz : cal->tz;

	return icaltime_from_timet_with_zone(time, is_date, tz);
}

static void calendar_drop(struct cal *cal, double mx, double my) {
	struct event *ev = get_target(cal);

	if (!ev)
		return;

	icaltime_span span = icalcomponent_get_span(ev->vevent);

	// TODO: use default event ARRAY_SIZE when dragging from gutter?
	time_t len = span.end - span.start;

	// XXX: should dragging timezone be the local timezone?
	// XXX: this will probably destroy the timezone, we don't want that
	// TODO: convert timezone on drag?

	icaltimetype startt =
		icaltime_from_timet_ours(ev->drag_time, 0, cal);

	icalcomponent_set_dtstart(ev->vevent, startt);

	icaltimetype endt =
		icaltime_from_timet_ours(ev->drag_time + len, 0, cal);

	icalcomponent_set_dtend(ev->vevent, endt);
}


static time_t location_to_time(time_t start, time_t end, double loc) {
	return (time_t)((double)start) + (loc * (end - start));
}



static time_t calendar_pos_to_time(struct cal *cal, double y) {
	// TODO: this is wrong wrt. zoom
	return location_to_time(calendar_view_start(cal),
				calendar_view_end(cal),
				y/((double)cal->height * cal->zoom));
}

static time_t calendar_loc_to_time(struct cal *cal, double y) {
	return location_to_time(calendar_view_start(cal),
				calendar_view_end(cal),
				y/cal->zoom);
}

static void event_click(struct cal *cal, struct event *event, int mx, int my) {
	printf("clicked %s\n", icalcomponent_get_summary(event->vevent));

	calendar_pos_to_time(cal, my);
}

// TODO: this should handle zh_CN and others as well
void time_remove_seconds(char *time, int n) {
	int len = strlen(time);
	int count = 0;
	char *ws;
	for (int i = 0; i < len; ++i) {
		if (count == n) {
			ws = &time[i];
			while (*ws != '\0' &&
			       (*ws == ':' ||
				(*ws >= '0' && *ws <= '9'))) ws++;
			len = strlen(ws);
			memcpy(&time[i-1], ws, len);
			time[i-1+len] = '\0';
			return;
		}
		// FIXME: instead of (==':'), we want (!= 0..9), in a unicode-enumerated way
		count += time[i] == ':' ? 1 : 0;
	}
}


static char *format_locale_time(char *buffer, int bsize, struct tm *tm) {
	strftime(buffer, bsize, "%X", tm);
	time_remove_seconds(buffer, 2);
	return buffer;
}


static char *format_locale_timet(char *buffer, int bsize, time_t time) {
	struct tm lt;
	lt = *localtime(&time);
	return format_locale_time(buffer, bsize, &lt);
}



static icalcomponent *create_event(struct cal *cal, time_t start, time_t end,
				   icalcomponent *ical) {
	static const char *default_event_summary = "";
	icalcomponent *vevent;
	icaltimetype dtstart = icaltime_from_timet_ours(start, 0, cal);
	icaltimetype dtend = icaltime_from_timet_ours(end, 0, cal);

	vevent = icalcomponent_new(ICAL_VEVENT_COMPONENT);

	icalcomponent_set_summary(vevent, default_event_summary);
	icalcomponent_set_dtstart(vevent, dtstart);
	icalcomponent_set_dtend(vevent, dtend);
	icalcomponent_add_component(ical, vevent);

	calendar_refresh_events(cal);
	cal->select_after_sort = vevent;

	return vevent;
}

static icalcomponent *calendar_def_cal(struct cal *cal) {
  // TODO: configurable default calendar
  if (cal->ncalendars > 0)
    return cal->calendars[0].calendar;
  return NULL;
}

static time_t closest_timeblock_for_timet(time_t st, int timeblock_size) {
	struct tm lt;
	lt = *localtime(&st);
	lt.tm_min = round(lt.tm_min / timeblock_size) * timeblock_size;
	lt.tm_sec = 0; // removes jitter
	return mktime(&lt);
}

static time_t closest_timeblock(struct cal *cal, int y) {
	time_t st = calendar_pos_to_time(cal, y);
	return closest_timeblock_for_timet(st, cal->timeblock_size);
}



static int event_hit (struct event *ev, double mx, double my) {
	return
		mx >= ev->x
		&& mx <= (ev->x + ev->width)
		&& my >= ev->y
		&& my <= (ev->y + ev->height);
}


static int events_hit (struct event *events, int nevents,
				 double mx, double my)
{
	for (int i = 0; i < nevents; ++i) {
		if (event_hit(&events[i], mx, my))
			return i;
	}
	return -1;
}

static void zoom(struct cal *cal, double amt)
{
	double newzoom = cal->zoom - amt * max(0.1, log(cal->zoom)) * 0.5;

	if (newzoom < ZOOM_MIN) {
		newzoom = ZOOM_MIN;
	}
	else if (newzoom > ZOOM_MAX) {
		newzoom = ZOOM_MAX;
	}

	cal->zoom = newzoom;
	cal->zoom_at = cal->my;
}

static int event_minutes(struct event *event)
{
	time_t st, et;
	vevent_span_timet(event->vevent, &st, &et);
	return (et - st) / 60;
}


static int timeblock_size(struct cal *cal)
{
	if (cal->selected_event_ind != -1) {
		struct event *ev = get_selected_event(cal);
		return event_minutes(ev);
	}

	return cal->timeblock_size;
}

static int find_closest_event(struct cal *cal, time_t near, int rel)
{
	struct event *ev;
	int is_up, ind;
	time_t start, end, diff, prev;

	is_up = rel == -1;
	prev = near;

	if (cal->nevents == 0)
		return -1;
	else if (cal->nevents == 1)
		return 0;

	for (int i = cal->nevents-1; i >= 0; i--) {
		ev = &cal->events[i];
		vevent_span_timet(ev->vevent, &start, &end);

		if (end <= near) {
			ind = is_up ? i : i+1;
			return ind == cal->nevents ? -1 : ind;
		}
	}

	return 0;
}

static inline int relative_selection(struct cal *cal, int rel)
{
	if (cal->selected_event_ind == -1) {
		return find_closest_event(cal, cal->current, rel);
	}

	return clamp(cal->selected_event_ind + rel, 0, cal->nevents - 1);
}

static time_t get_hour(time_t current)
{
	struct tm current_tm;
	current_tm = *localtime(&current);
	current_tm.tm_min = 0;
	current_tm.tm_sec = 0;
	return mktime(&current_tm);
}

static int get_minute(time_t current)
{
	struct tm current_tm;
	current_tm = *localtime(&current);
	return current_tm.tm_min;
}

static time_t get_smallest_closest_timeblock(time_t current, int round_by)
{
	struct tm current_tm;
	current_tm = *localtime(&current);
	current_tm.tm_min =
		round(current_tm.tm_min / (double)round_by) * round_by;
	current_tm.tm_sec = 0;

	return mktime(&current_tm);
}

static void align_down(struct cal *cal)
{
	/* assert(!"implement me"); */
	struct event *event =
		get_selected_event(cal);
	(void)event;
}

static void align_up(struct cal *cal)
{
	/* assert(!"implement me"); */
	struct event *event =
		get_selected_event(cal);
	(void)event;
}

static void align_hour(struct cal *cal)
{
	struct tm current_tm;
	time_t hour;
	current_tm = *localtime(&cal->current);
	current_tm.tm_min =
		round(current_tm.tm_min / 60.0) * 60;
	current_tm.tm_sec = 0;
	hour = mktime(&current_tm);

	printf("tm_min %d\n", current_tm.tm_min);

	cal->current = hour;
}

static void move_event_to(struct cal *cal, struct event *event, time_t to)
{
	time_t st, et;

	vevent_span_timet(event->vevent, &st, &et);

	icaltimetype dtstart =
		icaltime_from_timet_ours(to, 0, NULL);

	icaltimetype dtend =
		icaltime_from_timet_ours(to + (et - st), 0, NULL);

	icalcomponent_set_dtstart(event->vevent, dtstart);
	icalcomponent_set_dtend(event->vevent, dtend);

	calendar_refresh_events(cal);
}

static void move_event_now(struct cal *cal)
{
	struct event *event =
		get_selected_event(cal);

	if (event == NULL)
		return;

	time_t closest =
		get_smallest_closest_timeblock(time(NULL), SMALLEST_TIMEBLOCK);

	move_event_to(cal, event, closest);
}

static int time_in_view(struct cal *cal, time_t time) {
	time_t st = calendar_loc_to_time(cal, 0);
	time_t et = calendar_loc_to_time(cal, 1.0);

	return time >= st && time <= et;
}

static int timeline_in_view(struct cal *cal)
{
	return time_in_view(cal, cal->current);
}

static void deselect(struct cal *cal)
{
	cal->selected_event_ind = -1;
}

static void move_now(struct cal *cal)
{
	deselect(cal);

	cal->current =
		get_smallest_closest_timeblock(time(NULL), SMALLEST_TIMEBLOCK);

	if (!timeline_in_view(cal))
		center_view(cal);
}

static void insert_event(struct cal *cal, time_t st, time_t et,
			 struct ical *ical)
{
	cal->flags |= CAL_INSERTING;
	create_event(cal, st, et, ical->calendar);
}

static void insert_event_action_with(struct cal *cal, time_t st)
{
	// we should eventually always have a calendar
	// at least a temporary one
	if (cal->ncalendars == 0)
		return;

	time_t et = st + cal->timeblock_size * 60;

	insert_event(cal, st, et, current_calendar(cal));
}

static void insert_event_after_action(struct cal *cal)
{
	time_t start = cal->current + cal->timeblock_size * 60;
	insert_event_action_with(cal, start);
}


static void insert_event_action(struct cal *cal)
{
	insert_event_action_with(cal, cal->current);
}


static void calendar_view_clicked(struct cal *cal, int mx, int my) {
	time_t closest;
	/* int y; */
	char buf[32];

	closest = closest_timeblock(cal, my);

	format_locale_timet(buf, sizeof(buf), closest);
	printf("DEBUG (%d,%d) clicked @%s\n", mx, my, buf);
	insert_event_action(cal);
}


static int query_span(struct cal *cal, int index_hint, time_t start, time_t end,
		      time_t min_start, time_t max_end)
{
	time_t st, et;
	struct event *ev;

	for (int i=index_hint; i < cal->nevents; i++) {
		ev = &cal->events[i];

		if (!ev->ical->visible)
			continue;

		icaltimetype dtstart =
			icalcomponent_get_dtstart(ev->vevent);

		// date events aren't spans
		if (dtstart.is_date)
			continue;

		vevent_span_timet(ev->vevent, &st, &et);

		if ((min_start != 0 && st < min_start) ||
		    (max_end   != 0 && et > max_end))
			continue;
		else if (span_overlaps(st, et, start, end))
			return i;
	}

	return -1;
}

// TODO comebine parts with move_event?
static void move_relative(struct cal *cal, int rel)
{
	time_t st;
	time_t et;
	int hit;
	int timeblock = cal->timeblock_size * 60;

	// no current event selection
	if (cal->selected_event_ind == -1)  {
		cal->current += rel * timeblock;
	}
	else { // and event is selection
		struct event *ev = get_selected_event(cal);
		vevent_span_timet(ev->vevent, &st, &et);

		cal->current = rel > 0 ? et : st - timeblock;
	}

	st = cal->current;
	et = cal->current + timeblock;

	if ((hit = query_span(cal, 0, st, et, 0, 0)) != -1) {
		struct event *ev = &cal->events[hit];
		vevent_span_timet(ev->vevent, &st, &et);

		cal->current = st;
	}

	cal->selected_event_ind = hit;
}

static void move_up(struct cal *cal, int repeat)
{
	move_relative(cal, -1);
}

static void move_down(struct cal *cal, int repeat)
{
	move_relative(cal, 1);
}

static int number_of_hours_in_view(struct cal *cal)
{
	time_t st = calendar_loc_to_time(cal, 0);
	time_t et = calendar_loc_to_time(cal, 1.0);

	return (et - st) / 60 / 60;
}


static void relative_view(struct cal *cal, int hours)
{
	time_t current_hour;

	// currently needed because the grid is hour-aligned
	current_hour = get_hour(cal->current);

	// zt -> cal->current - cal->today
	cal->start_at = current_hour - cal->today - (hours * 60 * 60);
	cal->scroll = 0;
}


static void top_view(struct cal *cal)
{
	relative_view(cal, 0);
}

static void bottom_view(struct cal *cal)
{
	relative_view(cal, number_of_hours_in_view(cal) - 1);
}

static void center_view(struct cal *cal)
{
	int half_hours = number_of_hours_in_view(cal) / 2;
	relative_view(cal, half_hours);
}

static void expand_event(struct event *event, int minutes)
{
	icaltimetype dtend =
		icalcomponent_get_dtend(event->vevent);

	struct icaldurationtype add_minutes =
		icaldurationtype_from_int(minutes * 60);

	icaltimetype new_dtend =
		icaltime_add(dtend, add_minutes);

	icalcomponent_set_dtend(event->vevent, new_dtend);
	// TODO: push down
}

static void expand_selection_relative(struct cal *cal, int sign)
{
	int *step = &cal->timeblock_step;
	*step = SMALLEST_TIMEBLOCK;

	struct event *event = get_selected_event(cal);

	int size = timeblock_size(cal);

	/* *step = min(*step, size); */

	/* if (sign < 0 && size <= 15) */
	/* 	*step = 5; */
	/* else if (sign > 0 && size >= 15) */
	/* 	*step = 15; */

	int minutes = *step * sign;

	if (size + minutes <= 0)
		return;

	// no selected event, just expand selector
	if (event == NULL) {
		cal->timeblock_size += minutes;
		return;
	} else {
		// expand event if it's selected
		expand_event(event, minutes);
	}
}

static void expand_selection(struct cal *cal)
{
	expand_selection_relative(cal, 1);
}

static void shrink_selection(struct cal *cal)
{
	expand_selection_relative(cal, -1);
}

static void select_dir(struct cal *cal, int rel)
{
	int ind = relative_selection(cal, rel);
	select_event(cal, ind);
	if (!timeline_in_view(cal)) {
		center_view(cal);
	}
}

static void select_up(struct cal *cal) 
{
	for (int i = 0; i < cal->repeat; i++) {
		select_dir(cal, -1);
	}
}

static void select_down(struct cal *cal)
{
	for (int i = 0; i < cal->repeat; i++) {
		select_dir(cal, 1);
	}
}

// TODO: make zoom_amt configurable
static const int zoom_amt = 1.5;

static void zoom_in(struct cal *cal) {
	for (int i=0; i < cal->repeat; i++)
		zoom(cal, -zoom_amt);
}

static void zoom_out(struct cal *cal) {
	for (int i=0; i < cal->repeat; i++)
		zoom(cal, zoom_amt);
}

static void push_down(struct cal *cal, int ind, time_t push_to)
{
	time_t st, et, new_et;
	struct event *ev;

	ev = &cal->events[ind];
	vevent_span_timet(ev->vevent, &st, &et);

	if (st >= push_to)
		return;

	new_et = et - st + push_to;
	move_event_to(cal, ev, push_to);

	if (ind + 1 > cal->nevents - 1)
		return;

	// push rest
	push_down(cal, ind+1, new_et);
}

static void push_up(struct cal *cal, int ind, time_t push_to)
{
	time_t st, et, a_st, a_et, new_st;
	struct event *ev, *above;

	// our event
	ev = &cal->events[ind];
	vevent_span_timet(ev->vevent, &st, &et);

	move_event_to(cal, ev, push_to);

	if (ind - 1 < 0)
		return;

	// above event
	above = &cal->events[ind - 1 < 0 ? 0 : ind - 1];
	vevent_span_timet(above->vevent, &a_st, &a_et);

	if (push_to > a_et)
		return;

	new_st = push_to - (a_et - a_st);
	push_up(cal, ind-1, new_st);
}

static void push_expand_selection(struct cal *cal)
{
	time_t st, et, push_to, new_st;
	struct event *ev;

	expand_selection(cal);

	ev = get_selected_event(cal);

	if (ev == NULL)
		return;

	vevent_span_timet(ev->vevent, &st, &et);

	push_down(cal, cal->selected_event_ind+1, et);
}

static void pushmove_dir(struct cal *cal, int dir) {
	time_t st, et, push_to, new_st;
	struct event *ev;

	ev = get_selected_event(cal);

	if (ev == NULL)
		return;

	vevent_span_timet(ev->vevent, &st, &et);

	// TODO: configurable?
	static const int adjust = SMALLEST_TIMEBLOCK * 60;

	push_to = st + (adjust * dir);

	if (dir == 1)
		push_down(cal, cal->selected_event_ind, push_to);
	else
		push_up(cal, cal->selected_event_ind, push_to);
}

static void pushmove_down(struct cal *cal) {
	pushmove_dir(cal, 1);
}

static void pushmove_up(struct cal *cal) {
	pushmove_dir(cal, -1);
}

static void open_below(struct cal *cal)
{
	time_t st, et;
	int ind;
	time_t push_to;
	struct event *ev;

	ev = get_selected_event(cal);

	if (ev == NULL) {
		insert_event_after_action(cal);
		return;
	}

	vevent_span_timet(ev->vevent, &st, &et);

	push_to = et + timeblock_size(cal) * 60;

	// push down all nearby events
	// TODO: filter on visible calendars
	// TODO: don't push down immovable events
	for (ind = cal->selected_event_ind + 1; ind != -1; ind++) {
		ind = query_span(cal, ind, et, push_to, et, 0);

		if (ind == -1)
			break;
		else
			push_down(cal, ind, push_to);
	}

	set_current_calendar(cal, ev->ical);

	insert_event(cal, et, push_to, ev->ical);
}


static void save_calendar(struct ical *calendar)
{
	// TODO: caldav saving
	assert(calendar->source == SOURCE_FILE);
	printf("DEBUG saving %s\n", calendar->source_location);

	const char *str =
		icalcomponent_as_ical_string_r(calendar->calendar);

	FILE *fd = fopen(calendar->source_location, "w+");

	fwrite(str, strlen(str), 1, fd);

	fclose(fd);
}


static void finish_editing(struct cal *cal)
{
	struct event *event = get_selected_event(cal);

	if (!event)
		return;

	// TODO: what are we editing?
	// Right now we can only edit the summary

	// set summary of selected event
	icalcomponent_set_summary(event->vevent, g_editbuf);

	// leave edit mode, clear inserting flag
	cal->flags &= ~(CAL_CHANGING | CAL_INSERTING);

	// save the calendar
	save_calendar(event->ical);
}

static void append_str_edit_buffer(const char *src)
{
	if (*src == '\0')
		return;

	char *dst = &g_editbuf[g_editbuf_pos];
	int c = g_editbuf_pos;

	while (c < EDITBUF_MAX && (*dst++ = *src++))
		c++;

	if (c == EDITBUF_MAX-1)
		g_editbuf[EDITBUF_MAX-1] = '\0';

	g_editbuf_pos = c;

}

/* static void append_edit_buffer(char key) */
/* { */
/* 	if (g_editbuf_pos + 1 >= EDITBUF_MAX) { */
/* 		warn("attempting to write past end of edit buffer"); */
/* 		return; */
/* 	} */
/* 	g_editbuf[g_editbuf_pos++] = key; */
/* } */

static void pop_edit_buffer(int amount)
{
	amount = clamp(amount, 0, EDITBUF_MAX-1);
	int top = g_editbuf_pos - amount;
	top = clamp(top, 0, EDITBUF_MAX-1);
	g_editbuf[top] = '\0';
	g_editbuf_pos = top;
}

static time_t get_selection_end(struct cal *cal)
{
	return cal->current + cal->timeblock_size * 60;
}

static int event_is_today(time_t today, struct event *event)
{
	time_t st;
	vevent_span_timet(event->vevent, &st, NULL);
	return st < today + DAY_SECONDS;
}

static void move_event(struct event *event, int minutes)
{
	icaltimetype st, et;
	struct icaldurationtype add;

	st = icalcomponent_get_dtstart(event->vevent);
	et = icalcomponent_get_dtend(event->vevent);

	add = icaldurationtype_from_int(minutes * 60);

	st = icaltime_add(st, add);
	et = icaltime_add(et, add);

	icalcomponent_set_dtstart(event->vevent, st);
	icalcomponent_set_dtend(event->vevent, et);
}



static void move_event_action(struct cal *cal, int direction)
{
	struct event *event =
		get_selected_event(cal);

	if (!event)
		return;

	move_event(event, direction * cal->repeat * SMALLEST_TIMEBLOCK);
}

static void save_calendars(struct cal *cal)
{
	printf("DEBUG saving calendars\n");
	for (int i = 0; i < cal->ncalendars; ++i)
		save_calendar(&cal->calendars[i]);
}

static int closest_to_current(struct cal *cal, int ind_hint)
{
	int timeblock = timeblock_size(cal);
	return query_span(cal, ind_hint-1 < 0 ? 0 : ind_hint-1, cal->current,
			  cal->current + timeblock * 60, 0, 0);
}

static void delete_event(struct cal *cal, struct event *event)
{
	int i, ind = -1;
	time_t st;
	vevent_span_timet(event->vevent, &st, NULL);
	icalcomponent_remove_component(event->ical->calendar, event->vevent);

	for (i = cal->nevents - 1; i >= 0; i--) {
		if (&cal->events[i] == event) {
			ind = i;
			break;
		}
	}

	assert(ind != -1);

	memmove(&cal->events[ind],
		&cal->events[ind + 1],
		(cal->nevents - ind - 1) * sizeof(*cal->events));

	// adjust indices
	cal->nevents--;
	cal->selected_event_ind = closest_to_current(cal, 0);
	cal->target--;
}

// delete the event, and then pull everything below upwards (within that day)
static void delete_timeblock(struct cal *cal)
{
	int first;
	int i;
	int timeblock = timeblock_size(cal);

	struct event *event =
		get_selected_event(cal);

	if (event)
		delete_event(cal, event);

	// get all events in current day past dtend of current selection
	time_t starting_at =
		get_selection_end(cal);

	first =
		first_event_starting_at(cal, starting_at);

	// nothing to push down today
	if (first == -1)
		return;

	for (i = first;
	     i < cal->nevents && event_is_today(cal->today, &cal->events[i]);
	     i++) {
		struct event *event = &cal->events[i];
		move_event(event, -timeblock);
	}

	cal->selected_event_ind = closest_to_current(cal, first);
}


static void delete_event_action(struct cal *cal)
{
	struct event *event =
		get_selected_event(cal);

	if (event == NULL)
		return;

	delete_event(cal, event);
}

static void cancel_editing(struct cal *cal)
{
	// delete the event if we cancel during insert
	if (cal->flags & CAL_INSERTING) {
		struct event *event =
			get_selected_event(cal);

		// we should have a selected event if we're cancelling
		assert(event);

		delete_event(cal, event);
	}

	cal->flags &= ~(CAL_CHANGING | CAL_INSERTING);
}

static void pop_word_edit_buffer()
{
	int c = clamp(g_editbuf_pos - 2, 0, EDITBUF_MAX-1);
	char *p = &g_editbuf[c];

	while (p >= g_editbuf && *(p--) != ' ')
		;

	if (*(p + 1) == ' ') {
		p += 2;
		*p = '\0';
	}
	else
		*(++p) = '\0';

	g_editbuf_pos = p - g_editbuf;

	return;
}


static int on_edit_keypress(struct cal *cal, GdkEventKey *event)
{
	char key = *event->string;

	switch (event->keyval) {
	case GDK_KEY_Escape:
		cancel_editing(cal);
		return 1;

	case GDK_KEY_Return:
		finish_editing(cal);
		return 1;

	case GDK_KEY_BackSpace:
		pop_edit_buffer(1);
		return 1;
	}

	switch (key) {
	// Ctrl-w
	case 0x17:
		pop_word_edit_buffer();
		break;
	}

	// TODO: more special edit keys

	if (*event->string >= 0x20)
		append_str_edit_buffer(event->string);

	return 1;
}

static void debug_edit_buffer(GdkEventKey *event)
{
	int len = strlen(event->string);
	printf("DEBUG edit buffer: %s[%x][%ld] %d %d '%s'\n",
	       event->string,
	       len > 0 ? *event->string : '\0',
	       strlen(event->string),
	       event->state,
	       g_editbuf_pos,
	       g_editbuf);
}

static chord_cmd *get_chord_cmd(char current_chord, char key) {
	struct chord *chord;

	for (size_t i = 0; i < ARRAY_SIZE(chords); ++i) {
		chord = &chords[i];
		if (chord->keys[0] == current_chord && chord->keys[1] == key) {
			return chord->cmd;
		}
	}

	return NULL;
}

static void set_chord(struct cal *cal, char c)
{
	assert(cal->chord == 0);
	cal->chord = c;
}

static void move_event_to_calendar(struct cal *cal, struct event *event,
				   struct ical *from, struct ical *to)
{
	icalcomponent_remove_component(from->calendar, event->vevent);
	icalcomponent_add_component(to->calendar, event->vevent);
	event->ical = to;
}

static void next_calendar(struct cal *cal)
{
	struct event *event;
	struct ical *from;
	struct ical *to;

	from = &cal->calendars[cal->selected_calendar_ind];
	cal->selected_calendar_ind =
		(cal->selected_calendar_ind + 1) % cal->ncalendars;

	while((to = &cal->calendars[cal->selected_calendar_ind]) != from && !to->visible) {
		cal->selected_calendar_ind =
			(cal->selected_calendar_ind + 1) % cal->ncalendars;
	}

	// only one selectable calendar
	if (from == to)
		return;

	printf("using calendar %s\n", to->source_location);

	// move event to next calendar if we're editing it
	if (cal->flags & CAL_CHANGING) {
		event = get_selected_event(cal);
		if (!event)
			assert(!"no selected event when CAL_CHANGING");

		move_event_to_calendar(cal, event, from, to);
	}
}

static void toggle_calendar_visibility(struct cal *cal, int ind)
{
	if (ind+1 > cal->ncalendars)
		return;
	cal->calendars[ind].visible =
		!cal->calendars[ind].visible;
}

static gboolean on_keypress (GtkWidget *widget, GdkEvent *event,
			     gpointer user_data)
{
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;
	char key;
	int hardware_key;
	int state_changed = 1;
	static const int scroll_amt = 60*60;

	switch (event->type) {
	case GDK_KEY_PRESS:
		key = *event->key.string;
		hardware_key = event->key.hardware_keycode;

		printf("DEBUG keystring %x %d hw:%d\n",
		       key, event->key.state, event->key.hardware_keycode);

		// Ctrl-tab during editing still switch cal
		if (key != '\t' && (cal->flags & CAL_CHANGING)) {
			state_changed = on_edit_keypress(cal, &event->key);
			debug_edit_buffer(&event->key);
			goto check_state;
		}

		// handle chords
		if (cal->chord) {
			chord_cmd *cmd =
				get_chord_cmd(cal->chord, key);

			// no chord cmd found, reset chord
			if (cmd == NULL)
				cal->chord = 0;
			else {
				// execute chord
				(*cmd)(cal);

				// reset chord
				cal->chord = 0;

				// we've executed a command, so reset repeat
				cal->repeat = 1;

				state_changed = 1;
				goto check_state;
			}
		}

		int nkey = key - '0';

		if (nkey >= 2 && nkey <= 9) {
			printf("DEBUG repeat %d\n", nkey);
			cal->repeat = nkey;
			break;
		}

		switch (hardware_key) {
			// f1, f2, ...
		case 67: case 68: case 69:
		case 70: case 71: case 72:
			printf("f%d\n", hardware_key-66);
			int ind = hardware_key-67;
			assert(ind >= 0);
			toggle_calendar_visibility(cal, ind);
			break;
		}

		switch (key) {

		// Ctrl-d
		case 0x4:
			cal->scroll += scroll_amt;
			break;

		// Ctrl-u
		case 0x15:
			cal->scroll -= scroll_amt;
			break;

		// Ctrl-s
		case 0x13:
			save_calendars(cal);
			break;

		// Ctrl--
		case 0x2d:
			cal->font_size -= 2;
			break;

		// Ctrl-=
		case 0x3d:
			cal->font_size += 2;
			break;

		// tab
		case '\t':
			next_calendar(cal);
			break;

		case 'C':
		case 'c':
		case 's':
		case 'S':
			edit_mode(cal, EDIT_CLEAR);
			break;

		case 'A':
			if (cal->selected_event_ind != -1)
				edit_mode(cal, 0);
			break;

		case 'x':
			delete_event_action(cal);
			break;

		case 't':
			move_now(cal);
			break;

		case 'T':
			move_event_now(cal);
			break;

		case 'K':
			move_event_action(cal, -1);
			break;

		case 'J':
			move_event_action(cal, 1);
			break;

		// Ctrl-j
		case 0xa:
			pushmove_down(cal);
			break;

		case 0xb:
			pushmove_up(cal);
			break;

		case 'j':
			move_down(cal, cal->repeat);
			break;

		case 'k':
			move_up(cal, cal->repeat);
			break;

		case 0x16:
			push_expand_selection(cal);
			break;

		case 'v':
			expand_selection(cal);
			break;

		case 'V':
			shrink_selection(cal);
			break;

		case 'i':
			insert_event_action(cal);
			break;

		case 'o':
			open_below(cal);
			break;

		default:
			set_chord(cal, key);
		}

		//if (key != 0) {
		//	printf("DEBUG resetting repeat\n");
		//	cal->repeat = 1;
		//}

		break;
	default:
		state_changed = 0;
		break;
	}

check_state:
	if (state_changed)
		on_state_change(widget, event, user_data);

	return 1;
}

static int
on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data) {
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;
	struct event *target = NULL;
	double mx = ev->x;
	double my = ev->y;
	int state_changed = 1;

	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		cal->flags |= CAL_MDOWN;
		cal->target = events_hit(cal->events, cal->nevents, mx, my);
		target = get_target(cal);
		if (target) {
			target->dragy_off = target->y - my;
			target->dragx_off = target->x - mx;
		}
		break;
	case GDK_BUTTON_RELEASE:
		target = get_target(cal);

		if ((cal->flags & CAL_DRAGGING) != 0) {
			// finished drag
			// TODO: handle drop into and out of gutter
			calendar_drop(cal, mx, my);
		}
		else {
			// clicked target
			if (target)
				event_click(cal, target, mx, my);
			else if (my < cal->y) {
				// TODO: gutter clicked, create date event + increase gutter size
			}
			else {
				calendar_view_clicked(cal, mx, my - cal->y);
			}
		}

		// finished dragging
		cal->flags &= ~(CAL_MDOWN | CAL_DRAGGING);

		// clear target drag state
		if (target) {
			target->dragx = 0.0;
			target->dragy = 0.0;
			target->drag_time =
				icaltime_as_timet(icalcomponent_get_dtstart(target->vevent));
			target = NULL;
		}
		break;

	default:
		state_changed = 0;
		break;
	}

	if (state_changed)
		on_state_change(widget, (GdkEvent*)ev, user_data);

	return 1;
}

static struct event*
event_any_flags(struct event *events, int nevents, int flag) {
  for (int i = 0; i < nevents; i++) {
    if ((events[i].flags & flag) != 0)
      return &events[i];
  }
  return NULL;
}

static int
on_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer user_data) {
	// TODO: GtkGestureZoom
	// https://developer.gnome.org/gtk3/stable/GtkGestureZoom.html
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;

	on_state_change(widget, (GdkEvent*)ev, user_data);
	zoom(cal, ev->delta_y);

	return 0;
}

static void
update_event_flags (struct event *ev, double mx, double my) {
	if (event_hit(ev, mx, my))
		ev->flags |=  EV_HIGHLIGHTED;
	else
		ev->flags &= ~EV_HIGHLIGHTED;
}


static void
events_update_flags (struct event *events, int nevents, double mx, double my) {
	for (int i = 0; i < nevents; ++i) {
		struct event *ev = &events[i];
		update_event_flags (ev, mx, my);
	}
}



static int
on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer user_data) {
	static struct event* prev_hit = NULL;

	struct event *hit = NULL;
	struct event *target = NULL;

	int state_changed = 0;
	int dragging_event = 0;
	double mx = ev->x;
	double my = ev->y;

	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;
	GdkWindow *gdkwin = gtk_widget_get_window(widget);

	cal->mx = mx - cal->x;
	cal->my = my - cal->y;

	double px = ev->x;
	double py = ev->y;

	// drag detection
	if ((cal->flags & CAL_MDOWN) != 0)
	if ((cal->flags & CAL_DRAGGING) == 0)
		cal->flags |= CAL_DRAGGING;

		// dragging logic
	if ((cal->flags & CAL_DRAGGING) != 0) {
		target = get_target(cal);
		if (target) {
			dragging_event = 1;
			target->dragx = px - target->x;
			target->dragy =
				target->dragy_off + py - target->y - cal->y;
		}
	}

	events_update_flags (cal->events, cal->nevents, mx, my);
	hit = event_any_flags(cal->events, cal->nevents, EV_HIGHLIGHTED);

	gdk_window_set_cursor(gdkwin, hit ? cursor_pointer : cursor_default);

	state_changed = dragging_event || hit != prev_hit;

	fflush(stdout);
	prev_hit = hit;

	if (state_changed)
		on_state_change(widget, (GdkEvent*)ev, user_data);

	return 1;
}


static void
format_margin_time(char *buffer, int bsize, int hour) {
	struct tm tm = { .tm_min = 0, .tm_hour = hour };
	strftime(buffer, bsize, "%X", &tm);
	time_remove_seconds(buffer, 1);
}


static double
time_to_location (time_t start, time_t end, time_t time) {
	return ((double)(time - start) / ((double)(end - start)));
}



static double
calendar_time_to_loc(struct cal *cal, time_t time) {
	// ZOOM
	return time_to_location(calendar_view_start(cal),
				calendar_view_end(cal), time) * cal->zoom;
}



static double calendar_time_to_loc_absolute(struct cal *cal, time_t time) {
	return calendar_time_to_loc(cal, time) * cal->height + cal->y;
}



static void
event_update (struct event *ev, struct cal *cal)
{
	icaltimetype dtstart = icalcomponent_get_dtstart(ev->vevent);
	/* icaltimetype dtend = icalcomponent_get_dtend(ev->vevent); */
	int isdate = dtstart.is_date;
	double sx, sy, y, eheight, height, width;


	height = cal->height;
	width = cal->width;

	sx = cal->x;
	sy = cal->y;

	// height is fixed in top gutter for date events
	if (isdate) {
		// TODO: (DATEEV) gutter positioning
		eheight = 20.0;
		y = EVPAD;
	}
	else {
		// convert to local time
		time_t st, et;
		vevent_span_timet(ev->vevent, &st, &et);

		double sloc = calendar_time_to_loc(cal, st);
		double eloc = calendar_time_to_loc(cal, et);

		double dloc = eloc - sloc;
		eheight = dloc * height;
		y = (sloc * height) + sy;
	}

	ev->width = width;
	ev->height = eheight;
	ev->x = sx;
	ev->y = y;
}

static void
update_calendar (struct cal *cal) {
	int i, width, height;
	width = cal->width;
	height = cal->height;

	width  -= cal->x;
	height -= cal->y * 2;

	if (cal->refresh_events) {
		on_change_view(cal);
		cal->refresh_events = 0;
	}

	for (i = 0; i < cal->nevents; ++i) {
		struct event *ev = &cal->events[i];
		event_update(ev, cal);
	}
}




static void draw_rectangle (cairo_t *cr, double x, double y) {
	cairo_rel_line_to (cr, x, 0);
	cairo_rel_line_to (cr, 0, y);
	cairo_rel_line_to (cr, -x, 0);
	cairo_close_path (cr);
}


static void draw_background (cairo_t *cr, int width, int height) {
	cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
	draw_rectangle (cr, width, height);
	cairo_fill (cr);
}


static void
draw_hours (cairo_t *cr, struct cal* cal)
{
	double height = cal->height;
	double width  = cal->width;
	double zoom   = cal->zoom;

	double section_height = (((double)height) / 48.0) * zoom;
	char buffer[32] = {0};
	const double col = 0.4;
	cairo_set_source_rgb (cr, col, col, col);

	// TODO: dynamic section subdivide on zoom?
	for (int section = 0; section < 48; section++) {
		int start_section = ((cal->start_at + cal->scroll) / 60 / 60) * 2;
		int minutes = (start_section + section) * 30;
		int onhour = ((minutes / 30) % 2) == 0;
		int hour = (minutes / 60) % 24;
		int onday = onhour && hour == 0;

		if (section_height < 14 && !onhour)
			continue;

		double y = cal->y + ((double)section) * section_height;

		cairo_set_line_width (cr, onday ? 4 : 1);
		cairo_move_to (cr, cal->x, y);
		cairo_rel_line_to (cr, width, 0);

		if (section % 2 == 0)
			cairo_set_dash (cr, NULL, 0, 0);
		else
			cairo_set_dash (cr, dashed, 1, 0);

		cairo_stroke(cr);
		cairo_set_dash (cr, NULL, 0, 0);

		if (onhour) {
			format_margin_time(buffer, 32, hour);
			// TODO: text extents for proper time placement?
			cairo_move_to(cr, g_lmargin - (g_margin_time_w + EVPAD),
				      y+TXTPAD);
			cairo_set_source_rgb (cr,
						g_text_color.r,
						g_text_color.g,
						g_text_color.b);
			cairo_show_text(cr, buffer);
			cairo_set_source_rgb (cr, col, col, col);
		}
	}
}

static void
format_time_duration(char *buf, int bufsize, int seconds)
{
	int hours = seconds / 60 / 60;
	seconds -= hours * 60 * 60;

	int minutes = seconds / 60;
	seconds -= minutes * 60;

	if (hours == 0 && minutes == 0)
		snprintf(buf, bufsize, "%ds", seconds);
	else if (hours != 0 && minutes == 0)
		snprintf(buf, bufsize, "%dh", hours);
	else if (hours == 0 && minutes != 0)
		snprintf(buf, bufsize, "%dm", minutes);
	else
		snprintf(buf, bufsize, "%dh%dm", hours, minutes);
}

#define  Pr  .299
#define  Pg  .587
#define  Pb  .114
static void saturate(union rgba *c, double change)
{
	double  P=sqrt(
		(c->r)*(c->r)*Pr+
		(c->g)*(c->g)*Pg+
		(c->b)*(c->b)*Pb ) ;

	c->r = P+((c->r)-P)*change;
	c->g = P+((c->g)-P)*change;
	c->b = P+((c->b)-P)*change;
}

static double get_evheight(double evheight)
{
	return max(1.0, evheight - EVMARGIN);
}

static void
draw_event_summary(cairo_t *cr, struct cal *cal, time_t st, time_t et,
		   int is_date, int is_selected, double height, const char *summary,
		   struct event *sel, double x, double y)
{
	// TODO: event text color
	static char buffer[1024] = {0};
	static const double txtc = 0.2;
	static char bsmall[32] = {0};
	static char bsmall2[32] = {0};
	char *start_time;
	char *end_time;
	time_t len = et - st;

	cairo_text_extents_t exts;

	int is_editing = is_selected && (cal->flags & CAL_CHANGING);

	summary = is_editing ? g_editbuf : summary;

	cairo_set_source_rgb(cr, txtc, txtc, txtc);
	if (is_date) {
		sprintf(buffer, is_selected ? "'%s'" : "%s", summary);
		cairo_text_extents(cr, buffer, &exts);
		cairo_move_to(cr, x + EVPAD, y + (height / 2.0)
						+ ((double)exts.height / 2.0));
		cairo_show_text(cr, buffer);
	}
	/* else if (len > 30*60) { */
	/*   format_locale_timet(bsmall, 32, st); */
	/*   format_locale_timet(bsmall2, 32, et); */
	/*   sprintf(buffer, "%s — %s", bsmall, bsmall2); */
	/*   cairo_show_text(cr, buffer); */
	/*   cairo_move_to(cr, x + EVPAD, y + EVPAD + TXTPAD * 2); */
	/*   cairo_show_text(cr, summary); */
	/* } */
	else {
		start_time = format_locale_timet(bsmall, 32, st);
		end_time   = format_locale_timet(bsmall2, 32, et);
		// TODO: configurable event format
		char duration_format[32] = {0};
		char duration_format_in[32] = {0};
		char duration_format_out[32] = {0};
		time_t now, in, out;
		time(&now);

		in = now - st;
		out = et - now;

		format_time_duration(duration_format,
				     sizeof(duration_format), len);

		format_time_duration(duration_format_in,
				     sizeof(duration_format), in);

		format_time_duration(duration_format_out,
				     sizeof(duration_format), out);

		#define SHARED_EDIT "'%s' | %s-%s +%s"
		#define SHARED      "%s | %s-%s +%s"

		if (out >= 0 && in >= 0 && out < len) {
			const char *fmt =
				is_editing
				?  SHARED_EDIT "-%s %s"
				:  SHARED      "-%s %s";

				sprintf(buffer,
					fmt,
					summary,
					start_time,
					end_time,
					duration_format_in,
					duration_format_out,
					duration_format
					);
		}
		else if (in >= 0 && in < 0) {
			const char *fmt =
				is_editing
				?  SHARED_EDIT " | %s"
				:  SHARED     "%s | %s-%s %s +%s";

			sprintf(buffer, fmt,
				summary,
				start_time,
				end_time,
				duration_format,
				duration_format_in
				);
		}
		else {
			const char *fmt =
				is_editing
				? "'%s' | %s-%s %s"
				: "%s | %s-%s %s";

			sprintf(buffer,
				fmt,
				summary,
				start_time,
				end_time,
				duration_format);
		}

		cairo_text_extents(cr, buffer, &exts);
		double ey = height < exts.height
			? y + TXTPAD - EVPAD
			: y + TXTPAD + EVPAD;
		cairo_move_to(cr, x + EVPAD, ey);
		cairo_show_text(cr, buffer);
	}
}

static void
draw_event (cairo_t *cr, struct cal *cal, struct event *ev,
	    struct event *sel, struct event *target)
{
	union rgba c = ev->ical->color;

	int is_dragging = target == ev && (cal->flags & CAL_DRAGGING);
	int is_selected = sel == ev;
	icaltimetype dtstart =
		icalcomponent_get_dtstart(ev->vevent);

	time_t st, et;
	vevent_span_timet(ev->vevent, &st, &et);

	double x = ev->x;
	double y = ev->y;
	double evheight = get_evheight(ev->height);

	const char *summary =
		icalcomponent_get_summary(ev->vevent);

	if (is_dragging || ev->flags & EV_HIGHLIGHTED) {
		c.a *= 0.95;
	}

	// grid logic
	if (is_dragging) {
		/* x += ev->dragx; */
		y += ev->dragy;
		st = closest_timeblock(cal, y);
		y = calendar_time_to_loc_absolute(cal, st);
		target->drag_time = st;
	}

	/* y -= EVMARGIN; */

	cairo_move_to(cr, x, y);

	// TODO: selected event rendering
	if (is_selected)
		saturate(&c, 0.2);

	cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
	draw_rectangle(cr, ev->width, evheight);
	cairo_fill(cr);
	draw_event_summary(cr, cal, st, et, dtstart.is_date, is_selected,
			   evheight, summary, sel, x, y);
}


static inline void
draw_line (cairo_t *cr, double x, double y, double w) {
	cairo_move_to(cr, x, y + 0.5);
	cairo_rel_line_to(cr, w, 0);
}



static void
draw_time_line(cairo_t *cr, struct cal *cal, time_t time) {
	double y = calendar_time_to_loc_absolute(cal, time);
	int w = cal->width;

	cairo_set_line_width(cr, 1.0);

	cairo_set_source_rgb (cr, 1.0, 0, 0);
	draw_line(cr, cal->x, y - 1, w);
	cairo_stroke(cr);

	/* cairo_set_source_rgb (cr, 1.0, 1.0, 1.0); */
	/* draw_line(cr, cal->x, y, w); */
	/* cairo_stroke(cr); */

	/* cairo_set_source_rgb (cr, 0, 0, 0); */
	/* draw_line(cr, cal->x, y + 1, w); */
	/* cairo_stroke(cr); */
}

static void
draw_selection (cairo_t *cr, struct cal *cal)
{
	static const char *summary = "Selection";
	static const int is_selected = 0;
	static const int is_date = 0;
	double sx = cal->x;
	double sy = calendar_time_to_loc_absolute(cal, cal->current);
	time_t et = get_selection_end(cal);
	double height = calendar_time_to_loc_absolute(cal, et) - sy;

	cairo_move_to(cr, sx, sy);

	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
	draw_rectangle(cr, cal->width, height);
	cairo_fill(cr);
	draw_event_summary(cr, cal, cal->current, et, is_date, is_selected,
			   height, summary, NULL, sx, sy);

}

static void draw_current_minute(cairo_t *cr, struct cal *cal)
{
	char buffer[32] = {0};
	time_t now;
	time(&now);

	double y = calendar_time_to_loc_absolute(cal, now);
	const double col = 0.4; // TODO: duplication from draw_hours

	int min = get_minute(now);

	format_margin_time(buffer, 32, min);

	cairo_set_source_rgb (cr, 1.0, 0, 0);

	cairo_move_to(cr, g_lmargin - (g_margin_time_w + EVPAD),
		      y+(TXTPAD/2.0)-2.0);

	cairo_show_text(cr, buffer);
	cairo_set_source_rgb (cr, col, col, col);
}

static int
draw_calendar (cairo_t *cr, struct cal *cal) {
	int i, width, height;
	time_t now;
	width = cal->width;
	height = cal->height;

	cairo_move_to(cr, cal->x, cal->y);
	draw_background(cr, width, height);
	draw_hours(cr, cal);
	draw_current_minute(cr, cal);

	struct event *selected =
		get_selected_event(cal);

	// draw calendar events
	for (i = 0; i < cal->nevents; ++i) {
		struct event *ev = &cal->events[i];
		if (ev->ical->visible)
			draw_event(cr, cal, ev, selected, get_target(cal));
	}

	if (cal->selected_event_ind == -1)
		draw_selection(cr, cal);

	draw_time_line(cr, cal, time(&now));

	return 1;
}

static gboolean
on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	int width, height;
	struct extra_data *data = (struct extra_data*) user_data;
	struct cal *cal = data->cal;

	if (!margin_calculated) {
		char buffer[32];
		cairo_text_extents_t exts;

		format_margin_time(buffer, 32, 23);
		cairo_text_extents(cr, buffer, &exts);
		g_margin_time_w = exts.width;
		g_lmargin = g_margin_time_w + EVPAD*2;

		margin_calculated = 1;
	}

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_set_font_size(cr, cal->font_size);
	cairo_select_font_face(cr,
				"terminus",
				CAIRO_FONT_SLANT_NORMAL,
				CAIRO_FONT_WEIGHT_NORMAL);

	gtk_window_get_size(data->win, &width, &height);

	cal->y = cal->gutter_height;

	cal->width = width - cal->x;
	cal->height = height - cal->y;

	update_calendar(cal);
	draw_calendar(cr, cal);

	return FALSE;
}


void usage() {
	printf("usage: viscal <calendar.ics ...>\n");
	exit(1);
}

static inline double rand_0to1() {
	return (double) rand() / RAND_MAX;
}

static gboolean redraw_timer_handler(struct extra_data *data) {
	gtk_widget_queue_draw(data->cal->widget);
	return 1;
}


int main(int argc, char *argv[])
{
	GtkWidget *window;
	GtkWidget *darea;
	GdkDisplay *display;
	GdkColor color;
	char buffer[32];
	double text_col = 0.6;
	struct ical *ical;
	union rgba defcol;

	defcol.r = 106.0 / 255.0;
	defcol.g = 219.0 / 255.0;
	defcol.b = 219.0 / 255.0;
	defcol.a = 1.0;

	struct cal cal;

	calendar_create(&cal);

	if (argc < 2)
		usage();

	srand(0);

	for (int i = 1; i < argc; i++) {
		printf("loading calendar %s\n", argv[i]);
		ical = calendar_load_ical(&cal, argv[i]);
		ical->source = SOURCE_FILE;
		ical->source_location = argv[i];

		// TODO: configure colors from cli?
		if (ical != NULL) {
			ical->color = defcol;
			ical->color.r = rand_0to1() > 0.5 ? 1.0 : 0;
			ical->color.g = rand_0to1() > 0.5 ? 1.0 : 0;
			ical->color.b = rand_0to1() > 0.5 ? 1.0 : 0;
			ical->color.a = 0.9;

			saturate(&ical->color, 0.5);
		}
		else {
			printf("failed to load calendar\n");
		}
	}


	on_change_view(&cal);
	//select_closest_to_now(&cal);

	// TODO: get system timezone
	g_cal_tz = cal.tz = icaltimezone_get_builtin_timezone("America/Vancouver");
	tz_utc = icaltimezone_get_builtin_timezone("UTC");

	g_text_color.r = text_col;
	g_text_color.g = text_col;
	g_text_color.b = text_col;

	color.red = BGCOLOR * 0xffff * 0.6;
	color.green = BGCOLOR * 0xffff * 0.6;
	color.blue = BGCOLOR * 0xffff * 0.6;

	/* setlocale(LC_TIME, ""); */

	// calc margin
	format_margin_time(buffer, 32, 12);

	gtk_init(&argc, &argv);

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	struct extra_data extra_data = {
		.win = GTK_WINDOW(window),
		.cal = &cal
	};

	display = gdk_display_get_default();
	darea = gtk_drawing_area_new();
	cal.widget = darea;
	gtk_container_add(GTK_CONTAINER(window), darea);

	cursor_pointer = gdk_cursor_new_from_name (display, "pointer");
	cursor_default = gdk_cursor_new_from_name (display, "default");

	// redraw timer
	g_timeout_add(500, (GSourceFunc)redraw_timer_handler,
		      (gpointer)&extra_data);

	g_signal_connect(G_OBJECT(darea), "button-press-event",
			G_CALLBACK(on_press), (gpointer)&extra_data);

	g_signal_connect(window, "key-press-event",
			 G_CALLBACK(on_keypress), (gpointer)&extra_data);

	g_signal_connect(G_OBJECT(darea), "button-release-event",
			G_CALLBACK(on_press), (gpointer)&extra_data);

	g_signal_connect(G_OBJECT(darea), "motion-notify-event",
			G_CALLBACK(on_motion), (gpointer)&extra_data);

	g_signal_connect(G_OBJECT(darea), "scroll-event",
			G_CALLBACK(on_scroll), (gpointer)&extra_data);

	g_signal_connect(G_OBJECT(darea), "draw",
			G_CALLBACK(on_draw_event), (gpointer)&extra_data);

	g_signal_connect(window, "destroy",
			 G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_set_events(darea, GDK_BUTTON_PRESS_MASK
				| GDK_BUTTON_RELEASE_MASK
				| GDK_KEY_PRESS_MASK
				| GDK_KEY_RELEASE_MASK
				| GDK_SCROLL_MASK
				| GDK_SMOOTH_SCROLL_MASK
				| GDK_POINTER_MOTION_MASK);

	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(window), 400, 800);
	gtk_window_set_title(GTK_WINDOW(window), "viscal");

	// TODO: proper css/gtk styling?
	gtk_widget_modify_bg(window, GTK_STATE_NORMAL, &color);
	gtk_widget_show_all(window);

	gtk_main();

	return 0;
}
