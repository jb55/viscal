
using Gtk;
using Cairo;

public class Calendar : Gtk.Window {

    private const int SIZE = 30;
    private const double MIN_EVENT_HEIGHT = 10.0;
    private const int GAP = 0;
    private const int LMARGIN = 40;
    private const int TXTPAD = 11;
    private const int EVPAD = 2;
	private const double dashed[] = {1.0};

    public Calendar () {
        this.title = "Cairo Vala Demo";
        this.destroy.connect (Gtk.main_quit);
        set_default_size (450, 550);
        create_widgets ();
    }

    private void create_widgets () {
        var drawing_area = new DrawingArea ();
        drawing_area.draw.connect (on_draw);
        add (drawing_area);
    }

	private double time_location (DateTime d) {
		int doy = d.get_day_of_year();
		int hour = d.get_hour();
		int minute = d.get_minute();
		int second = d.get_second();

		int seconds =
			(hour * 60 * 60) + (minute * 60) + second;

		return ((double)seconds / 86400.0);
	}

    private void draw_event (Context ctx, int sx, int sy, Event ev, int width, int height) {
		double sloc = time_location(ev.start);
		double eloc = time_location(ev.end);
		double dloc = eloc - sloc;
		double eheight = dloc * height;

		// double height = Math.fmin(, MIN_EVENT_HEIGHT);
        // stdout.printf("sloc %f eloc %f dloc %f eheight %f\n",
		// 			  sloc, eloc, dloc, eheight);

		ctx.move_to(sx, (sloc * height)+sy);
		ctx.set_source_rgba(1.0, 0, 0, 0.25);
		rectangle(ctx, (double)width, eheight);
		ctx.fill();
		ctx.set_source_rgb(0.5, 0, 0);
		ctx.stroke();
		ctx.move_to(sx + EVPAD, (sloc * height)+EVPAD+TXTPAD);
		ctx.show_text(ev.title);
	}

	private void draw_hours (Context ctx, int sy, int width, int height) {
		double section_height = ((double)height) / 48.0;
		char[] buffer;
		const double col = 0.4;
        ctx.set_source_rgb (col, col, col);
		ctx.set_line_width (1);

		for (int section = 0; section < 48; section++) {
			int minutes = section * 30;
			double y = sy + ((double)section) * section_height;
			ctx.move_to (0, y);
			ctx.rel_line_to (width, 0);

			if (section % 2 == 0)
			  ctx.set_dash ({}, 0);
			else
			  ctx.set_dash (dashed, 0);

			ctx.stroke();

			bool onhour = ((minutes / 30) % 2) == 0;
			if (onhour) {
			  var t = Time();
			  t.minute = 0;
			  t.hour = minutes / 60;
			  var strtime = t.format("%H:%M");
			  // TODO: text extents for proper time placement?
			  ctx.move_to(1.0, y+TXTPAD);
			  ctx.show_text(strtime);
			}
		}
	}

	private void draw_background (Context ctx, int width, int height) {
        ctx.set_source_rgb (0.3, 0.3, 0.3);
	    rectangle(ctx, width, height);
		ctx.fill();
	}

    private bool on_draw (Widget da, Context ctx) {
		int width, height;
		var ev = new Event();
		ev.title = "Coding this";
		ev.start = new DateTime.now_local();
		ev.end = new DateTime.now_local().add_hours(1);
		this.get_size(out width, out height);
		width -= LMARGIN+GAP; height -= GAP*2;

		ctx.move_to(LMARGIN, GAP);
		draw_background(ctx, width, height);

		// ctx.move_to (GAP, GAP);
		draw_hours(ctx, GAP, width+LMARGIN, height);

		draw_event(ctx, LMARGIN, GAP, ev, width, height);

        return true;
    }

    private void rectangle (Context ctx, double x, double y) {
        ctx.rel_line_to (x, 0);
        ctx.rel_line_to (0, y);
        ctx.rel_line_to (-x, 0);
        ctx.close_path ();
    }

    static int main (string[] args) {
        Gtk.init (ref args);

        var cairo_sample = new Calendar ();
        cairo_sample.show_all ();

        Gtk.main ();

        return 0;
    }
}