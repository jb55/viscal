
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <locale.h>

#define length(array) (sizeof((array))/sizeof((array)[0]))

enum event_flags {
    EV_SELECTED    = 1 << 0
  , EV_HIGHLIGHTED = 1 << 1
  , EV_DRAGGING    = 1 << 2
};

enum cal_flags {
    CAL_MDOWN    = 1 << 0
  , CAL_DRAGGING = 1 << 1
};

struct cal {
  struct event *events;
  int nevents;
  enum cal_flags flags;
  struct event *target;
};

struct extra_data {
  GtkWindow *win;
  struct cal *cal;
};

struct event {
  time_t start;
  time_t end;
  char *title;
  enum event_flags flags;

  // set on draw
  double width, height;
  double x, y;
  double dragx, dragy;
};

union rgba {
  double rgba[4];
  struct {
    double r, g, b, a;
  };
};

#define max(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a > _b ? _a : _b; })

#define min(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a < _b ? _a : _b; })

static const double BGCOLOR = 0.35;
static const int TXTPAD = 11;
static const int EVPAD = 2;
static const int GAP = 0;
static const int DEF_LMARGIN = 20;

static int g_lmargin = 40;
static int g_margin_time_w = 0;
static union rgba g_text_color;
static int margin_calculated = 0;
static GdkCursor *cursor_pointer;
static GdkCursor *cursor_default;
const double dashed[] = {1.0};


static struct event* events_hit (struct event *, int, double, double);
static int event_hit (struct event *, double, double);
static void calendar_print_state(struct cal *cal);
static void format_margin_time (char *, int, int);
static void draw_hours (cairo_t *, int, int, int);
static void draw_background (cairo_t *, int, int);
static void draw_rectangle (cairo_t *, double, double);
static void event_update (struct event *, int, int, int, int);
static void event_draw (cairo_t *, struct event *);
static void update_events_flags (struct event*, int, double, double);
static void calendar_update (struct cal *cal, int width, int height);
static int calendar_draw (cairo_t *, struct cal*, int, int);

static gboolean
on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data);

static int on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data);
static int on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer user_data);
static int on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data);

static int
on_state_change(GtkWidget *widget, GdkEvent *ev, gpointer user_data) {
  struct extra_data *data = (struct extra_data*)user_data;
  struct cal *cal = data->cal;

  calendar_print_state(cal);

  return 1;
}

static void
calendar_drop(struct cal *cal, double mx, double my) {
  if (cal->target) {
    printf("dropped %s\n", cal->target->title);
  }
}

static void
event_click(struct event *event) {
  printf("clicked %s\n", event->title);
}

static int
on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data) {
  struct extra_data *data = (struct extra_data*)user_data;
  struct cal *cal = data->cal;
  double mx = ev->x;
  double my = ev->y;

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
    cal->flags &= ~(CAL_MDOWN | CAL_DRAGGING);
    break;
  }

  on_state_change(widget, (GdkEvent*)ev, user_data);

  return 1;
}

static int
event_any_flags(struct event *events, int flag, int nevents) {
  for (int i = 0; i < nevents; i++) {
    if ((events[i].flags & flag) != 0)
      return 1;
  }
  return 0;
}

static void
calendar_print_state(struct cal *cal) {
  printf("%s %s\r",
         (cal->flags & CAL_DRAGGING) != 0 ? "D " : "  ",
         (cal->flags & CAL_MDOWN)    != 0 ? "M " : "  "
         );
  fflush(stdout);
}

static int
on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer user_data) {
  static int prev_hit = 0;

  int hit = 0;
  int needs_redraw = 0;

  struct extra_data *data = (struct extra_data*)user_data;
  struct cal *cal = data->cal;
  GdkWindow *gdkwin = gtk_widget_get_window(widget);

  // drag detection
  if ((cal->flags & CAL_MDOWN) != 0) {
    if ((cal->flags & CAL_DRAGGING) == 0)
      cal->flags |= CAL_DRAGGING;
  }

  update_events_flags (cal->events, cal->nevents, ev->x, ev->y);
  hit = event_any_flags(cal->events, cal->nevents, EV_HIGHLIGHTED);

  gdk_window_set_cursor(gdkwin, hit ? cursor_pointer : cursor_default);

  needs_redraw = hit != prev_hit;

  if (needs_redraw)
    gtk_widget_queue_draw(widget);

  prev_hit = hit;

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

static int event_hit (struct event *ev, double mx, double my) {
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
update_events_flags (struct event *events, int nevents, double mx, double my) {
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
void time_remove_seconds(char *time) {
  int len = strlen(time);
  int count = 0;
  char *ws;
  for (int i = 0; i < len; ++i) {
    if (count == 1) {
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
  time_remove_seconds(buffer);
}


static void
draw_hours (cairo_t *cr, int sy, int width, int height) {
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

static double time_location (struct tm *d) {
  int hour = d->tm_hour;
  int minute = d->tm_min;
  int second = d->tm_sec;

  int seconds =
    (hour * 60 * 60) + (minute * 60) + second;

  return ((double)seconds / 86400.0);
}

static void
event_update (struct event *ev, int sx, int sy, int width, int height) {
  double sloc = time_location(localtime(&ev->start));
  double eloc = time_location(localtime(&ev->end));
  double dloc = eloc - sloc;
  double eheight = dloc * height;
  double y = (sloc * height) + sy;

  ev->width = width;
  ev->height = eheight;
  ev->x = sx;
  ev->y = y;
}

static void
event_draw (cairo_t *cr, struct event *ev) {
  // double height = Math.fmin(, MIN_EVENT_HEIGHT);
  // stdout.printf("sloc %f eloc %f dloc %f eheight %f\n",
  // 			  sloc, eloc, dloc, eheight);
  union rgba c = {
    .rgba = { 1.0, 0.0, 0.0, 0.25 }
  };

  if ((ev->flags & EV_HIGHLIGHTED) != 0) {
    c.a += 0.25;
  }

  cairo_move_to(cr, ev->x, ev->y);
  // TODO: calendar color for events
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  draw_rectangle(cr, ev->width, ev->height);
  cairo_fill(cr);
  cairo_move_to(cr, ev->x, ev->y);
  draw_rectangle(cr, ev->width, ev->height);
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a*2);
  cairo_stroke(cr);
  cairo_move_to(cr, ev->x + EVPAD, ev->y + EVPAD + TXTPAD);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_show_text(cr, ev->title);
}

static void
calendar_update (struct cal *cal, int width, int height) {
  int i;

  // TODO refactor urxff
  width -= g_lmargin+GAP;
  height -= GAP*2;

  for (i = 0; i < cal->nevents; ++i) {
    struct event *ev = &cal->events[i];
    event_update(ev, g_lmargin, GAP, width, height);
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
  draw_hours(cr, GAP, width+g_lmargin, height);

  // draw calendar events
  for (i = 0; i < cal->nevents; ++i) {
    struct event *ev = &cal->events[i];
    event_draw(cr, ev);
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

  struct event ev;
  struct event ev2;
  struct tm *sev;

  time(&ev.start);
  sev = localtime(&ev.start);
  sev->tm_hour += 1;
  ev.end = mktime(sev);
  ev.title = "Coding this";

  time(&ev2.start);
  sev = localtime(&ev2.start);
  sev->tm_hour += 1;
  sev->tm_min += 30;
  ev2.start = mktime(sev);
  sev->tm_min += 30;
  ev2.end = mktime(sev);
  ev2.title = "After coding this";

  struct event events[] = { ev, ev2 };

  struct cal cal = {
    .events = events,
    .nevents = length(events),
  };

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
