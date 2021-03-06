/* PSPP - a program for statistical analysis.
   Copyright (C) 2009, 2010, 2011, 2012, 2013, 2014, 2015, 2016 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <config.h>

#include "output/cairo.h"

#include "libpspp/assertion.h"
#include "libpspp/cast.h"
#include "libpspp/message.h"
#include "libpspp/pool.h"
#include "libpspp/start-date.h"
#include "libpspp/str.h"
#include "libpspp/string-map.h"
#include "libpspp/version.h"
#include "data/file-handle-def.h"
#include "output/cairo-chart.h"
#include "output/chart-item-provider.h"
#include "output/charts/boxplot.h"
#include "output/charts/np-plot.h"
#include "output/charts/piechart.h"
#include "output/charts/barchart.h"
#include "output/charts/plot-hist.h"
#include "output/charts/roc-chart.h"
#include "output/charts/spreadlevel-plot.h"
#include "output/charts/scree.h"
#include "output/charts/scatterplot.h"
#include "output/driver-provider.h"
#include "output/group-item.h"
#include "output/message-item.h"
#include "output/options.h"
#include "output/page-setup-item.h"
#include "output/render.h"
#include "output/table-item.h"
#include "output/table.h"
#include "output/text-item.h"

#include <cairo/cairo-pdf.h>
#include <cairo/cairo-ps.h>
#include <cairo/cairo-svg.h>
#include <cairo/cairo.h>
#include <math.h>
#include <pango/pango-font.h>
#include <pango/pango-layout.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>

#include "gl/c-ctype.h"
#include "gl/c-strcase.h"
#include "gl/intprops.h"
#include "gl/minmax.h"
#include "gl/xalloc.h"

#include "gettext.h"
#define _(msgid) gettext (msgid)

/* This file uses TABLE_HORZ and TABLE_VERT enough to warrant abbreviating. */
#define H TABLE_HORZ
#define V TABLE_VERT

/* The unit used for internal measurements is inch/(72 * XR_POINT).
   (Thus, XR_POINT units represent one point.) */
#define XR_POINT PANGO_SCALE

/* Conversions to and from points. */
static double
xr_to_pt (int x)
{
  return x / (double) XR_POINT;
}

/* Conversion from 1/96" units ("pixels") to Cairo/Pango units. */
static int
px_to_xr (int x)
{
  return x * (PANGO_SCALE * 72 / 96);
}

/* Dimensions for drawing lines in tables. */
#define XR_LINE_WIDTH (XR_POINT / 2) /* Width of an ordinary line. */
#define XR_LINE_SPACE XR_POINT       /* Space between double lines. */

/* Output types. */
enum xr_output_type
  {
    XR_PDF,
    XR_PS,
    XR_SVG
  };

/* Cairo fonts. */
enum xr_font_type
  {
    XR_FONT_PROPORTIONAL,
    XR_FONT_EMPHASIS,
    XR_FONT_FIXED,
    XR_N_FONTS
  };

/* A font for use with Cairo. */
struct xr_font
  {
    PangoFontDescription *desc;
    PangoLayout *layout;
  };

/* An output item whose rendering is in progress. */
struct xr_render_fsm
  {
    /* Renders as much of itself as it can on the current page.  Returns true
       if rendering is complete, false if the output item needs another
       page. */
    bool (*render) (struct xr_render_fsm *, struct xr_driver *);

    /* Destroys the output item. */
    void (*destroy) (struct xr_render_fsm *);
  };

/* Cairo output driver. */
struct xr_driver
  {
    struct output_driver driver;

    /* User parameters. */
    struct xr_font fonts[XR_N_FONTS];

    int width;                  /* Page width minus margins. */
    int length;                 /* Page length minus margins and header. */

    int left_margin;            /* Left margin in inch/(72 * XR_POINT). */
    int right_margin;           /* Right margin in inch/(72 * XR_POINT). */
    int top_margin;             /* Top margin in inch/(72 * XR_POINT). */
    int bottom_margin;          /* Bottom margin in inch/(72 * XR_POINT). */

    int line_space;		/* Space between lines. */
    int line_width;		/* Width of lines. */

    int min_break[TABLE_N_AXES]; /* Min cell size to break across pages. */
    int object_spacing;         /* Space between output objects. */

    struct xr_color bg;    /* Background color */
    struct xr_color fg;    /* Foreground color */

    int initial_page_number;

    struct page_heading headings[2]; /* Top and bottom headings. */
    int headings_height[2];

    /* Internal state. */
    struct render_params *params;
    int char_width, char_height;
    char *command_name;
    char *title;
    char *subtitle;
    cairo_t *cairo;
    cairo_surface_t *surface;
    int page_number;		/* Current page number. */
    int x, y;
    struct xr_render_fsm *fsm;
    int nest;
    struct string_map heading_vars;
  };

static const struct output_driver_class cairo_driver_class;

static void xr_driver_destroy_fsm (struct xr_driver *);
static void xr_driver_run_fsm (struct xr_driver *);

static void xr_draw_line (void *, int bb[TABLE_N_AXES][2],
                          enum render_line_style styles[TABLE_N_AXES][2],
                          struct cell_color colors[TABLE_N_AXES][2]);
static void xr_measure_cell_width (void *, const struct table_cell *,
                                   int *min, int *max);
static int xr_measure_cell_height (void *, const struct table_cell *,
                                   int width);
static void xr_draw_cell (void *, const struct table_cell *, int color_idx,
                          int bb[TABLE_N_AXES][2],
                          int spill[TABLE_N_AXES][2],
                          int clip[TABLE_N_AXES][2]);
static int xr_adjust_break (void *, const struct table_cell *,
                            int width, int height);

static struct xr_render_fsm *xr_render_output_item (
  struct xr_driver *, const struct output_item *);

/* Output driver basics. */

static struct xr_driver *
xr_driver_cast (struct output_driver *driver)
{
  assert (driver->class == &cairo_driver_class);
  return UP_CAST (driver, struct xr_driver, driver);
}

static struct driver_option *
opt (struct output_driver *d, struct string_map *options, const char *key,
     const char *default_value)
{
  return driver_option_get (d, options, key, default_value);
}

/* Parse color information specified by KEY into {RED,GREEN,BLUE}.
   Currently, the input string must be of the form "#RRRRGGGGBBBB"
   Future implementations might allow things like "yellow" and
   "sky-blue-ultra-brown"
*/
void
parse_color (struct output_driver *d, struct string_map *options,
	     const char *key, const char *default_value,
	     struct xr_color *color)
{
  int red, green, blue;
  char *string = parse_string (opt (d, options, key, default_value));

  if (3 != sscanf (string, "#%04x%04x%04x", &red, &green, &blue))
    {
      /* If the parsed option string fails, then try the default value */
      if ( 3 != sscanf (default_value, "#%04x%04x%04x", &red, &green, &blue))
	{
	  /* ... and if that fails set everything to zero */
	  red = green = blue = 0;
	}
    }

  free (string);

  /* Convert 16 bit ints to float */
  color->red = red / (double) 0xFFFF;
  color->green = green / (double) 0xFFFF;
  color->blue = blue / (double) 0xFFFF;
}

static PangoFontDescription *
parse_font (const char *font, int default_size, bool bold, bool italic)
{
  if (!c_strcasecmp (font, "Monospaced"))
    font = "Monospace";

  PangoFontDescription *desc = pango_font_description_from_string (font);
  if (desc == NULL)
    return NULL;

  /* If the font description didn't include an explicit font size, then set it
     to DEFAULT_SIZE, which is in inch/72000 units. */
  if (!(pango_font_description_get_set_fields (desc) & PANGO_FONT_MASK_SIZE))
    pango_font_description_set_size (desc,
                                     (default_size / 1000.0) * PANGO_SCALE);

  pango_font_description_set_weight (desc, (bold
                                            ? PANGO_WEIGHT_BOLD
                                            : PANGO_WEIGHT_NORMAL));
  pango_font_description_set_style (desc, (italic
                                           ? PANGO_STYLE_ITALIC
                                           : PANGO_STYLE_NORMAL));

  return desc;
}

