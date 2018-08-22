
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <libical/ical.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <locale.h>

#define length(array) (sizeof((array))/sizeof((array)[0]))

#define max(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a > _b ? _a : _b; })

#define min(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a < _b ? _a : _b; })

#define MAX_EVENTS 1024

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
    CAL_MDOWN    = 1 << 0
  , CAL_DRAGGING = 1 << 1
  , CAL_SPLIT    = 1 << 2
};

union rgba {
  double rgba[4];
  struct {
    double r, g, b, a;
  };
};

struct ical {
  icalcomponent * calendar;
  union rgba color;
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

struct cal {
	GtkWidget *widget;
	struct ical calendars[128];
	int ncalendars;

	struct event events[MAX_EVENTS];
	int nevents;
	char chord;
	int repeat;

	enum cal_flags flags;
	// TODO: make multiple target selection
	struct event *target;
	int minute_round;
	int refresh_events;
	int x, y, mx, my;
	int gutter_height;
	double zoom, zoom_at;

	time_t today, start_at, scroll;

	int height, width;
};

struct extra_data {
  GtkWindow *win;
  struct cal *cal;
};

static GdkCursor *cursor_default;
static GdkCursor *cursor_pointer;
static icaltimezone *g_timezone;
static int g_lmargin = 18;
static int g_margin_time_w = 0;
static int margin_calculated = 0;

static union rgba g_text_color;
/* static union rgba g_timeline_color; */

static const double dashed[] = {1.0};

static void
calendar_create(struct cal *cal) {
  time_t now;
  time_t today;
  struct tm nowtm;

  now = time(NULL);
  nowtm = *localtime(&now);
  nowtm.tm_hour = 0;
  nowtm.tm_min = 0;
  today = mktime(&nowtm);

  cal->chord = 0;
  cal->gutter_height = 40;
  cal->minute_round = 30;
  cal->ncalendars = 0;
  cal->nevents = 0;
  cal->start_at = 5*60*60;
  cal->scroll = 0;
  cal->repeat = 1;
  cal->today = today;
  cal->x = g_lmargin;
  cal->y = cal->gutter_height;
  cal->zoom = 1.0;
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




static int
vevent_in_view(icalcomponent *vevent, time_t start, time_t end) {
	icaltime_span span = icalcomponent_get_span(vevent);
	return span_overlaps(span.start, span.end, start, end);
}

static void
events_for_view(struct cal *cal, time_t start, time_t end)
{
	int i, count = 0;
	struct event *event;
	icalcomponent *vevent;
	struct ical *calendar;
	icalcomponent *ical;

	for (i = 0; i < cal->ncalendars; ++i) {
		calendar = &cal->calendars[i];
		ical = calendar->calendar;
		for (vevent = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
		     vevent != NULL && count < MAX_EVENTS;
		     vevent = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT))
		{

			if (vevent_in_view(vevent, start, end)) {
				event = &cal->events[count++];
				/* printf("event in view %s\n", icalcomponent_get_summary(vevent)); */
				event->vevent = vevent;
				event->ical = calendar;
			}
		}
		cal->nevents = count;
	}
}


static void
on_change_view(struct cal *cal) {
  events_for_view(cal, calendar_view_start(cal), calendar_view_end(cal));
}


static void
calendar_print_state(struct cal *cal) {
	static int c = 0;
	printf("%f %d %d %s %s %d\r",
	       cal->zoom, cal->mx, cal->my,
	       (cal->flags & CAL_DRAGGING) != 0 ? "D " : "  ",
	       (cal->flags & CAL_MDOWN)    != 0 ? "M " : "  ",
	       c++
		);
	fflush(stdout);
}

static int
on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data) {
	/* struct extra_data *data = (struct extra_data*)user_data; */

	/* calendar_print_state(cal); */
	gtk_widget_queue_draw(widget);

	return 1;
}


