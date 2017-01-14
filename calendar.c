
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <locale.h>

struct event {
  time_t start;
  time_t end;
  char *title;
};

union color {
  float rgb[3];
  float r, g, b;
};

#define max(a,b)                                \
  ({ __typeof__ (a) _a = (a);                   \
    __typeof__ (b) _b = (b);                    \
    _a > _b ? _a : _b; })

static const int DEF_LMARGIN = 20;
static int g_lmargin = 40;
static int g_margin_time_w = 0;
static union color g_text_color;
static int margin_calculated = 0;
static const double bg_color = 0.35;
static const int TXTPAD = 11;
static const int EVPAD = 2;
static const int GAP = 0;
const double dashed[] = {1.0};

static void format_margin_time (char *, int, int);
static void draw_hours (cairo_t *, int, int, int);
static void draw_background (cairo_t *, int, int);
static void draw_rectangle (cairo_t *, double, double);
static void draw_event (cairo_t *, int, int, struct event *, int, int);
static int draw_cal (cairo_t *, int, int);

static gboolean
on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data);

static int on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data);

static int
on_press(GtkWidget *widget, GdkEventButton *ev, gpointer user_data) {
  printf("press\n");
  return 1;
}

static gboolean
on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  int width, height;
  GtkWindow *win = (GtkWindow*) user_data;
  if (!margin_calculated) {
    char buffer[32];
    cairo_text_extents_t exts;

    format_margin_time(buffer, 32, 23);
    cairo_text_extents(cr, buffer, &exts);
    g_margin_time_w = exts.width;
    g_lmargin = max(g_margin_time_w + EVPAD*2, DEF_LMARGIN);

    margin_calculated = 1;
  }
  gtk_window_get_size(win, &width, &height);
  draw_cal(cr, width, height);

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

void time_remove_seconds(char *time) {
  int len = strlen(time);
  int count = 0;
  char *ws;
  for (int i = 0; i < len; ++i) {
    if (count == 2) {
      ws = &time[i];
      while (*ws != '\0' && (*ws >= '0' && *ws <= '9')) ws++;
      len = strlen(ws);
      memcpy(&time[i-1], ws, len);
      time[i-1+len] = '\0';
      return;
    }
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
draw_event (cairo_t *cr, int sx, int sy,
            struct event *ev, int width, int height) {
  double sloc = time_location(localtime(&ev->start));
  double eloc = time_location(localtime(&ev->end));
  double dloc = eloc - sloc;
  double eheight = dloc * height;

  // double height = Math.fmin(, MIN_EVENT_HEIGHT);
  // stdout.printf("sloc %f eloc %f dloc %f eheight %f\n",
  // 			  sloc, eloc, dloc, eheight);

  cairo_move_to(cr, sx, (sloc * height)+sy);
  cairo_set_source_rgba(cr, 1.0, 0, 0, 0.25);
  draw_rectangle(cr, (double)width, eheight);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_stroke(cr);
  cairo_move_to(cr, sx + EVPAD, (sloc * height)+EVPAD+TXTPAD+sy);
  cairo_show_text(cr, ev->title);
}


static int draw_cal (cairo_t *cr, int width, int height) {
  struct event ev;
  struct tm *sev;

  time(&ev.start);
  sev = localtime(&ev.start);
  sev->tm_hour += 1;
  ev.end = mktime(sev);
  ev.title = "Coding this";

  width -= g_lmargin+GAP; height -= GAP*2;

  cairo_move_to(cr, g_lmargin, GAP);
  draw_background(cr, width, height);

  // cairo_move_to (GAP, GAP);
  draw_hours(cr, GAP, width+g_lmargin, height);

  draw_event(cr, g_lmargin, GAP, &ev, width, height);

  return 1;
}

int main(int argc, char *argv[])
{
  GtkWidget *window;
  GtkWidget *darea;
  GdkColor color;
  char buffer[32];
  double text_col = 0.6;

  g_text_color.r = text_col;
  g_text_color.g = text_col;
  g_text_color.b = text_col;

  color.red = bg_color * 0xffff;
  color.green = bg_color * 0xffff;
  color.blue = bg_color * 0xffff;

  /* setlocale(LC_TIME, ""); */

  // calc margin
  format_margin_time(buffer, 32, 12);

  gtk_init(&argc, &argv);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

  darea = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(window), darea);

  g_signal_connect(G_OBJECT(darea), "button-press-event",
                   G_CALLBACK(on_press), (gpointer)NULL);

  g_signal_connect(G_OBJECT(darea), "draw",
                   G_CALLBACK(on_draw_event), (gpointer)window);

  g_signal_connect(window, "destroy",
      G_CALLBACK(gtk_main_quit), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 90);
  gtk_window_set_title(GTK_WINDOW(window), "GTK window");

  gtk_widget_modify_bg(window, GTK_STATE_NORMAL, &color);
  gtk_widget_show_all(window);

  gtk_main();

  return 0;
}