static PangoFontDescription *
parse_font_option (struct output_driver *d, struct string_map *options,
                   const char *key, const char *default_value,
                   int default_size, bool bold, bool italic)
{
  char *string = parse_string (opt (d, options, key, default_value));
  PangoFontDescription *desc = parse_font (string, default_size, bold, italic);
  if (!desc)
    {
      msg (MW, _("`%s': bad font specification"), string);

      /* Fall back to DEFAULT_VALUE, which had better be a valid font
         description. */
      desc = parse_font (default_value, default_size, bold, italic);
      assert (desc != NULL);
    }
  free (string);

  return desc;
}

static void
apply_options (struct xr_driver *xr, struct string_map *o)
{
  struct output_driver *d = &xr->driver;

  /* In inch/72000 units used by parse_paper_size() and parse_dimension(). */
  int left_margin, right_margin;
  int top_margin, bottom_margin;
  int paper_width, paper_length;
  int font_size;
  int min_break[TABLE_N_AXES];

  /* Scale factor from inch/72000 to inch/(72 * XR_POINT). */
  const double scale = XR_POINT / 1000.;

  int i;

  for (i = 0; i < XR_N_FONTS; i++)
    {
      struct xr_font *font = &xr->fonts[i];

      if (font->desc != NULL)
        pango_font_description_free (font->desc);
    }

  font_size = parse_int (opt (d, o, "font-size", "10000"), 1000, 1000000);
  xr->fonts[XR_FONT_FIXED].desc = parse_font_option
    (d, o, "fixed-font", "monospace", font_size, false, false);
  xr->fonts[XR_FONT_PROPORTIONAL].desc = parse_font_option (
    d, o, "prop-font", "sans serif", font_size, false, false);
  xr->fonts[XR_FONT_EMPHASIS].desc = parse_font_option (
    d, o, "emph-font", "sans serif", font_size, false, true);

  parse_color (d, o, "background-color", "#FFFFFFFFFFFF", &xr->bg);
  parse_color (d, o, "foreground-color", "#000000000000", &xr->fg);

  /* Get dimensions.  */
  parse_paper_size (opt (d, o, "paper-size", ""), &paper_width, &paper_length);
  left_margin = parse_dimension (opt (d, o, "left-margin", ".5in"));
  right_margin = parse_dimension (opt (d, o, "right-margin", ".5in"));
  top_margin = parse_dimension (opt (d, o, "top-margin", ".5in"));
  bottom_margin = parse_dimension (opt (d, o, "bottom-margin", ".5in"));

  min_break[H] = parse_dimension (opt (d, o, "min-hbreak", NULL)) * scale;
  min_break[V] = parse_dimension (opt (d, o, "min-vbreak", NULL)) * scale;

  int object_spacing = (parse_dimension (opt (d, o, "object-spacing", NULL))
                        * scale);

  /* Convert to inch/(XR_POINT * 72). */
  xr->left_margin = left_margin * scale;
  xr->right_margin = right_margin * scale;
  xr->top_margin = top_margin * scale;
  xr->bottom_margin = bottom_margin * scale;
  xr->width = (paper_width - left_margin - right_margin) * scale;
  xr->length = (paper_length - top_margin - bottom_margin) * scale;
  xr->min_break[H] = min_break[H] >= 0 ? min_break[H] : xr->width / 2;
  xr->min_break[V] = min_break[V] >= 0 ? min_break[V] : xr->length / 2;
  xr->object_spacing = object_spacing >= 0 ? object_spacing : XR_POINT * 12;

  /* There are no headings so headings_height can stay 0. */
}

static struct xr_driver *
xr_allocate (const char *name, int device_type, struct string_map *o)
{
  struct xr_driver *xr = xzalloc (sizeof *xr);
  struct output_driver *d = &xr->driver;

  output_driver_init (d, &cairo_driver_class, name, device_type);

  string_map_init (&xr->heading_vars);

  apply_options (xr, o);

  return xr;
}

static int
pango_to_xr (int pango)
{
  return (XR_POINT != PANGO_SCALE
          ? ceil (pango * (1. * XR_POINT / PANGO_SCALE))
          : pango);
}

static int
xr_to_pango (int xr)
{
  return (XR_POINT != PANGO_SCALE
          ? ceil (xr * (1. / XR_POINT * PANGO_SCALE))
          : xr);
}

static void
xr_measure_fonts (cairo_t *cairo, const struct xr_font fonts[XR_N_FONTS],
                  int *char_width, int *char_height)
{
  *char_width = 0;
  *char_height = 0;
  for (int i = 0; i < XR_N_FONTS; i++)
    {
      PangoLayout *layout = pango_cairo_create_layout (cairo);
      pango_layout_set_font_description (layout, fonts[i].desc);

      pango_layout_set_text (layout, "0", 1);

      int cw, ch;
      pango_layout_get_size (layout, &cw, &ch);
      *char_width = MAX (*char_width, pango_to_xr (cw));
      *char_height = MAX (*char_height, pango_to_xr (ch));

      g_object_unref (G_OBJECT (layout));
    }
}

static int
get_layout_height (PangoLayout *layout)
{
  int w, h;
  pango_layout_get_size (layout, &w, &h);
  return h;
}

static int
xr_render_page_heading (cairo_t *cairo, const PangoFontDescription *font,
                        const struct page_heading *ph, int page_number,
                        int width, bool draw, int base_y)
{
  PangoLayout *layout = pango_cairo_create_layout (cairo);
  pango_layout_set_font_description (layout, font);

  int y = 0;
  for (size_t i = 0; i < ph->n; i++)
    {
      const struct page_paragraph *pp = &ph->paragraphs[i];

      char *markup = output_driver_substitute_heading_vars (pp->markup,
                                                            page_number);
      pango_layout_set_markup (layout, markup, -1);
      free (markup);

      pango_layout_set_alignment (
        layout,
        (pp->halign == TABLE_HALIGN_LEFT ? PANGO_ALIGN_LEFT
         : pp->halign == TABLE_HALIGN_CENTER ? PANGO_ALIGN_CENTER
         : pp->halign == TABLE_HALIGN_MIXED ? PANGO_ALIGN_LEFT
         : PANGO_ALIGN_RIGHT));
      pango_layout_set_width (layout, xr_to_pango (width));
      if (draw)
        {
          cairo_save (cairo);
          cairo_translate (cairo, 0, xr_to_pt (y + base_y));
          pango_cairo_show_layout (cairo, layout);
          cairo_restore (cairo);
        }

      y += pango_to_xr (get_layout_height (layout));
    }

  g_object_unref (G_OBJECT (layout));

  return y;
}

static int
xr_measure_headings (cairo_surface_t *surface,
                     const PangoFontDescription *font,
                     const struct page_heading headings[2],
                     int width, int object_spacing, int height[2])
{
  cairo_t *cairo = cairo_create (surface);
  int total = 0;
  for (int i = 0; i < 2; i++)
    {
      int h = xr_render_page_heading (cairo, font, &headings[i], -1,
                                      width, false, 0);

      /* If the top heading is nonempty, add some space below it. */
      if (h && i == 0)
        h += object_spacing;

      if (height)
        height[i] = h;
      total += h;
    }
  cairo_destroy (cairo);
  return total;
}

static bool
xr_check_fonts (cairo_surface_t *surface,
                const struct xr_font fonts[XR_N_FONTS],
                int usable_width, int usable_length)
{
  cairo_t *cairo = cairo_create (surface);
  int char_width, char_height;
  xr_measure_fonts (cairo, fonts, &char_width, &char_height);
  cairo_destroy (cairo);

  bool ok = true;
  enum { MIN_WIDTH = 3, MIN_LENGTH = 3 };
  if (usable_width / char_width < MIN_WIDTH)
    {
      msg (ME, _("The defined page is not wide enough to hold at least %d "
                 "characters in the default font.  In fact, there's only "
                 "room for %d characters."),
           MIN_WIDTH, usable_width / char_width);
      ok = false;
    }
  if (usable_length / char_height < MIN_LENGTH)
    {
      msg (ME, _("The defined page is not long enough to hold at least %d "
                 "lines in the default font.  In fact, there's only "
                 "room for %d lines."),
           MIN_LENGTH, usable_length / char_height);
      ok = false;
    }
  return ok;
}

