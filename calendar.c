
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

static const double BGCOLOR = 0.35;
static const int DAY_SECONDS = 86400;
static const int MAX_EVENTS = 1024;
static const int TXTPAD = 11;
static const int EVPAD = 2;
static const int GAP = 0;
static const int DEF_LMARGIN = 20;


enum event_flags {
    EV_SELECTED    = 1 << 0
  , EV_HIGHLIGHTED = 1 << 1
  , EV_DRAGGING    = 1 << 2
};

enum cal_flags {
    CAL_MDOWN    = 1 << 0
  , CAL_DRAGGING = 1 << 1
};

struct event {
  icalcomponent *vevent;
  icalcomponent *calendar;

  enum event_flags flags;
  // set on draw
  double width, height;
  double x, y;
  double dragx, dragy;
  time_t drag_time;
};

struct cal {
  icalcomponent * calendars[128];
  int ncalendars;

  struct event events[MAX_EVENTS];
  int nevents;

  enum cal_flags flags;
  // TODO: make multiple target selection
  struct event *target;
  int minute_round;

  time_t view_start;
  time_t view_end;
};

struct extra_data {
  GtkWindow *win;
  struct cal *cal;
};

union rgba {
  double rgba[4];
  struct {
    double r, g, b, a;
  };
};

static int g_lmargin = 40;
static icaltimezone *g_timezone;
static int g_margin_time_w = 0;
static union rgba g_text_color;
static int margin_calculated = 0;
static GdkCursor *cursor_pointer;
static GdkCursor *cursor_default;
static const double dashed[] = {1.0};

static struct event* events_hit (struct event *, int, double, double);
static int event_hit (struct event *, double, double);

static icalcomponent* calendar_load_ical(struct cal *, char *);
static void calendar_print_state(struct cal *);
static void calendar_create(struct cal *);
static void calendar_update (struct cal *, int, int);
static int calendar_draw (cairo_t *, struct cal*, int, int);

static void format_margin_time (char *, int, int);
static void format_locale_time(char *, int, struct tm *);
static void draw_hours (cairo_t *, int, int, int);
static void draw_background (cairo_t *, int, int);
static void draw_rectangle (cairo_t *, double, double);

static int vevent_in_view(icalcomponent *, time_t, time_t);
static void events_for_view(struct cal *, time_t, time_t);
static void event_update (struct event *, time_t, time_t, int, int, int, int);
static void event_draw (cairo_t *, struct cal*, struct event *, int);
static inline icaltime_span event_get_span (struct event*);
static void events_update_flags (struct event*, int, double, double);


static gboolean
on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data);

static void on_change_view(struct cal*);

static int on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data);
static int on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer user_data);
static int on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data);

static void
calendar_create(struct cal *cal) {
  time_t now;
  time_t today;
  int start_at = 0;
  struct tm nowtm;

  now = time(NULL);
  nowtm = *localtime(&now);
  nowtm.tm_hour = 0;
  nowtm.tm_min = 0;
  today = mktime(&nowtm);

  cal->ncalendars = 0;
  cal->nevents = 0;
  cal->minute_round = 30;
  cal->view_start = today + start_at;
  cal->view_end = today + DAY_SECONDS;
}

static void
on_change_view(struct cal *cal) {
  events_for_view(cal, cal->view_start, cal->view_end);
}

static int
on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data) {
  struct extra_data *data = (struct extra_data*)user_data;
  struct cal *cal = data->cal;

  calendar_print_state(cal);
  gtk_widget_queue_draw(widget);

  return 1;
}

static char *
file_load(char *path) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *string = malloc(fsize);
  fread(string, fsize, 1, f);
  fclose(f);
  return string;
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
  icalcomponent *ical;

  for (i = 0; i < cal->ncalendars; ++i) {
    ical = cal->calendars[i];
    for (vevent = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
         vevent != NULL && count < MAX_EVENTS;
         vevent = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT))
    {
      if (vevent_in_view(vevent, start, end)) {
        event = &cal->events[count++];
        /* printf("event in view %s\n", icalcomponent_get_summary(vevent)); */
        event->vevent = vevent;
        event->calendar = ical;
      }
    }
    cal->nevents = count;
  }
}


