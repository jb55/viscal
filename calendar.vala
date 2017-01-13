
using Gtk;
using Cairo;

public class Calendar : Gtk.Window {

    private const int SIZE = 30;
    private const int GAP = 20;
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

    private void draw_event (Context ctx, Event ev) {
	}

	private void draw_hours (Context ctx, int width, int height) {
		double hour_height = ((double)height) / 24.0;
		const double col = 0.35;
        ctx.set_source_rgb (col, col, col);
		ctx.set_line_width (1);
		for (int hour = 1; hour < 24; hour++) {
			double y = ((double)hour) * hour_height;
			ctx.move_to (GAP, y+GAP);
			ctx.rel_line_to (width, 0);

			if (hour % 2 == 0)
			  ctx.set_dash (dashed, 0);
			else
			  ctx.set_dash ({}, 0);

			ctx.stroke();
		}
	}

	private void draw_background (Context ctx, int width, int height) {
        ctx.set_source_rgb (0.3, 0.3, 0.3);
	    rectangle(ctx, width, height);
		ctx.fill();
	}

    private bool on_draw (Widget da, Context ctx) {
		int width, height;
		this.get_size(out width, out height);
		width -= GAP*2; height -= GAP*2;

		ctx.move_to (GAP, GAP);
		draw_background(ctx, width, height);

		// ctx.move_to (GAP, GAP);
		draw_hours(ctx, width, height);

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