static void
xr_set_cairo (struct xr_driver *xr, cairo_t *cairo)
{
  xr->cairo = cairo;

  cairo_set_line_width (xr->cairo, xr_to_pt (XR_LINE_WIDTH));

  xr_measure_fonts (xr->cairo, xr->fonts, &xr->char_width, &xr->char_height);

  for (int i = 0; i < XR_N_FONTS; i++)
    {
      struct xr_font *font = &xr->fonts[i];
      font->layout = pango_cairo_create_layout (cairo);
      pango_layout_set_font_description (font->layout, font->desc);
    }

  if (xr->params == NULL)
    {
      xr->params = xmalloc (sizeof *xr->params);
      xr->params->draw_line = xr_draw_line;
      xr->params->measure_cell_width = xr_measure_cell_width;
      xr->params->measure_cell_height = xr_measure_cell_height;
      xr->params->adjust_break = xr_adjust_break;
      xr->params->draw_cell = xr_draw_cell;
      xr->params->aux = xr;
      xr->params->size[H] = xr->width;
      xr->params->size[V] = xr->length;
      xr->params->font_size[H] = xr->char_width;
      xr->params->font_size[V] = xr->char_height;

      int lw = XR_LINE_WIDTH;
      int ls = XR_LINE_SPACE;
      for (int i = 0; i < TABLE_N_AXES; i++)
        {
          xr->params->line_widths[i][RENDER_LINE_NONE] = 0;
          xr->params->line_widths[i][RENDER_LINE_SINGLE] = lw;
          xr->params->line_widths[i][RENDER_LINE_DASHED] = lw;
          xr->params->line_widths[i][RENDER_LINE_THICK] = lw * 2;
          xr->params->line_widths[i][RENDER_LINE_THIN] = lw / 2;
          xr->params->line_widths[i][RENDER_LINE_DOUBLE] = 2 * lw + ls;
        }

      for (int i = 0; i < TABLE_N_AXES; i++)
        xr->params->min_break[i] = xr->min_break[i];
      xr->params->supports_margins = true;
      xr->params->rtl = render_direction_rtl ();
    }

  cairo_set_source_rgb (xr->cairo, xr->fg.red, xr->fg.green, xr->fg.blue);
}

static struct output_driver *
xr_create (const char *file_name, enum settings_output_devices device_type,
           struct string_map *o, enum xr_output_type file_type)
{
  struct xr_driver *xr;
  cairo_status_t status;
  double width_pt, length_pt;

  xr = xr_allocate (file_name, device_type, o);

  width_pt = xr_to_pt (xr->width + xr->left_margin + xr->right_margin);
  length_pt = xr_to_pt (xr->length + xr->top_margin + xr->bottom_margin);
  if (file_type == XR_PDF)
    xr->surface = cairo_pdf_surface_create (file_name, width_pt, length_pt);
  else if (file_type == XR_PS)
    xr->surface = cairo_ps_surface_create (file_name, width_pt, length_pt);
  else if (file_type == XR_SVG)
    xr->surface = cairo_svg_surface_create (file_name, width_pt, length_pt);
  else
    NOT_REACHED ();

  status = cairo_surface_status (xr->surface);
  if (status != CAIRO_STATUS_SUCCESS)
    {
      msg (ME, _("error opening output file `%s': %s"),
             file_name, cairo_status_to_string (status));
      goto error;
    }

  if (!xr_check_fonts (xr->surface, xr->fonts, xr->width, xr->length))
    goto error;

  return &xr->driver;

 error:
  output_driver_destroy (&xr->driver);
  return NULL;
}

static struct output_driver *
xr_pdf_create (struct  file_handle *fh, enum settings_output_devices device_type,
               struct string_map *o)
{
  struct output_driver *od = xr_create (fh_get_file_name (fh), device_type, o, XR_PDF);
  fh_unref (fh);
  return od ;
}

static struct output_driver *
xr_ps_create (struct  file_handle *fh, enum settings_output_devices device_type,
               struct string_map *o)
{
  struct output_driver *od =  xr_create (fh_get_file_name (fh), device_type, o, XR_PS);
  fh_unref (fh);
  return od ;
}

static struct output_driver *
xr_svg_create (struct file_handle *fh, enum settings_output_devices device_type,
               struct string_map *o)
{
  struct output_driver *od = xr_create (fh_get_file_name (fh), device_type, o, XR_SVG);
  fh_unref (fh);
  return od ;
}

static void
xr_destroy (struct output_driver *driver)
{
  struct xr_driver *xr = xr_driver_cast (driver);
  size_t i;

  xr_driver_destroy_fsm (xr);

  if (xr->cairo != NULL)
    {
      cairo_surface_finish (xr->surface);
      cairo_status_t status = cairo_status (xr->cairo);
      if (status != CAIRO_STATUS_SUCCESS)
        msg (ME, _("error drawing output for %s driver: %s"),
               output_driver_get_name (driver),
               cairo_status_to_string (status));
      cairo_surface_destroy (xr->surface);

      cairo_destroy (xr->cairo);
    }

  for (i = 0; i < XR_N_FONTS; i++)
    {
      struct xr_font *font = &xr->fonts[i];

      if (font->desc != NULL)
        pango_font_description_free (font->desc);
      if (font->layout != NULL)
        g_object_unref (font->layout);
    }

  free (xr->params);
  free (xr);
}

static void
xr_flush (struct output_driver *driver)
{
  struct xr_driver *xr = xr_driver_cast (driver);

  cairo_surface_flush (cairo_get_target (xr->cairo));
}

static void
xr_update_page_setup (struct output_driver *driver,
                      const struct page_setup *ps)
{
  struct xr_driver *xr = xr_driver_cast (driver);

  xr->initial_page_number = ps->initial_page_number;
  xr->object_spacing = ps->object_spacing * 72 * XR_POINT;

  if (xr->cairo)
    return;

  int usable[TABLE_N_AXES];
  for (int i = 0; i < 2; i++)
    usable[i] = (ps->paper[i]
                 - (ps->margins[i][0] + ps->margins[i][1])) * 72 * XR_POINT;

  int headings_height[2];
  usable[V] -= xr_measure_headings (
    xr->surface, xr->fonts[XR_FONT_PROPORTIONAL].desc, ps->headings,
    usable[H], xr->object_spacing, headings_height);

  enum table_axis h = ps->orientation == PAGE_LANDSCAPE;
  enum table_axis v = !h;
  if (!xr_check_fonts (xr->surface, xr->fonts, usable[h], usable[v]))
    return;

  for (int i = 0; i < 2; i++)
    {
      page_heading_uninit (&xr->headings[i]);
      page_heading_copy (&xr->headings[i], &ps->headings[i]);
      xr->headings_height[i] = headings_height[i];
    }
  xr->width = usable[h];
  xr->length = usable[v];
  xr->left_margin = ps->margins[h][0] * 72 * XR_POINT;
  xr->right_margin = ps->margins[h][1] * 72 * XR_POINT;
  xr->top_margin = ps->margins[v][0] * 72 * XR_POINT;
  xr->bottom_margin = ps->margins[v][1] * 72 * XR_POINT;
  cairo_pdf_surface_set_size (xr->surface,
                              ps->paper[h] * 72.0, ps->paper[v] * 72.0);
}

static void
xr_submit (struct output_driver *driver, const struct output_item *output_item)
{
  struct xr_driver *xr = xr_driver_cast (driver);

  if (is_page_setup_item (output_item))
    {
      xr_update_page_setup (driver,
                            to_page_setup_item (output_item)->page_setup);
      return;
    }

  if (!xr->cairo)
    {
      xr->page_number = xr->initial_page_number - 1;
      xr_set_cairo (xr, cairo_create (xr->surface));
      cairo_save (xr->cairo);
      xr_driver_next_page (xr, xr->cairo);
    }

  xr_driver_output_item (xr, output_item);
  while (xr_driver_need_new_page (xr))
    {
      cairo_restore (xr->cairo);
      cairo_show_page (xr->cairo);
      cairo_save (xr->cairo);
      xr_driver_next_page (xr, xr->cairo);
    }
}