static icalcomponent *
calendar_load_ical(struct cal *cal, char *path) {
  // TODO: don't load duplicate calendars

  icalcomponent *prop;
  // TODO: free icalcomponent somewhere
  const char *str = file_load(path);
  icalcomponent *calendar = icalparser_parse_string(str);
  if (!calendar) return NULL;

  // TODO: support >128 calendars
  if (length(cal->calendars) == cal->ncalendars)
    return NULL;

  cal->calendars[cal->ncalendars++] = calendar;

  free((void*)str);
  return calendar;
}


static void
event_set_start(struct event *ev, time_t time, const icaltimezone *zone) {
  if (zone == NULL)
    zone = g_timezone;
  icaltimetype ictime = icaltime_from_timet_with_zone(time, 1, zone);
  icalcomponent_set_dtstart(ev->vevent, ictime);
}

static void
event_set_end(struct event *ev, time_t time, const icaltimezone *zone) {
  if (zone == NULL)
    zone = g_timezone;
  icaltimetype ictime = icaltime_from_timet_with_zone(time, 1, zone);
  icalcomponent_set_dtend(ev->vevent, ictime);
}



static void
calendar_drop(struct cal *cal, double mx, double my) {
  struct event *ev = cal->target;
  if (ev) {
    icaltime_span span = icalcomponent_get_span(ev->vevent);
    icaltimetype start = icalcomponent_get_dtstart(ev->vevent);

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
}

static void
event_click(struct event *event) {
  printf("clicked %s\n", icalcomponent_get_summary(event->vevent));
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
    break;
  case GDK_BUTTON_RELEASE:
    if ((cal->flags & CAL_DRAGGING) != 0) {
      // finished drag
      calendar_drop(cal, mx, my);
    }
    else {
      // clicked target
      if (cal->target)
        event_click(cal->target);
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
  default: state_changed = 0; break;
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

static void
calendar_print_state(struct cal *cal) {
  static int c = 0;
  printf("%s %s %d\r",
         (cal->flags & CAL_DRAGGING) != 0 ? "D " : "  ",
         (cal->flags & CAL_MDOWN)    != 0 ? "M " : "  ",
         c++
         );
  fflush(stdout);
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

  double px = ev->x;
  double py = ev->y;

  // drag detection
  if ((cal->flags & CAL_MDOWN) != 0) {
    if ((cal->flags & CAL_DRAGGING) == 0)
      cal->flags |= CAL_DRAGGING;
  }

  // dragging logic
  if ((cal->flags & CAL_DRAGGING) != 0) {
    if (cal->target) {
      dragging_event = 1;
      cal->target->dragx = px - cal->target->x;
      cal->target->dragy = py - cal->target->y;
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

static struct event*
events_hit (struct event *events, int nevents, double mx, double my) {
  for (int i = 0; i < nevents; ++i) {
    if (event_hit(&events[i], mx, my))
      return &events[i];
  }
  return NULL;
}

static int
event_hit (struct event *ev, double mx, double my) {
  return
    mx >= ev->x
    && mx <= (ev->x + ev->width)
    && my >= ev->y
    && my <= (ev->y + ev->height);
}

static void
update_event_flags (struct event *ev, double mx, double my) {
  int hit = event_hit(ev, mx, my);
  if (hit) ev->flags |=  EV_HIGHLIGHTED;
  else     ev->flags &= ~EV_HIGHLIGHTED;
}

static void
events_update_flags (struct event *events, int nevents, double mx, double my) {
  for (int i = 0; i < nevents; ++i) {
    struct event *ev = &events[i];
    update_event_flags (ev, mx, my);
  }
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

  gtk_window_get_size(data->win, &width, &height);
  calendar_update(cal, width, height);
  calendar_draw(cr, cal, width, height);

  return FALSE;
}


static void draw_background (cairo_t *cr, int width, int height) {
  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  draw_rectangle (cr, width, height);
  cairo_fill (cr);
}


static void draw_rectangle (cairo_t *cr, double x, double y) {
  cairo_rel_line_to (cr, x, 0);
  cairo_rel_line_to (cr, 0, y);
  cairo_rel_line_to (cr, -x, 0);
  cairo_close_path (cr);
}


// TODO: this should handle zh_CN and others as well
void time_remove_seconds(char *time, int n) {
  int len = strlen(time);
  int count = 0;
  char *ws;
  for (int i = 0; i < len; ++i) {
    if (count == n) {
      ws = &time[i];
      while (*ws != '\0' && (*ws == ':' || (*ws >= '0' && *ws <= '9'))) ws++;
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
format_margin_time(char *buffer, int bsize, int hour) {
  struct tm tm = { .tm_min = 0, .tm_hour = hour };
  strftime(buffer, bsize, "%X", &tm);
  time_remove_seconds(buffer, 1);
}

static void
format_locale_time(char *buffer, int bsize, struct tm *tm) {
  strftime(buffer, bsize, "%X", tm);
  time_remove_seconds(buffer, 2);
}


static void
draw_hours (cairo_t *cr, int sy, int width, int height)
{
  double section_height = ((double)height) / 48.0;
  char buffer[32] = {0};
  const double col = 0.4;
  cairo_set_source_rgb (cr, col, col, col);
  cairo_set_line_width (cr, 1);

  for (int section = 0; section < 48; section++) {

    int minutes = section * 30;
    double y = sy + ((double)section) * section_height;
    cairo_move_to (cr, 0, y);
    cairo_rel_line_to (cr, width, 0);

    if (section % 2 == 0)
      cairo_set_dash (cr, NULL, 0, 0);
    else
      cairo_set_dash (cr, dashed, 1, 0);

    cairo_stroke(cr);
    cairo_set_dash (cr, NULL, 0, 0);

    int onhour = ((minutes / 30) % 2) == 0;
    if (onhour) {
      format_margin_time(buffer, 32, minutes / 60);
      // TODO: text extents for proper time placement?
      cairo_move_to(cr, g_lmargin - (g_margin_time_w + EVPAD), y+TXTPAD);
      cairo_set_source_rgb (cr,
                            g_text_color.r,
                            g_text_color.g,
                            g_text_color.b);
      cairo_show_text(cr, buffer);
      cairo_set_source_rgb (cr, col, col, col);
    }
  }
}

static time_t
location_to_time(time_t start, time_t end, double loc) {
  return (time_t)((double)start) + (loc * (end - start));
}

static double time_to_location (time_t start, time_t end, time_t time) {
  return ((double)(time - start) / ((double)(end - start)));
}

static void
event_update (struct event *ev, time_t view_start, time_t view_end,
              int sx, int sy, int width, int height)
{
  icaltime_span span = icalcomponent_get_span(ev->vevent);
  double sloc = time_to_location(view_start, view_end, span.start);
  double eloc = time_to_location(view_start, view_end, span.end); 

  double dloc = eloc - sloc;
  double eheight = dloc * height;
  double y = (sloc * height) + sy;

  ev->width = width;
  ev->height = eheight;
  ev->x = sx;
  ev->y = y;
}

static void
event_draw (cairo_t *cr, struct cal *cal, struct event *ev, int height) {
  // double height = Math.fmin(, MIN_EVENT_HEIGHT);
  // stdout.printf("sloc %f eloc %f dloc %f eheight %f\n",
  // 			  sloc, eloc, dloc, eheight);
  static char bsmall[32] = {0};
  static char buffer[1024] = {0};

  int is_dragging = cal->target == ev && (cal->flags & CAL_DRAGGING);
  double x = ev->x;
  double y = ev->y;
  time_t st = icalcomponent_get_span(ev->vevent).start;
  struct tm lt;

  union rgba c = {
    .rgba = { 106.0 / 255.0
            , 190.0 / 255.0
            , 219.0 / 255.0
            , 1.0
            }
  };

  if (is_dragging || ev->flags & EV_HIGHLIGHTED) {
    c.a *= 0.95;
  }

  // grid logic
  if (is_dragging) {
    /* x += ev->dragx; */
    y += ev->dragy;
    st = location_to_time(cal->view_start, cal->view_end, y/height);
    lt = *localtime(&st);
    lt.tm_min = round(lt.tm_min / cal->minute_round) * cal->minute_round;
    lt.tm_sec = 0; // removes jitter
    st = mktime(&lt);
    y = time_to_location(cal->view_start, cal->view_end, st) * height;
    cal->target->drag_time = st;
  }

  cairo_move_to(cr, x, y);
  // TODO: calendar color for events
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  draw_rectangle(cr, ev->width, ev->height);
  cairo_fill(cr);
  cairo_move_to(cr, x, y);
  cairo_rel_line_to(cr, ev->width, 0);
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  cairo_stroke(cr);
  cairo_move_to(cr, x + EVPAD, y + EVPAD + TXTPAD);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  lt = *localtime(&st);
  format_locale_time(bsmall, 32, &lt);
  sprintf(buffer, "%s %s", bsmall, icalcomponent_get_summary(ev->vevent));
  cairo_show_text(cr, buffer);
}

static void
calendar_update (struct cal *cal, int width, int height) {
  int i;

  // TODO refactor urxff
  width -= g_lmargin+GAP;
  height -= GAP*2;

  for (i = 0; i < cal->nevents; ++i) {
    struct event *ev = &cal->events[i];
    event_update(ev, cal->view_start, cal->view_end,
                 g_lmargin, GAP, width, height);
  }
}

static int
calendar_draw (cairo_t *cr, struct cal *cal, int width, int height) {
  int i;

  // TODO refactor urxff
  width -= g_lmargin+GAP;
  height -= GAP*2;

  cairo_move_to(cr, g_lmargin, GAP);
  draw_background(cr, width, height);

  // cairo_move_to (GAP, GAP);
  draw_hours(cr, GAP, width + g_lmargin, height);

  // draw calendar events
  for (i = 0; i < cal->nevents; ++i) {
    struct event *ev = &cal->events[i];
    event_draw(cr, cal, ev, height);
  }

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

  struct cal cal;

  calendar_create(&cal);
  calendar_load_ical(&cal, "/home/jb55/Downloads/mycalendar.ics");
  on_change_view(&cal);

  // TODO: get system timezone
  g_timezone = icaltimezone_get_builtin_timezone("America/Vancouver");

  g_text_color.r = text_col;
  g_text_color.g = text_col;
  g_text_color.b = text_col;

  color.red = BGCOLOR * 0xffff;
  color.green = BGCOLOR * 0xffff;
  color.blue = BGCOLOR * 0xffff;

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
  gtk_container_add(GTK_CONTAINER(window), darea);

  cursor_pointer = gdk_cursor_new_from_name (display, "pointer");
  cursor_default = gdk_cursor_new_from_name (display, "default");

  g_signal_connect(G_OBJECT(darea), "button-press-event",
                   G_CALLBACK(on_press), (gpointer)&extra_data);
  g_signal_connect(G_OBJECT(darea), "button-release-event",
                   G_CALLBACK(on_press), (gpointer)&extra_data);
  g_signal_connect(G_OBJECT(darea), "motion-notify-event",
                   G_CALLBACK(on_motion), (gpointer)&extra_data);

  g_signal_connect(G_OBJECT(darea), "draw",
                   G_CALLBACK(on_draw_event), (gpointer)&extra_data);

  g_signal_connect(window, "destroy",
      G_CALLBACK(gtk_main_quit), NULL);

  gtk_widget_set_events(darea, GDK_BUTTON_PRESS_MASK
                             | GDK_BUTTON_RELEASE_MASK
                             | GDK_POINTER_MOTION_MASK);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 800);
  gtk_window_set_title(GTK_WINDOW(window), "viscal");

  gtk_widget_modify_bg(window, GTK_STATE_NORMAL, &color);
  gtk_widget_show_all(window);

  gtk_main();

  return 0;
}