static char *
file_load(char *path) {
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

static struct ical *
calendar_load_ical(struct cal *cal, char *path) {
  // TODO: don't load duplicate calendars
  struct ical* ical;

  // TODO: free icalcomponent somewhere
  const char *str = file_load(path);
  if (str == NULL) return NULL;
  icalcomponent *calendar = icalparser_parse_string(str);
  if (!calendar) return NULL;

  // TODO: support >128 calendars
  if (length(cal->calendars) == cal->ncalendars)
    return NULL;

  ical = &cal->calendars[cal->ncalendars++];
  ical->calendar = calendar;

  free((void*)str);
  return ical;
}


/* static void */
/* event_set_start(struct event *ev, time_t time, const icaltimezone *zone) { */
/*   if (zone == NULL) */
/*     zone = g_timezone; */
/*   icaltimetype ictime = icaltime_from_timet_with_zone(time, 1, zone); */
/*   icalcomponent_set_dtstart(ev->vevent, ictime); */
/* } */

/* static void */
/* event_set_end(struct event *ev, time_t time, const icaltimezone *zone) { */
/*   if (zone == NULL) */
/*     zone = g_timezone; */
/*   icaltimetype ictime = icaltime_from_timet_with_zone(time, 1, zone); */
/*   icalcomponent_set_dtend(ev->vevent, ictime); */
/* } */



static void
calendar_drop(struct cal *cal, double mx, double my) {
	struct event *ev = cal->target;

	if (!ev)
		return;

	icaltime_span span = icalcomponent_get_span(ev->vevent);

	// TODO: use default event length when dragging from gutter?
	time_t len = span.end - span.start;

	// XXX: should dragging timezone be the local timezone?
	// XXX: this will probably destroy the timezone, we don't want that
	// TODO: convert timezone on drag?

	icaltimetype startt =
	icaltime_from_timet(ev->drag_time, 0);

	icalcomponent_set_dtstart(ev->vevent, startt);

	icaltimetype endt =
	icaltime_from_timet(ev->drag_time + len, 0);

	icalcomponent_set_dtend(ev->vevent, endt);
}


static time_t
location_to_time(time_t start, time_t end, double loc) {
	return (time_t)((double)start) + (loc * (end - start));
}



static time_t
calendar_loc_to_time(struct cal *cal, double y) {
	// TODO: this is wrong wrt. zoom
	return location_to_time(calendar_view_start(cal),
				calendar_view_end(cal),
				y/((double)cal->height * cal->zoom));
}

static void
event_click(struct cal *cal, struct event *event, int mx, int my) {
	printf("clicked %s\n", icalcomponent_get_summary(event->vevent));

	calendar_loc_to_time(cal, my);
}


static icalcomponent *
event_create(time_t time) {
  icalcomponent *vevent;
  vevent = icalcomponent_new(ICAL_VEVENT_COMPONENT);
  return vevent;
}

static icalcomponent *
calendar_def_cal(struct cal *cal) {
  // TODO: configurable default calendar
  if (cal->ncalendars > 0)
    return cal->calendars[0].calendar;
  return NULL;
}

static void
calendar_refresh_events(struct cal *cal) {
  cal->refresh_events = 1;
  gtk_widget_queue_draw(cal->widget);
}


static time_t
closest_timeblock(struct cal *cal, int y) {
	time_t st;
	struct tm lt;
	st = calendar_loc_to_time(cal, y);
	lt = *localtime(&st);
	lt.tm_min = round(lt.tm_min / cal->minute_round) * cal->minute_round;
	lt.tm_sec = 0; // removes jitter
	return mktime(&lt);
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



static void
format_locale_time(char *buffer, int bsize, struct tm *tm) {
	strftime(buffer, bsize, "%X", tm);
	time_remove_seconds(buffer, 2);
}



static void
format_locale_timet(char *buffer, int bsize, time_t time) {
	struct tm lt;
	lt = *localtime(&time);
	format_locale_time(buffer, bsize, &lt);
}


static void
calendar_view_clicked(struct cal *cal, int mx, int my) {
  icalcomponent *vevent;
  time_t closest;
  /* int y; */
  char buf[32];

  closest = closest_timeblock(cal, my);

  icaltimetype dtstart = icaltime_from_timet(closest, 0);
  icaltimetype dtend = icaltime_from_timet(closest + cal->minute_round * 60, 0);

  format_locale_timet(buf, length(buf), closest);
  printf("(%d,%d) clicked @%s\n", mx, my, buf);
  vevent = event_create(closest);
  icalcomponent_set_summary(vevent, "New Event");
  // TODO: configurable default duration
  /* icalcomponent_set_duration(vevent, duration); */
  icalcomponent_set_dtstart(vevent, dtstart);
  icalcomponent_set_dtend(vevent, dtend);
  icalcomponent_add_component(calendar_def_cal(cal), vevent);
  calendar_refresh_events(cal);

  /* y = calendar_time_to_loc(cal, closest) * cal->height; */
}

static int
event_hit (struct event *ev, double mx, double my) {
	return
		mx >= ev->x
		&& mx <= (ev->x + ev->width)
		&& my >= ev->y
		&& my <= (ev->y + ev->height);
}


static struct event*
events_hit (struct event *events, int nevents, double mx, double my) {
	for (int i = 0; i < nevents; ++i) {
		if (event_hit(&events[i], mx, my))
			return &events[i];
	}
	return NULL;
}

static void zoom(struct cal *cal, double amt) {
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

static gboolean on_keypress (GtkWidget *widget, GdkEvent  *event, gpointer user_data)
{
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;
	char key;
	int state_changed = 1;
	int i = 0;
	static const int scroll_amt = 60*60;
	static const int zoom_amt = 1.5;

	switch (event->type) {
	case GDK_KEY_PRESS:
		key = *event->key.string;
		int nkey = key - '0';

		if (nkey >= 2 && nkey <= 9) {
			cal->repeat = nkey;
			break;
		}

		switch (key) {
		case 'd':
			cal->scroll += scroll_amt;
			cal->repeat = 1;
			break;
		case 'u':
			cal->scroll -= scroll_amt;
			cal->repeat = 1;
			break;
		case 'z':
			if (cal->chord == 0) {
				cal->chord = 'z';
			}
			else if (cal->chord == 'z') {
				// TODO: center around current time
				cal->chord = 0;
				cal->repeat = 1;
			}
			break;
		case 'i':
			if (cal->chord == 'z') {
				for (i=0; i < cal->repeat; i++)
					zoom(cal, -zoom_amt);
				cal->repeat = 1;
				cal->chord = 0;
			}
			break;
		case 'o':
			if (cal->chord == 'z') {
				for (i=0; i < cal->repeat; i++)
					zoom(cal, zoom_amt);
				cal->repeat = 1;
				cal->chord = 0;
			}
			break;
		}
		break;
	default:
		state_changed = 0;
		break;
	}

	if (state_changed)
		on_state_change(widget, event, user_data);

	return 1;
}

static int
on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data) {
	struct extra_data *data = (struct extra_data*)user_data;
	struct cal *cal = data->cal;
	double mx = ev->x;
	double my = ev->y;
	int state_changed = 1;

	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		cal->flags |= CAL_MDOWN;
		cal->target = events_hit(cal->events, cal->nevents, mx, my);
		if (cal->target) {
			cal->target->dragy_off = cal->target->y - my;
			cal->target->dragx_off = cal->target->x - mx;
		}
		break;
	case GDK_BUTTON_RELEASE:
		if ((cal->flags & CAL_DRAGGING) != 0) {
			// finished drag
			// TODO: handle drop into and out of gutter
			calendar_drop(cal, mx, my);
		}
		else {
			// clicked target
			if (cal->target)
				event_click(cal, cal->target, mx, my);
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
		if (cal->target) {
			cal->target->dragx = 0.0;
			cal->target->dragy = 0.0;
			cal->target->drag_time =
				icaltime_as_timet(icalcomponent_get_dtstart(cal->target->vevent));
			cal->target = NULL;
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
		if (cal->target) {
			dragging_event = 1;
			cal->target->dragx = px - cal->target->x;
			cal->target->dragy =
				cal->target->dragy_off + py - cal->target->y - cal->y;
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



static void
event_update (struct event *ev, struct cal *cal)
{
	icaltimetype dtstart = icalcomponent_get_dtstart(ev->vevent);
	icaltimetype dtend = icalcomponent_get_dtend(ev->vevent);
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
		time_t st = icaltime_as_timet_with_zone(dtstart, dtstart.zone);
		time_t et = icaltime_as_timet_with_zone(dtend, dtend.zone);

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
	cairo_set_line_width (cr, 1);

	// TODO: dynamic section subdivide on zoom?
	for (int section = 0; section < 48; section++) {
		int start_section = ((cal->start_at + cal->scroll) / 60 / 60) * 2;
		int minutes = (start_section + section) * 30;
		int onhour = ((minutes / 30) % 2) == 0;
		if (section_height < 14 && !onhour)
			continue;

		double y = cal->y + ((double)section) * section_height;
		cairo_move_to (cr, cal->x, y);
		cairo_rel_line_to (cr, width, 0);

		if (section % 2 == 0)
			cairo_set_dash (cr, NULL, 0, 0);
		else
			cairo_set_dash (cr, dashed, 1, 0);

		cairo_stroke(cr);
		cairo_set_dash (cr, NULL, 0, 0);

		if (onhour) {
			format_margin_time(buffer, 32, (minutes / 60) % 24);
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

static void
draw_event (cairo_t *cr, struct cal *cal, struct event *ev) {
	// double height = Math.fmin(, MIN_EVENT_HEIGHT);
	// stdout.printf("sloc %f eloc %f dloc %f eheight %f\n",
	// 			  sloc, eloc, dloc, eheight);
	static char bsmall[32] = {0};
	static char bsmall2[32] = {0};
	static char buffer[1024] = {0};

	union rgba c = ev->ical->color;
	int is_dragging = cal->target == ev && (cal->flags & CAL_DRAGGING);
	double evheight = max(1.0, ev->height - EVMARGIN);
	/* double evwidth = ev->width; */
	/* icaltimezone *tz = icalcomponent_get_timezone(ev->vevent, "UTC"); */
	icaltimetype dtstart = icalcomponent_get_dtstart(ev->vevent);
	icaltimetype dtend = icalcomponent_get_dtend(ev->vevent);
	int isdate = dtstart.is_date;

	double x = ev->x;
	// TODO: date-event stacking
	double y = ev->y;

	/* printf("utc? %s dstart hour %d min %d\n", */
	/*        dtstart.is_utc? "yes" : "no", */
	/*        dtstart.hour, */
	/*        dtstart.minute); */

	time_t st = icaltime_as_timet_with_zone(dtstart, dtstart.zone);
	time_t et = icaltime_as_timet_with_zone(dtend, dtend.zone);
	time_t len = et - st;
	cairo_text_extents_t exts;

	const char * const summary =
		icalcomponent_get_summary(ev->vevent);

	if (is_dragging || ev->flags & EV_HIGHLIGHTED) {
		c.a *= 0.95;
	}

	// grid logic
	if (is_dragging) {
		/* x += ev->dragx; */
		y += ev->dragy;
		st = closest_timeblock(cal, y);
		y = cal->y + calendar_time_to_loc(cal, st) * cal->height;
		cal->target->drag_time = st;
	}

	/* y -= EVMARGIN; */

	cairo_move_to(cr, x, y);
	cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
	draw_rectangle(cr, ev->width, evheight);
	cairo_fill(cr);
	// TODO: event text color
	static const double txtc = 0.2;
	cairo_set_source_rgb(cr, txtc, txtc, txtc);
	if (isdate) {
		sprintf(buffer, "%s", summary);
		cairo_text_extents(cr, buffer, &exts);
		cairo_move_to(cr, x + EVPAD, y + (evheight / 2.0)
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
		format_locale_timet(bsmall, 32, st);
		format_locale_timet(bsmall2, 32, et);
		/* printf("%ld %ld start %s end %s\n", st, et, bsmall, bsmall2); */
		// TODO: configurable event format
		char duration_format[32] = {0};
		char duration_format_in[32] = {0};
		char duration_format_out[32] = {0};
		time_t now, in, out;
		time(&now);

		in = now - st;
		out = et - now;

		format_time_duration(duration_format, sizeof(duration_format), len);
		format_time_duration(duration_format_in, sizeof(duration_format), in);
		format_time_duration(duration_format_out, sizeof(duration_format), out);

		if (out >= 0 && in >= 0 && out < len)
			sprintf(buffer, "%s | %s | %s in | %s left", summary,
				duration_format,
				duration_format_in,
				duration_format_out);
		else if (in >= 0 && in < 0)
			sprintf(buffer, "%s | %s | %s in", summary,
				duration_format,
				duration_format_in);
		else
			sprintf(buffer, "%s | %s", summary, duration_format);

		cairo_text_extents(cr, buffer, &exts);
		double ey = evheight < exts.height
			? y + TXTPAD - EVPAD
			: y + TXTPAD + EVPAD;
		cairo_move_to(cr, x + EVPAD, ey);
		cairo_show_text(cr, buffer);
	}
}


static inline void
draw_line (cairo_t *cr, double x, double y, double w) {
	cairo_move_to(cr, x, y + 0.5);
	cairo_rel_line_to(cr, w, 0);
}



static void
draw_time_line(cairo_t *cr, struct cal *cal, time_t time) {
	double loc = calendar_time_to_loc(cal, time);
	double y = (loc * cal->height) + cal->y;
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

static int
draw_calendar (cairo_t *cr, struct cal *cal) {
	int i, width, height;
	time_t now;
	width = cal->width;
	height = cal->height;

	cairo_move_to(cr, cal->x, cal->y);
	draw_background(cr, width, height);
	draw_hours(cr, cal);

	// draw calendar events
	for (i = 0; i < cal->nevents; ++i) {
		struct event *ev = &cal->events[i];
		draw_event(cr, cal, ev);
	}

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
	cairo_set_font_size(cr, 12);
	cairo_select_font_face(cr,
				"sans",
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

		// TODO: configure colors from cli?
		if (ical != NULL) {
			ical->color = defcol;
			ical->color.r = rand_0to1();
			ical->color.g = rand_0to1();
			ical->color.b = rand_0to1();
			ical->color.a = 1.0;
		}
		else {
			printf("failed to load calendar\n");
		}
	}

	on_change_view(&cal);

	// TODO: get system timezone
	g_timezone = icaltimezone_get_builtin_timezone("America/Vancouver");

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

	gtk_widget_modify_bg(window, GTK_STATE_NORMAL, &color);
	gtk_widget_show_all(window);

	gtk_main();

	return 0;
}