/* Functions for rendering a series of output items to a series of Cairo
   contexts, with pagination.

   Used by PSPPIRE for printing, and by the basic Cairo output driver above as
   its underlying implementation.

   See the big comment in cairo.h for intended usage. */

/* Gives new page CAIRO to XR for output. */
void
xr_driver_next_page (struct xr_driver *xr, cairo_t *cairo)
{
  cairo_save (cairo);
  cairo_set_source_rgb (cairo, xr->bg.red, xr->bg.green, xr->bg.blue);
  cairo_rectangle (cairo, 0, 0, xr->width, xr->length);
  cairo_fill (cairo);
  cairo_restore (cairo);

  cairo_translate (cairo,
                   xr_to_pt (xr->left_margin),
                   xr_to_pt (xr->top_margin + xr->headings_height[0]));

  xr->page_number++;
  xr->cairo = cairo;
  xr->x = xr->y = 0;

  xr_render_page_heading (xr->cairo, xr->fonts[XR_FONT_PROPORTIONAL].desc,
                          &xr->headings[0], xr->page_number, xr->width, true,
                          -xr->headings_height[0]);
  xr_render_page_heading (xr->cairo, xr->fonts[XR_FONT_PROPORTIONAL].desc,
                          &xr->headings[1], xr->page_number, xr->width, true,
                          xr->length);

  xr_driver_run_fsm (xr);
}

/* Start rendering OUTPUT_ITEM to XR.  Only valid if XR is not in the middle of
   rendering a previous output item, that is, only if xr_driver_need_new_page()
   returns false. */
void
xr_driver_output_item (struct xr_driver *xr,
                       const struct output_item *output_item)
{
  assert (xr->fsm == NULL);
  xr->fsm = xr_render_output_item (xr, output_item);
  xr_driver_run_fsm (xr);
}

/* Returns true if XR is in the middle of rendering an output item and needs a
   new page to be appended using xr_driver_next_page() to make progress,
   otherwise false. */
bool
xr_driver_need_new_page (const struct xr_driver *xr)
{
  return xr->fsm != NULL;
}

/* Returns true if the current page doesn't have any content yet. */
bool
xr_driver_is_page_blank (const struct xr_driver *xr)
{
  return xr->y == 0;
}

static void
xr_driver_destroy_fsm (struct xr_driver *xr)
{
  if (xr->fsm != NULL)
    {
      xr->fsm->destroy (xr->fsm);
      xr->fsm = NULL;
    }
}

static void
xr_driver_run_fsm (struct xr_driver *xr)
{
  if (xr->fsm != NULL && !xr->fsm->render (xr->fsm, xr))
    xr_driver_destroy_fsm (xr);
}

static void
xr_layout_cell (struct xr_driver *, const struct table_cell *,
                int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2],
                int *width, int *height, int *brk);

static void
set_source_rgba (cairo_t *cairo, const struct cell_color *color)
{
  cairo_set_source_rgba (cairo,
                         color->r / 255., color->g / 255., color->b / 255.,
                         color->alpha / 255.);
}

static void
dump_line (struct xr_driver *xr, int x0, int y0, int x1, int y1, int style,
           const struct cell_color *color)
{
  cairo_new_path (xr->cairo);
  set_source_rgba (xr->cairo, color);
  cairo_set_line_width (
    xr->cairo,
    xr_to_pt (style == RENDER_LINE_THICK ? XR_LINE_WIDTH * 2
              : style == RENDER_LINE_THIN ? XR_LINE_WIDTH / 2
              : XR_LINE_WIDTH));
  cairo_move_to (xr->cairo, xr_to_pt (x0 + xr->x), xr_to_pt (y0 + xr->y));
  cairo_line_to (xr->cairo, xr_to_pt (x1 + xr->x), xr_to_pt (y1 + xr->y));
  cairo_stroke (xr->cairo);
}

static void UNUSED
dump_rectangle (struct xr_driver *xr, int x0, int y0, int x1, int y1)
{
  cairo_new_path (xr->cairo);
  cairo_set_line_width (xr->cairo, xr_to_pt (XR_LINE_WIDTH));
  cairo_move_to (xr->cairo, xr_to_pt (x0 + xr->x), xr_to_pt (y0 + xr->y));
  cairo_line_to (xr->cairo, xr_to_pt (x1 + xr->x), xr_to_pt (y0 + xr->y));
  cairo_line_to (xr->cairo, xr_to_pt (x1 + xr->x), xr_to_pt (y1 + xr->y));
  cairo_line_to (xr->cairo, xr_to_pt (x0 + xr->x), xr_to_pt (y1 + xr->y));
  cairo_close_path (xr->cairo);
  cairo_stroke (xr->cairo);
}

static void
fill_rectangle (struct xr_driver *xr, int x0, int y0, int x1, int y1)
{
  cairo_new_path (xr->cairo);
  cairo_set_line_width (xr->cairo, xr_to_pt (XR_LINE_WIDTH));
  cairo_rectangle (xr->cairo,
                   xr_to_pt (x0 + xr->x), xr_to_pt (y0 + xr->y),
                   xr_to_pt (x1 - x0), xr_to_pt (y1 - y0));
  cairo_fill (xr->cairo);
}

/* Draws a horizontal line X0...X2 at Y if LEFT says so,
   shortening it to X0...X1 if SHORTEN is true.
   Draws a horizontal line X1...X3 at Y if RIGHT says so,
   shortening it to X2...X3 if SHORTEN is true. */
static void
horz_line (struct xr_driver *xr, int x0, int x1, int x2, int x3, int y,
           enum render_line_style left, enum render_line_style right,
           const struct cell_color *left_color,
           const struct cell_color *right_color,
           bool shorten)
{
  if (left != RENDER_LINE_NONE && right != RENDER_LINE_NONE && !shorten
      && cell_color_equal (left_color, right_color))
    dump_line (xr, x0, y, x3, y, left, left_color);
  else
    {
      if (left != RENDER_LINE_NONE)
        dump_line (xr, x0, y, shorten ? x1 : x2, y, left, left_color);
      if (right != RENDER_LINE_NONE)
        dump_line (xr, shorten ? x2 : x1, y, x3, y, right, right_color);
    }
}

/* Draws a vertical line Y0...Y2 at X if TOP says so,
   shortening it to Y0...Y1 if SHORTEN is true.
   Draws a vertical line Y1...Y3 at X if BOTTOM says so,
   shortening it to Y2...Y3 if SHORTEN is true. */
static void
vert_line (struct xr_driver *xr, int y0, int y1, int y2, int y3, int x,
           enum render_line_style top, enum render_line_style bottom,
           const struct cell_color *top_color,
           const struct cell_color *bottom_color,
           bool shorten)
{
  if (top != RENDER_LINE_NONE && bottom != RENDER_LINE_NONE && !shorten
      && cell_color_equal (top_color, bottom_color))
    dump_line (xr, x, y0, x, y3, top, top_color);
  else
    {
      if (top != RENDER_LINE_NONE)
        dump_line (xr, x, y0, x, shorten ? y1 : y2, top, top_color);
      if (bottom != RENDER_LINE_NONE)
        dump_line (xr, x, shorten ? y2 : y1, x, y3, bottom, bottom_color);
    }
}

static void
xr_draw_line (void *xr_, int bb[TABLE_N_AXES][2],
              enum render_line_style styles[TABLE_N_AXES][2],
              struct cell_color colors[TABLE_N_AXES][2])
{
  const int x0 = bb[H][0];
  const int y0 = bb[V][0];
  const int x3 = bb[H][1];
  const int y3 = bb[V][1];
  const int top = styles[H][0];
  const int bottom = styles[H][1];

  int start_side = render_direction_rtl();
  int end_side = !start_side;
  const int start_of_line = styles[V][start_side];
  const int end_of_line   = styles[V][end_side];
  const struct cell_color *top_color = &colors[H][0];
  const struct cell_color *bottom_color = &colors[H][1];
  const struct cell_color *start_color = &colors[V][start_side];
  const struct cell_color *end_color = &colors[V][end_side];

  /* The algorithm here is somewhat subtle, to allow it to handle
     all the kinds of intersections that we need.

     Three additional ordinates are assigned along the x axis.  The
     first is xc, midway between x0 and x3.  The others are x1 and
     x2; for a single vertical line these are equal to xc, and for
     a double vertical line they are the ordinates of the left and
     right half of the double line.

     yc, y1, and y2 are assigned similarly along the y axis.

     The following diagram shows the coordinate system and output
     for double top and bottom lines, single left line, and no
     right line:

                 x0       x1 xc  x2      x3
               y0 ________________________
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
     y1 = y2 = yc |#########     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
               y3 |________#_____#_______|
  */
  struct xr_driver *xr = xr_;

  /* Offset from center of each line in a pair of double lines. */
  int double_line_ofs = (XR_LINE_SPACE + XR_LINE_WIDTH) / 2;

  /* Are the lines along each axis single or double?
     (It doesn't make sense to have different kinds of line on the
     same axis, so we don't try to gracefully handle that case.) */
  bool double_vert = top == RENDER_LINE_DOUBLE || bottom == RENDER_LINE_DOUBLE;
  bool double_horz = start_of_line == RENDER_LINE_DOUBLE || end_of_line == RENDER_LINE_DOUBLE;

  /* When horizontal lines are doubled,
     the left-side line along y1 normally runs from x0 to x2,
     and the right-side line along y1 from x3 to x1.
     If the top-side line is also doubled, we shorten the y1 lines,
     so that the left-side line runs only to x1,
     and the right-side line only to x2.
     Otherwise, the horizontal line at y = y1 below would cut off
     the intersection, which looks ugly:
               x0       x1     x2      x3
             y0 ________________________
                |        #     #       |
                |        #     #       |
                |        #     #       |
                |        #     #       |
             y1 |#########     ########|
                |                      |
                |                      |
             y2 |######################|
                |                      |
                |                      |
             y3 |______________________|
     It is more of a judgment call when the horizontal line is
     single.  We actually choose to cut off the line anyhow, as
     shown in the first diagram above.
  */
  bool shorten_y1_lines = top == RENDER_LINE_DOUBLE;
  bool shorten_y2_lines = bottom == RENDER_LINE_DOUBLE;
  bool shorten_yc_line = shorten_y1_lines && shorten_y2_lines;
  int horz_line_ofs = double_vert ? double_line_ofs : 0;
  int xc = (x0 + x3) / 2;
  int x1 = xc - horz_line_ofs;
  int x2 = xc + horz_line_ofs;

  bool shorten_x1_lines = start_of_line == RENDER_LINE_DOUBLE;
  bool shorten_x2_lines = end_of_line == RENDER_LINE_DOUBLE;
  bool shorten_xc_line = shorten_x1_lines && shorten_x2_lines;
  int vert_line_ofs = double_horz ? double_line_ofs : 0;
  int yc = (y0 + y3) / 2;
  int y1 = yc - vert_line_ofs;
  int y2 = yc + vert_line_ofs;

  if (!double_horz)
    horz_line (xr, x0, x1, x2, x3, yc, start_of_line, end_of_line,
               start_color, end_color, shorten_yc_line);
  else
    {
      horz_line (xr, x0, x1, x2, x3, y1, start_of_line, end_of_line,
                 start_color, end_color, shorten_y1_lines);
      horz_line (xr, x0, x1, x2, x3, y2, start_of_line, end_of_line,
                 start_color, end_color, shorten_y2_lines);
    }

  if (!double_vert)
    vert_line (xr, y0, y1, y2, y3, xc, top, bottom, top_color, bottom_color,
               shorten_xc_line);
  else
    {
      vert_line (xr, y0, y1, y2, y3, x1, top, bottom, top_color, bottom_color,
                 shorten_x1_lines);
      vert_line (xr, y0, y1, y2, y3, x2, top, bottom, top_color, bottom_color,
                 shorten_x2_lines);
    }
}

static void
xr_measure_cell_width (void *xr_, const struct table_cell *cell,
                       int *min_width, int *max_width)
{
  struct xr_driver *xr = xr_;
  int bb[TABLE_N_AXES][2];
  int clip[TABLE_N_AXES][2];
  int h;

  bb[H][0] = 0;
  bb[H][1] = INT_MAX;
  bb[V][0] = 0;
  bb[V][1] = INT_MAX;
  clip[H][0] = clip[H][1] = clip[V][0] = clip[V][1] = 0;
  xr_layout_cell (xr, cell, bb, clip, max_width, &h, NULL);

  bb[H][1] = 1;
  xr_layout_cell (xr, cell, bb, clip, min_width, &h, NULL);

  if (*min_width > 0)
    *min_width += px_to_xr (cell->style->cell_style.margin[H][0]
                            + cell->style->cell_style.margin[H][1]);
  if (*max_width > 0)
    *max_width += px_to_xr (cell->style->cell_style.margin[H][0]
                            + cell->style->cell_style.margin[H][1]);
}

static int
xr_measure_cell_height (void *xr_, const struct table_cell *cell, int width)
{
  struct xr_driver *xr = xr_;
  int bb[TABLE_N_AXES][2];
  int clip[TABLE_N_AXES][2];
  int w, h;

  bb[H][0] = 0;
  bb[H][1] = width - px_to_xr (cell->style->cell_style.margin[H][0]
                               + cell->style->cell_style.margin[H][1]);
  bb[V][0] = 0;
  bb[V][1] = INT_MAX;
  clip[H][0] = clip[H][1] = clip[V][0] = clip[V][1] = 0;
  xr_layout_cell (xr, cell, bb, clip, &w, &h, NULL);
  h += px_to_xr (cell->style->cell_style.margin[V][0]
                 + cell->style->cell_style.margin[V][1]);
  return h;
}

static void xr_clip (struct xr_driver *, int clip[TABLE_N_AXES][2]);

static void
xr_draw_cell (void *xr_, const struct table_cell *cell, int color_idx,
              int bb[TABLE_N_AXES][2],
              int spill[TABLE_N_AXES][2],
              int clip[TABLE_N_AXES][2])
{
  struct xr_driver *xr = xr_;
  int w, h, brk;

  cairo_save (xr->cairo);
  int bg_clip[TABLE_N_AXES][2];
  for (int axis = 0; axis < TABLE_N_AXES; axis++)
    {
      bg_clip[axis][0] = clip[axis][0];
      if (bb[axis][0] == clip[axis][0])
        bg_clip[axis][0] -= spill[axis][0];

      bg_clip[axis][1] = clip[axis][1];
      if (bb[axis][1] == clip[axis][1])
        bg_clip[axis][1] += spill[axis][1];
    }
  xr_clip (xr, bg_clip);
  set_source_rgba (xr->cairo, &cell->style->font_style.bg[color_idx]);
  fill_rectangle (xr,
                  bb[H][0] - spill[H][0],
                  bb[V][0] - spill[V][0],
                  bb[H][1] + spill[H][1],
                  bb[V][1] + spill[V][1]);
  cairo_restore (xr->cairo);

  cairo_save (xr->cairo);
  set_source_rgba (xr->cairo, &cell->style->font_style.fg[color_idx]);

  for (int axis = 0; axis < TABLE_N_AXES; axis++)
    {
      bb[axis][0] += px_to_xr (cell->style->cell_style.margin[axis][0]);
      bb[axis][1] -= px_to_xr (cell->style->cell_style.margin[axis][1]);
    }
  if (bb[H][0] < bb[H][1] && bb[V][0] < bb[V][1])
    xr_layout_cell (xr, cell, bb, clip, &w, &h, &brk);
  cairo_restore (xr->cairo);
}

static int
xr_adjust_break (void *xr_, const struct table_cell *cell,
                 int width, int height)
{
  struct xr_driver *xr = xr_;
  int bb[TABLE_N_AXES][2];
  int clip[TABLE_N_AXES][2];
  int w, h, brk;

  if (xr_measure_cell_height (xr_, cell, width) < height)
    return -1;

  bb[H][0] = 0;
  bb[H][1] = width - px_to_xr (cell->style->cell_style.margin[H][0]
                               + cell->style->cell_style.margin[H][1]);
  if (bb[H][1] <= 0)
    return 0;
  bb[V][0] = 0;
  bb[V][1] = height - px_to_xr (cell->style->cell_style.margin[V][0]
                                + cell->style->cell_style.margin[V][1]);
  clip[H][0] = clip[H][1] = clip[V][0] = clip[V][1] = 0;
  xr_layout_cell (xr, cell, bb, clip, &w, &h, &brk);
  return brk;
}

static void
xr_clip (struct xr_driver *xr, int clip[TABLE_N_AXES][2])
{
  if (clip[H][1] != INT_MAX || clip[V][1] != INT_MAX)
    {
      double x0 = xr_to_pt (clip[H][0] + xr->x);
      double y0 = xr_to_pt (clip[V][0] + xr->y);
      double x1 = xr_to_pt (clip[H][1] + xr->x);
      double y1 = xr_to_pt (clip[V][1] + xr->y);

      cairo_rectangle (xr->cairo, x0, y0, x1 - x0, y1 - y0);
      cairo_clip (xr->cairo);
    }
}

static void
add_attr_with_start (PangoAttrList *list, PangoAttribute *attr, guint start_index)
{
  attr->start_index = start_index;
  pango_attr_list_insert (list, attr);
}

static void
markup_escape (const char *in, struct string *out)
{
  for (int c = *in++; c; c = *in++)
    switch (c)
      {
      case '&':
        ds_put_cstr (out, "&amp;");
        break;
      case '<':
        ds_put_cstr (out, "&lt;");
        break;
      case '>':
        ds_put_cstr (out, "&gt;");
        break;
      default:
        ds_put_byte (out, c);
        break;
      }
}

static int
get_layout_dimension (PangoLayout *layout, enum table_axis axis)
{
  int size[TABLE_N_AXES];
  pango_layout_get_size (layout, &size[H], &size[V]);
  return size[axis];
}

static int
xr_layout_cell_text (struct xr_driver *xr, const struct table_cell *cell,
                     int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2],
                     int *widthp, int *brk)
{
  const struct font_style *font_style = &cell->style->font_style;
  const struct cell_style *cell_style = &cell->style->cell_style;
  unsigned int options = cell->options;

  enum table_axis X = options & TAB_ROTATE ? V : H;
  enum table_axis Y = !X;
  int R = options & TAB_ROTATE ? 0 : 1;

  struct xr_font *font = (options & TAB_FIX ? &xr->fonts[XR_FONT_FIXED]
                          : &xr->fonts[XR_FONT_PROPORTIONAL]);
  struct xr_font local_font;
  if (font_style->typeface)
    {
      PangoFontDescription *desc = parse_font (
        font_style->typeface,
        font_style->size ? font_style->size * 1000 * 72 / 128 : 10000,
        font_style->bold, font_style->italic);
      if (desc)
        {
          PangoLayout *layout = pango_cairo_create_layout (xr->cairo);
          pango_layout_set_font_description (layout, desc);

          local_font.desc = desc;
          local_font.layout = layout;
          font = &local_font;
        }
    }

  const char *text = cell->text;
  enum table_halign halign = table_halign_interpret (
    cell_style->halign, cell->options & TAB_NUMERIC);
  if (cell_style->halign == TABLE_HALIGN_DECIMAL && !(options & TAB_ROTATE))
    {
      int margin_adjustment = -px_to_xr (cell_style->decimal_offset);

      const char *decimal = strrchr (text, cell_style->decimal_char);
      if (decimal)
        {
          pango_layout_set_text (font->layout, decimal, strlen (decimal));
          pango_layout_set_width (font->layout, -1);
          margin_adjustment += get_layout_dimension (font->layout, H);
        }

      if (margin_adjustment < 0)
        bb[H][1] += margin_adjustment;
    }

  struct string tmp = DS_EMPTY_INITIALIZER;

  /* Deal with an oddity of the Unicode line-breaking algorithm (or perhaps in
     Pango's implementation of it): it will break after a period or a comma
     that precedes a digit, e.g. in ".000" it will break after the period.
     This code looks for such a situation and inserts a U+2060 WORD JOINER
     to prevent the break.

     This isn't necessary when the decimal point is between two digits
     (e.g. "0.000" won't be broken) or when the display width is not limited so
     that word wrapping won't happen.

     It isn't necessary to look for more than one period or comma, as would
     happen with grouping like 1,234,567.89 or 1.234.567,89 because if groups
     are present then there will always be a digit on both sides of every
     period and comma. */
  if (options & TAB_ROTATE || bb[H][1] != INT_MAX)
    {
      const char *decimal = text + strcspn (text, ".,");
      if (decimal[0]
          && c_isdigit (decimal[1])
          && (decimal == text || !c_isdigit (decimal[-1])))
        {
          ds_extend (&tmp, strlen (text) + 16);
          ds_put_substring (&tmp, ss_buffer (text, decimal - text + 1));
          ds_put_unichar (&tmp, 0x2060 /* U+2060 WORD JOINER */);
          ds_put_cstr (&tmp, decimal + 1);
        }
    }

  if (cell->n_footnotes)
    {
      int footnote_adjustment;
      if (cell->n_footnotes == 1 && halign == TABLE_HALIGN_RIGHT)
        {
          const char *marker = cell->footnotes[0]->marker;
          pango_layout_set_text (font->layout, marker, strlen (marker));

          PangoAttrList *attrs = pango_attr_list_new ();
          pango_attr_list_insert (attrs, pango_attr_rise_new (7000));
          pango_layout_set_attributes (font->layout, attrs);
          pango_attr_list_unref (attrs);

          int w = get_layout_dimension (font->layout, X);
          int right_margin = px_to_xr (cell_style->margin[X][R]);
          footnote_adjustment = MIN (w, right_margin);

          pango_layout_set_attributes (font->layout, NULL);
        }
      else
        footnote_adjustment = px_to_xr (cell_style->margin[X][R]);

      if (R)
        bb[X][R] += footnote_adjustment;
      else
        bb[X][R] -= footnote_adjustment;

      if (ds_is_empty (&tmp))
        {
          ds_extend (&tmp, strlen (text) + 16);
          ds_put_cstr (&tmp, text);
        }
      size_t initial_length = ds_length (&tmp);

      for (size_t i = 0; i < cell->n_footnotes; i++)
        {
          if (i)
            ds_put_byte (&tmp, ',');

          const char *marker = cell->footnotes[i]->marker;
          if (options & TAB_MARKUP)
            markup_escape (marker, &tmp);
          else
            ds_put_cstr (&tmp, marker);
        }

      if (options & TAB_MARKUP)
        pango_layout_set_markup (font->layout,
                                 ds_cstr (&tmp), ds_length (&tmp));
      else
        pango_layout_set_text (font->layout, ds_cstr (&tmp), ds_length (&tmp));

      PangoAttrList *attrs = pango_attr_list_new ();
      if (font_style->underline)
        pango_attr_list_insert (attrs, pango_attr_underline_new (
                               PANGO_UNDERLINE_SINGLE));
      add_attr_with_start (attrs, pango_attr_rise_new (7000), initial_length);
      add_attr_with_start (
        attrs, pango_attr_font_desc_new (font->desc), initial_length);
      pango_layout_set_attributes (font->layout, attrs);
      pango_attr_list_unref (attrs);
    }
  else
    {
      const char *content = ds_is_empty (&tmp) ? text : ds_cstr (&tmp);
      if (options & TAB_MARKUP)
        pango_layout_set_markup (font->layout, content, -1);
      else
        pango_layout_set_text (font->layout, content, -1);

      if (font_style->underline)
        {
          PangoAttrList *attrs = pango_attr_list_new ();
          pango_attr_list_insert (attrs, pango_attr_underline_new (
                                    PANGO_UNDERLINE_SINGLE));
          pango_layout_set_attributes (font->layout, attrs);
          pango_attr_list_unref (attrs);
        }
    }
  ds_destroy (&tmp);

  pango_layout_set_alignment (font->layout,
                              (halign == TABLE_HALIGN_RIGHT ? PANGO_ALIGN_RIGHT
                               : halign == TABLE_HALIGN_LEFT ? PANGO_ALIGN_LEFT
                               : PANGO_ALIGN_CENTER));
  pango_layout_set_width (
    font->layout,
    bb[X][1] == INT_MAX ? -1 : xr_to_pango (bb[X][1] - bb[X][0]));
  pango_layout_set_wrap (font->layout, PANGO_WRAP_WORD);

  if (clip[H][0] != clip[H][1])
    {
      cairo_save (xr->cairo);
      if (!(options & TAB_ROTATE))
        xr_clip (xr, clip);
      if (options & TAB_ROTATE)
        {
          cairo_translate (xr->cairo,
                           xr_to_pt (bb[H][0] + xr->x),
                           xr_to_pt (bb[V][1] + xr->y));
          cairo_rotate (xr->cairo, -M_PI_2);
        }
      else
        cairo_translate (xr->cairo,
                         xr_to_pt (bb[H][0] + xr->x),
                         xr_to_pt (bb[V][0] + xr->y));
      pango_cairo_show_layout (xr->cairo, font->layout);

      /* If enabled, this draws a blue rectangle around the extents of each
         line of text, which can be rather useful for debugging layout
         issues. */
      if (0)
        {
          PangoLayoutIter *iter;
          iter = pango_layout_get_iter (font->layout);
          do
            {
              PangoRectangle extents;

              pango_layout_iter_get_line_extents (iter, &extents, NULL);
              cairo_save (xr->cairo);
              cairo_set_source_rgb (xr->cairo, 1, 0, 0);
              dump_rectangle (xr,
                              pango_to_xr (extents.x) - xr->x,
                              pango_to_xr (extents.y) - xr->y,
                              pango_to_xr (extents.x + extents.width) - xr->x,
                              pango_to_xr (extents.y + extents.height) - xr->y);
              cairo_restore (xr->cairo);
            }
          while (pango_layout_iter_next_line (iter));
          pango_layout_iter_free (iter);
        }

      cairo_restore (xr->cairo);
    }

  int size[TABLE_N_AXES];
  pango_layout_get_size (font->layout, &size[H], &size[V]);
  int w = pango_to_xr (size[X]);
  int h = pango_to_xr (size[Y]);
  if (w > *widthp)
    *widthp = w;
  if (bb[V][0] + h >= bb[V][1] && !(options & TAB_ROTATE))
    {
      PangoLayoutIter *iter;
      int best UNUSED = 0;

      /* Choose a breakpoint between lines instead of in the middle of one. */
      iter = pango_layout_get_iter (font->layout);
      do
        {
          PangoRectangle extents;
          int y0, y1;
          int bottom;

          pango_layout_iter_get_line_extents (iter, NULL, &extents);
          pango_layout_iter_get_line_yrange (iter, &y0, &y1);
          extents.x = pango_to_xr (extents.x);
          extents.y = pango_to_xr (y0);
          extents.width = pango_to_xr (extents.width);
          extents.height = pango_to_xr (y1 - y0);
          bottom = bb[V][0] + extents.y + extents.height;
          if (bottom < bb[V][1])
            {
              if (brk && clip[H][0] != clip[H][1])
                best = bottom;
              if (brk)
                *brk = bottom;
            }
          else
            break;
        }
      while (pango_layout_iter_next_line (iter));
      pango_layout_iter_free (iter);

      /* If enabled, draws a green line across the chosen breakpoint, which can
         be useful for debugging issues with breaking.  */
      if (0)
        {
          if (best && !xr->nest)
            dump_line (xr, -xr->left_margin, best,
                       xr->width + xr->right_margin, best,
                       RENDER_LINE_SINGLE,
                       &(struct cell_color) CELL_COLOR (0, 255, 0));
        }
    }

  pango_layout_set_attributes (font->layout, NULL);

  if (font == &local_font)
    {
      g_object_unref (G_OBJECT (font->layout));
      pango_font_description_free (font->desc);
    }

  return h;
}

static void
xr_layout_cell (struct xr_driver *xr, const struct table_cell *cell,
                int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2],
                int *width, int *height, int *brk)
{
  *width = 0;
  *height = 0;

  /* If enabled, draws a blue rectangle around the cell extents, which can be
     useful for debugging layout. */
  if (0)
    {
      if (clip[H][0] != clip[H][1])
        {
          int offset = (xr->nest) * XR_POINT;

          cairo_save (xr->cairo);
          cairo_set_source_rgb (xr->cairo, 0, 0, 1);
          dump_rectangle (xr,
                          bb[H][0] + offset, bb[V][0] + offset,
                          bb[H][1] - offset, bb[V][1] - offset);
          cairo_restore (xr->cairo);
        }
    }

  if (brk)
    *brk = bb[V][0];
  *height = xr_layout_cell_text (xr, cell, bb, clip, width, brk);
}

struct output_driver_factory pdf_driver_factory =
  { "pdf", "pspp.pdf", xr_pdf_create };
struct output_driver_factory ps_driver_factory =
  { "ps", "pspp.ps", xr_ps_create };
struct output_driver_factory svg_driver_factory =
  { "svg", "pspp.svg", xr_svg_create };

static const struct output_driver_class cairo_driver_class =
{
  "cairo",
  xr_destroy,
  xr_submit,
  xr_flush,
};

/* GUI rendering helpers. */

struct xr_rendering
  {
    struct output_item *item;

    /* Table items. */
    struct render_pager *p;
    struct xr_driver *xr;
  };

#define CHART_WIDTH 500
#define CHART_HEIGHT 375



struct xr_driver *
xr_driver_create (cairo_t *cairo, struct string_map *options)
{
  struct xr_driver *xr = xr_allocate ("cairo", 0, options);
  xr_set_cairo (xr, cairo);
  return xr;
}

/* Destroy XR, which should have been created with xr_driver_create().  Any
   cairo_t added to XR is not destroyed, because it is owned by the client. */
void
xr_driver_destroy (struct xr_driver *xr)
{
  if (xr != NULL)
    {
      xr->cairo = NULL;
      output_driver_destroy (&xr->driver);
    }
}

static struct xr_rendering *
xr_rendering_create_text (struct xr_driver *xr, const char *text, cairo_t *cr)
{
  struct table_item *table_item;
  struct xr_rendering *r;

  table_item = table_item_create (table_from_string (TABLE_HALIGN_LEFT, text),
                                  NULL, NULL);
  r = xr_rendering_create (xr, &table_item->output_item, cr);
  table_item_unref (table_item);

  return r;
}

void
xr_rendering_apply_options (struct xr_rendering *xr, struct string_map *o)
{
  if (is_table_item (xr->item))
    apply_options (xr->xr, o);
}

struct xr_rendering *
xr_rendering_create (struct xr_driver *xr, const struct output_item *item,
                     cairo_t *cr)
{
  struct xr_rendering *r = NULL;

  if (is_text_item (item))
    r = xr_rendering_create_text (xr, text_item_get_text (to_text_item (item)),
                                  cr);
  else if (is_message_item (item))
    {
      const struct message_item *message_item = to_message_item (item);
      char *s = msg_to_string (message_item_get_msg (message_item));
      r = xr_rendering_create_text (xr, s, cr);
      free (s);
    }
  else if (is_table_item (item))
    {
      r = xzalloc (sizeof *r);
      r->item = output_item_ref (item);
      r->xr = xr;
      xr_set_cairo (xr, cr);
      r->p = render_pager_create (xr->params, to_table_item (item));
    }
  else if (is_chart_item (item))
    {
      r = xzalloc (sizeof *r);
      r->item = output_item_ref (item);
    }
  else if (is_group_open_item (item))
    r = xr_rendering_create_text (xr, to_group_open_item (item)->command_name,
                                  cr);

  return r;
}

void
xr_rendering_destroy (struct xr_rendering *r)
{
  if (r)
    {
      output_item_unref (r->item);
      render_pager_destroy (r->p);
      free (r);
    }
}

void
xr_rendering_measure (struct xr_rendering *r, int *wp, int *hp)
{
  int w, h;

  if (is_table_item (r->item))
    {
      w = render_pager_get_size (r->p, H) / XR_POINT;
      h = render_pager_get_size (r->p, V) / XR_POINT;
    }
  else
    {
      w = CHART_WIDTH;
      h = CHART_HEIGHT;
    }

  if (wp)
    *wp = w;
  if (hp)
    *hp = h;
}

static void xr_draw_chart (const struct chart_item *, cairo_t *,
                    double x, double y, double width, double height);

/* Draws onto CR */
void
xr_rendering_draw (struct xr_rendering *r, cairo_t *cr,
                   int x0, int y0, int x1, int y1)
{
  if (is_table_item (r->item))
    {
      struct xr_driver *xr = r->xr;

      xr_set_cairo (xr, cr);

      render_pager_draw_region (r->p, x0 * XR_POINT, y0 * XR_POINT,
                                (x1 - x0) * XR_POINT, (y1 - y0) * XR_POINT);
    }
  else
    xr_draw_chart (to_chart_item (r->item), cr,
                   0, 0, CHART_WIDTH, CHART_HEIGHT);
}

static void
xr_draw_chart (const struct chart_item *chart_item, cairo_t *cr,
               double x, double y, double width, double height)
{
  struct xrchart_geometry geom;

  cairo_save (cr);
  cairo_translate (cr, x, y + height);
  cairo_scale (cr, 1.0, -1.0);
  xrchart_geometry_init (cr, &geom, width, height);
  if (is_boxplot (chart_item))
    xrchart_draw_boxplot (chart_item, cr, &geom);
  else if (is_histogram_chart (chart_item))
    xrchart_draw_histogram (chart_item, cr, &geom);
  else if (is_np_plot_chart (chart_item))
    xrchart_draw_np_plot (chart_item, cr, &geom);
  else if (is_piechart (chart_item))
    xrchart_draw_piechart (chart_item, cr, &geom);
  else if (is_barchart (chart_item))
    xrchart_draw_barchart (chart_item, cr, &geom);
  else if (is_roc_chart (chart_item))
    xrchart_draw_roc (chart_item, cr, &geom);
  else if (is_scree (chart_item))
    xrchart_draw_scree (chart_item, cr, &geom);
  else if (is_spreadlevel_plot_chart (chart_item))
    xrchart_draw_spreadlevel (chart_item, cr, &geom);
  else if (is_scatterplot_chart (chart_item))
    xrchart_draw_scatterplot (chart_item, cr, &geom);
  else
    NOT_REACHED ();
  xrchart_geometry_free (cr, &geom);

  cairo_restore (cr);
}

char *
xr_draw_png_chart (const struct chart_item *item,
                   const char *file_name_template, int number,
		   const struct xr_color *fg,
		   const struct xr_color *bg
		   )
{
  const int width = 640;
  const int length = 480;

  cairo_surface_t *surface;
  cairo_status_t status;
  const char *number_pos;
  char *file_name;
  cairo_t *cr;

  number_pos = strchr (file_name_template, '#');
  if (number_pos != NULL)
    file_name = xasprintf ("%.*s%d%s", (int) (number_pos - file_name_template),
                           file_name_template, number, number_pos + 1);
  else
    file_name = xstrdup (file_name_template);

  surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, length);
  cr = cairo_create (surface);

  cairo_set_source_rgb (cr, bg->red, bg->green, bg->blue);
  cairo_paint (cr);

  cairo_set_source_rgb (cr, fg->red, fg->green, fg->blue);

  xr_draw_chart (item, cr, 0.0, 0.0, width, length);

  status = cairo_surface_write_to_png (surface, file_name);
  if (status != CAIRO_STATUS_SUCCESS)
    msg (ME, _("error writing output file `%s': %s"),
           file_name, cairo_status_to_string (status));

  cairo_destroy (cr);
  cairo_surface_destroy (surface);

  return file_name;
}

struct xr_table_state
  {
    struct xr_render_fsm fsm;
    struct render_pager *p;
  };

static bool
xr_table_render (struct xr_render_fsm *fsm, struct xr_driver *xr)
{
  struct xr_table_state *ts = UP_CAST (fsm, struct xr_table_state, fsm);

  while (render_pager_has_next (ts->p))
    {
      int used;

      used = render_pager_draw_next (ts->p, xr->length - xr->y);
      if (!used)
        {
          assert (xr->y > 0);
          return true;
        }
      else
        xr->y += used;
    }
  return false;
}

static void
xr_table_destroy (struct xr_render_fsm *fsm)
{
  struct xr_table_state *ts = UP_CAST (fsm, struct xr_table_state, fsm);

  render_pager_destroy (ts->p);
  free (ts);
}

static struct xr_render_fsm *
xr_render_table (struct xr_driver *xr, struct table_item *table_item)
{
  struct xr_table_state *ts;

  ts = xmalloc (sizeof *ts);
  ts->fsm.render = xr_table_render;
  ts->fsm.destroy = xr_table_destroy;

  if (xr->y > 0)
    xr->y += xr->char_height;

  ts->p = render_pager_create (xr->params, table_item);
  table_item_unref (table_item);

  return &ts->fsm;
}

struct xr_chart_state
  {
    struct xr_render_fsm fsm;
    struct chart_item *chart_item;
  };

static bool
xr_chart_render (struct xr_render_fsm *fsm, struct xr_driver *xr)
{
  struct xr_chart_state *cs = UP_CAST (fsm, struct xr_chart_state, fsm);

  const int chart_height = 0.8 * (xr->length < xr->width ? xr->length : xr->width);

  if (xr->y > xr->length - chart_height)
    return true;

  if (xr->cairo != NULL)
    {
      xr_draw_chart (cs->chart_item, xr->cairo,
		     0.0,
		     xr_to_pt (xr->y),
		     xr_to_pt (xr->width),
		     xr_to_pt (chart_height));
    }
  xr->y += chart_height;

  return false;
}

static void
xr_chart_destroy (struct xr_render_fsm *fsm)
{
  struct xr_chart_state *cs = UP_CAST (fsm, struct xr_chart_state, fsm);

  chart_item_unref (cs->chart_item);
  free (cs);
}

static struct xr_render_fsm *
xr_render_chart (const struct chart_item *chart_item)
{
  struct xr_chart_state *cs;

  cs = xmalloc (sizeof *cs);
  cs->fsm.render = xr_chart_render;
  cs->fsm.destroy = xr_chart_destroy;
  cs->chart_item = chart_item_ref (chart_item);

  return &cs->fsm;
}

static bool
xr_eject_render (struct xr_render_fsm *fsm UNUSED, struct xr_driver *xr)
{
  return xr->y > 0;
}

static void
xr_eject_destroy (struct xr_render_fsm *fsm UNUSED)
{
  /* Nothing to do. */
}

static struct xr_render_fsm *
xr_render_eject (void)
{
  static struct xr_render_fsm eject_renderer =
    {
      xr_eject_render,
      xr_eject_destroy
    };

  return &eject_renderer;
}

static struct xr_render_fsm *
xr_render_text (struct xr_driver *xr, const struct text_item *text_item)
{
  enum text_item_type type = text_item_get_type (text_item);
  const char *text = text_item_get_text (text_item);

  switch (type)
    {
    case TEXT_ITEM_PAGE_TITLE:
      string_map_replace (&xr->heading_vars, "PageTitle", text);
      break;

    case TEXT_ITEM_BLANK_LINE:
      if (xr->y > 0)
        xr->y += xr->char_height;
      break;

    case TEXT_ITEM_EJECT_PAGE:
      if (xr->y > 0)
        return xr_render_eject ();
      break;

    default:
      return xr_render_table (
        xr, text_item_to_table_item (text_item_ref (text_item)));
    }

  return NULL;
}

static struct xr_render_fsm *
xr_render_message (struct xr_driver *xr,
                   const struct message_item *message_item)
{
  char *s = msg_to_string (message_item_get_msg (message_item));
  struct text_item *item = text_item_create (TEXT_ITEM_PARAGRAPH, s);
  free (s);
  return xr_render_table (xr, text_item_to_table_item (item));
}

static struct xr_render_fsm *
xr_render_output_item (struct xr_driver *xr,
                       const struct output_item *output_item)
{
  if (is_table_item (output_item))
    return xr_render_table (xr, table_item_ref (to_table_item (output_item)));
  else if (is_chart_item (output_item))
    return xr_render_chart (to_chart_item (output_item));
  else if (is_text_item (output_item))
    return xr_render_text (xr, to_text_item (output_item));
  else if (is_message_item (output_item))
    return xr_render_message (xr, to_message_item (output_item));
  else
    return NULL;
}
