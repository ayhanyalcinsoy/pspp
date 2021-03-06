/* PSPP - a program for statistical analysis.
   Copyright (C) 2018 Free Software Foundation, Inc.

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

#include <stdlib.h>

#include "output/pivot-table.h"

#include "data/settings.h"
#include "libpspp/assertion.h"
#include "libpspp/pool.h"
#include "output/tab.h"
#include "output/table.h"
#include "output/table-item.h"
#include "output/text-item.h"
#include "output/table-provider.h"

#include "gl/minmax.h"
#include "gl/xalloc.h"

#define H TABLE_HORZ
#define V TABLE_VERT

static const struct pivot_category *
find_category (const struct pivot_dimension *d, int dim_index,
               const size_t *indexes, int row_ofs)
{
  size_t index = indexes[dim_index];
  assert (index < d->n_leaves);
  for (const struct pivot_category *c = d->presentation_leaves[index];
       c; c = c->parent)
    {
      if (!row_ofs)
        return c;

      row_ofs -= 1 + c->extra_depth;
      if (row_ofs < 0)
        return NULL;
    }
  return NULL;
}

static struct area_style *
area_style_override (struct pool *pool,
                     const struct area_style *in,
                     const struct cell_style *cell_,
                     const struct font_style *font_)
{
  const struct cell_style *cell = cell_ ? cell_ : &in->cell_style;
  const struct font_style *font = font_ ? font_ : &in->font_style;

  struct area_style *out = (pool
                            ? pool_alloc (pool, sizeof *out)
                            : xmalloc (sizeof *out));
  *out = (struct area_style) {
    .cell_style.halign = cell->halign,
    .cell_style.valign = cell->valign,
    .cell_style.decimal_offset = cell->decimal_offset,
    .cell_style.margin[H][0] = cell->margin[H][0],
    .cell_style.margin[H][1] = cell->margin[H][1],
    .cell_style.margin[V][0] = cell->margin[V][0],
    .cell_style.margin[V][1] = cell->margin[V][1],
    .font_style.fg[0] = font->fg[0],
    .font_style.fg[1] = font->fg[1],
    .font_style.bg[0] = font->bg[0],
    .font_style.bg[1] = font->bg[1],
    .font_style.typeface = (font->typeface
                            ? pool_strdup (pool, font->typeface)
                            : NULL),
    .font_style.size = font->size,
    .font_style.bold = font->bold,
    .font_style.italic = font->italic,
    .font_style.underline = font->underline,
    .font_style.markup = font->markup,
  };
  return out;
}

static void
fill_cell (struct tab_table *t, int x1, int y1, int x2, int y2,
           const struct area_style *style, int style_idx,
           const struct pivot_value *value, struct footnote **footnotes,
           enum settings_value_show show_values,
           enum settings_value_show show_variables,
           bool rotate_label)
{

  struct string s = DS_EMPTY_INITIALIZER;
  int opts = style_idx << TAB_STYLE_SHIFT;
  if (value)
    {
      bool numeric = pivot_value_format_body (value, show_values,
                                              show_variables, &s);
      if (numeric)
        opts |= TAB_NUMERIC;
      if (value->font_style && value->font_style->markup)
        opts |= TAB_MARKUP;
      if (rotate_label)
        opts |= TAB_ROTATE;
    }
  tab_joint_text (t, x1, y1, x2, y2, opts, ds_cstr (&s));
  ds_destroy (&s);

  if (value)
    {
      if (value->cell_style || value->font_style)
        tab_add_style (t, x1, y1,
                       area_style_override (t->container, style,
                                            value->cell_style,
                                            value->font_style));

      for (size_t i = 0; i < value->n_footnotes; i++)
        tab_add_footnote (t, x1, y1, footnotes[value->footnotes[i]->idx]);
    }
}

static struct table_item_text *
pivot_value_to_table_item_text (const struct pivot_value *value,
                                const struct area_style *area,
                                struct footnote **footnotes,
                                enum settings_value_show show_values,
                                enum settings_value_show show_variables)
{
  if (!value)
    return NULL;

  struct string s = DS_EMPTY_INITIALIZER;
  pivot_value_format_body (value, show_values, show_variables, &s);

  struct table_item_text *text = xmalloc (sizeof *text);
  *text = (struct table_item_text) {
    .content = ds_steal_cstr (&s),
    .footnotes = xnmalloc (value->n_footnotes, sizeof *text->footnotes),
    .n_footnotes = value->n_footnotes,
    .style = area_style_override (
      NULL, area, value->cell_style, value->font_style),
  };

  for (size_t i = 0; i < value->n_footnotes; i++)
    text->footnotes[i] = footnotes[value->footnotes[i]->idx];

  return text;
}

static int
get_table_rule (const struct table_border_style *styles,
                enum pivot_border style_idx)
{
  return styles[style_idx].stroke | (style_idx << TAB_RULE_STYLE_SHIFT);
}

static void
draw_line (struct tab_table *t, const struct table_border_style *styles,
           enum pivot_border style_idx,
           enum table_axis axis, int a, int b0, int b1)
{
  int rule = get_table_rule (styles, style_idx);
  if (axis == H)
    tab_hline (t, rule, b0, b1, a);
  else
    tab_vline (t, rule, a, b0, b1);
}

static void
compose_headings (struct tab_table *t,
                  const struct pivot_axis *a_axis, enum table_axis a,
                  const struct pivot_axis *b_axis,
                  const struct table_border_style *borders,
                  enum pivot_border dim_col_horz,
                  enum pivot_border dim_col_vert,
                  enum pivot_border cat_col_horz,
                  enum pivot_border cat_col_vert,
                  const size_t *column_enumeration, size_t n_columns,
                  const struct area_style *label_style, int label_style_idx,
                  const struct area_style *corner_style,
                  struct footnote **footnotes,
                  enum settings_value_show show_values,
                  enum settings_value_show show_variables,
                  bool rotate_inner_labels, bool rotate_outer_labels)
{
  enum table_axis b = !a;
  int b_size = a_axis->label_depth;
  int a_ofs = b_axis->label_depth;

  if (!a_axis->n_dimensions || !n_columns || !b_size)
    return;

  int bottom_row = b_size - 1;
  const int stride = MAX (1, a_axis->n_dimensions);
  for (int dim_index = 0; dim_index < a_axis->n_dimensions; dim_index++)
    {
      const struct pivot_dimension *d = a_axis->dimensions[dim_index];
      if (d->hide_all_labels)
        continue;

      for (int row_ofs = 0; row_ofs < d->label_depth; row_ofs++)
        {
          for (size_t x1 = 0; x1 < n_columns; )
            {
              const struct pivot_category *c = find_category (
                d, dim_index, column_enumeration + x1 * stride, row_ofs);
              if (!c)
                {
                  x1++;
                  continue;
                }

              size_t x2;
              for (x2 = x1 + 1; x2 < n_columns; x2++)
                {
                  const struct pivot_category *c2 = find_category (
                    d, dim_index, column_enumeration + x2 * stride, row_ofs);
                  if (c != c2)
                    break;
                }

              int y1 = bottom_row - row_ofs - c->extra_depth;
              int y2 = bottom_row - row_ofs + 1;
              bool is_outer_row = y1 == 0;
              bool is_inner_row = y2 == b_size;
              if (pivot_category_is_leaf (c) || c->show_label)
                {
                  int bb[TABLE_N_AXES][2];
                  bb[a][0] = x1 + a_ofs;
                  bb[a][1] = x2 + a_ofs - 1;
                  bb[b][0] = y1;
                  bb[b][1] = y2 - 1;
                  bool rotate = ((rotate_inner_labels && is_inner_row)
                                 || (rotate_outer_labels && is_outer_row));
                  fill_cell (t, bb[H][0], bb[V][0], bb[H][1], bb[V][1],
                             label_style, label_style_idx, c->name, footnotes,
                             show_values, show_variables, rotate);

                  if (pivot_category_is_leaf (c) && x2 + 1 <= n_columns)
                    {
                      enum pivot_border style
                        = (y1 == 0 && a_axis->label_depth > d->label_depth
                           ? dim_col_vert
                           : cat_col_vert);
                      draw_line (t, borders, style, b, x2 + a_ofs, y1,
                                 t->table.n[b] - 1);
                    }
                  if (pivot_category_is_leaf (c) && x1 > 0)
                    {
                      enum pivot_border style
                        = (y1 == 0 && a_axis->label_depth > d->label_depth
                           ? dim_col_vert
                           : cat_col_vert);
                      draw_line (t, borders, style, b, x1 + a_ofs, y1,
                                 t->table.n[b] - 1);
                    }
                }
              if (c->parent && c->parent->show_label)
                draw_line (t, borders, cat_col_horz, a, y1,
                           x1 + a_ofs, x2 + a_ofs - 1);

              x1 = x2;
            }
        }

      if (d->root->show_label_in_corner && a_ofs > 0)
        {
          int bb[TABLE_N_AXES][2];
          bb[a][0] = a_ofs - 1;
          bb[a][1] = a_ofs - 1;
          bb[b][0] = bottom_row - d->label_depth + 1;
          bb[b][1] = bottom_row;
          fill_cell (t, bb[H][0], bb[V][0], bb[H][1], bb[V][1],
                     corner_style, PIVOT_AREA_CORNER, d->root->name, footnotes,
                     show_values, show_variables, false);
        }

      if (dim_index > 1)
        draw_line (t, borders, dim_col_horz, a, bottom_row + 1, a_ofs,
                   t->table.n[a] - 1);

      bottom_row -= d->label_depth;
    }
}

static void
pivot_table_submit_layer (const struct pivot_table *pt,
                          const size_t *layer_indexes)
{
  const size_t *pindexes[PIVOT_N_AXES]
    = { [PIVOT_AXIS_LAYER] = layer_indexes };

  struct string layer_label = DS_EMPTY_INITIALIZER;
  const struct pivot_axis *layer_axis = &pt->axes[PIVOT_AXIS_LAYER];
  for (size_t i = 0; i < layer_axis->n_dimensions; i++)
    {
      const struct pivot_dimension *d = layer_axis->dimensions[i];
      if (d->n_leaves)
        {
          if (!ds_is_empty (&layer_label))
            ds_put_byte (&layer_label, '\n');
          pivot_value_format (d->root->name, pt->show_values,
                              pt->show_variables, &layer_label);
          ds_put_cstr (&layer_label, ": ");
          pivot_value_format (d->data_leaves[layer_indexes[i]]->name,
                              pt->show_values, pt->show_variables,
                              &layer_label);
        }
    }

  size_t body[TABLE_N_AXES];
  size_t *column_enumeration = pivot_table_enumerate_axis (
    pt, PIVOT_AXIS_COLUMN, layer_indexes, pt->omit_empty, &body[H]);
  size_t *row_enumeration = pivot_table_enumerate_axis (
    pt, PIVOT_AXIS_ROW, layer_indexes, pt->omit_empty, &body[V]);

  int stub[TABLE_N_AXES] = {
    [H] = pt->axes[PIVOT_AXIS_ROW].label_depth,
    [V] = pt->axes[PIVOT_AXIS_COLUMN].label_depth,
  };
  struct tab_table *table = tab_create (body[H] + stub[H],
                                        body[V] + stub[V]);
  tab_headers (table, stub[H], 0, stub[V], 0);

  for (size_t i = 0; i < PIVOT_N_AREAS; i++)
    table->styles[i] = area_style_override (table->container, &pt->areas[i],
                                            NULL, NULL);

  for (size_t i = 0; i < PIVOT_N_BORDERS; i++)
    {
      const struct table_border_style *in = &pt->borders[i];
      table->rule_colors[i] = pool_alloc (table->container,
                                          sizeof *table->rule_colors[i]);
      struct cell_color *out = table->rule_colors[i];
      out->alpha = in->color.alpha;
      out->r = in->color.r;
      out->g = in->color.g;
      out->b = in->color.b;
    }

  struct footnote **footnotes = xcalloc (pt->n_footnotes, sizeof *footnotes);
  for (size_t i = 0; i < pt->n_footnotes; i++)
    {
      char *content = pivot_value_to_string (
        pt->footnotes[i]->content, pt->show_values, pt->show_variables);
      char *marker = pivot_value_to_string (
        pt->footnotes[i]->marker, pt->show_values, pt->show_variables);
      footnotes[i] = tab_create_footnote (
        table, i, content, marker,
        area_style_override (table->container, &pt->areas[PIVOT_AREA_FOOTER],
                             pt->footnotes[i]->content->cell_style,
                             pt->footnotes[i]->content->font_style));
      free (marker);
      free (content);
    }

  compose_headings (table,
                    &pt->axes[PIVOT_AXIS_COLUMN], H, &pt->axes[PIVOT_AXIS_ROW],
                    pt->borders,
                    PIVOT_BORDER_DIM_COL_HORZ,
                    PIVOT_BORDER_DIM_COL_VERT,
                    PIVOT_BORDER_CAT_COL_HORZ,
                    PIVOT_BORDER_CAT_COL_VERT,
                    column_enumeration, body[H],
                    &pt->areas[PIVOT_AREA_COLUMN_LABELS],
                    PIVOT_AREA_COLUMN_LABELS,
                    &pt->areas[PIVOT_AREA_CORNER], footnotes,
                    pt->show_values, pt->show_variables,
                    pt->rotate_inner_column_labels, false);

  compose_headings (table,
                    &pt->axes[PIVOT_AXIS_ROW], V, &pt->axes[PIVOT_AXIS_COLUMN],
                    pt->borders,
                    PIVOT_BORDER_DIM_ROW_VERT,
                    PIVOT_BORDER_DIM_ROW_HORZ,
                    PIVOT_BORDER_CAT_ROW_VERT,
                    PIVOT_BORDER_CAT_ROW_HORZ,
                    row_enumeration, body[V],
                    &pt->areas[PIVOT_AREA_ROW_LABELS],
                    PIVOT_AREA_ROW_LABELS,
                    &pt->areas[PIVOT_AREA_CORNER], footnotes,
                    pt->show_values, pt->show_variables,
                    false, pt->rotate_outer_row_labels);

  size_t *dindexes = xcalloc (pt->n_dimensions, sizeof *dindexes);
  size_t y = 0;
  PIVOT_ENUMERATION_FOR_EACH (pindexes[PIVOT_AXIS_ROW], row_enumeration,
                              &pt->axes[PIVOT_AXIS_ROW])
    {
      size_t x = 0;
      PIVOT_ENUMERATION_FOR_EACH (pindexes[PIVOT_AXIS_COLUMN],
                                  column_enumeration,
                                  &pt->axes[PIVOT_AXIS_COLUMN])
        {
          pivot_table_convert_indexes_ptod (pt, pindexes, dindexes);
          const struct pivot_value *value = pivot_table_get (pt, dindexes);
          fill_cell (table,
                     x + stub[H], y + stub[V],
                     x + stub[H], y + stub[V],
                     &pt->areas[PIVOT_AREA_DATA], PIVOT_AREA_DATA,
                     value, footnotes,
                     pt->show_values, pt->show_variables, false);

          x++;
        }

      y++;
    }
  free (dindexes);

  if (pt->corner_text && stub[H] && stub[V])
    fill_cell (table, 0, 0, stub[H] - 1, stub[V] - 1,
               &pt->areas[PIVOT_AREA_CORNER], PIVOT_AREA_CORNER,
               pt->corner_text, footnotes,
               pt->show_values, pt->show_variables, false);

  if (tab_nc (table) && tab_nr (table))
    {
      tab_hline (
        table, get_table_rule (pt->borders, PIVOT_BORDER_INNER_TOP),
        0, tab_nc (table) - 1, 0);
      tab_hline (
        table, get_table_rule (pt->borders, PIVOT_BORDER_INNER_BOTTOM),
        0, tab_nc (table) - 1, tab_nr (table));
      tab_vline (
        table, get_table_rule (pt->borders, PIVOT_BORDER_INNER_LEFT),
        0, 0, tab_nr (table) - 1);
      tab_vline (
        table, get_table_rule (pt->borders, PIVOT_BORDER_INNER_RIGHT),
        tab_nc (table), 0, tab_nr (table) - 1);

      if (stub[V])
        tab_hline (
          table, get_table_rule (pt->borders, PIVOT_BORDER_DATA_TOP),
          0, tab_nc (table) - 1, stub[V]);
      if (stub[H])
        tab_vline (
          table, get_table_rule (pt->borders, PIVOT_BORDER_DATA_LEFT),
          stub[H], 0, tab_nr (table) - 1);

    }
  free (column_enumeration);
  free (row_enumeration);

  struct table_item *ti = table_item_create (&table->table, NULL, NULL);

  if (pt->title)
    {
      struct table_item_text *title = pivot_value_to_table_item_text (
        pt->title, &pt->areas[PIVOT_AREA_TITLE], footnotes,
        pt->show_values, pt->show_variables);
      table_item_set_title (ti, title);
      table_item_text_destroy (title);
    }

  if (!ds_is_empty (&layer_label))
    {
      struct table_item_text *layers = table_item_text_create (
        ds_cstr (&layer_label));
      layers->style = area_style_override (NULL, &pt->areas[PIVOT_AREA_LAYERS],
                                           NULL, NULL);
      table_item_set_layers (ti, layers);
      table_item_text_destroy (layers);

      ds_destroy (&layer_label);
    }

  if (pt->caption)
    {
      struct table_item_text *caption = pivot_value_to_table_item_text (
        pt->caption, &pt->areas[PIVOT_AREA_CAPTION], footnotes,
        pt->show_values, pt->show_variables);
      table_item_set_caption (ti, caption);
      table_item_text_destroy (caption);
    }

  free (footnotes);
  table_item_submit (ti);
}

void
pivot_table_submit (struct pivot_table *pt)
{
  pivot_table_assign_label_depth (CONST_CAST (struct pivot_table *, pt));

  int old_decimal = settings_get_decimal_char (FMT_COMMA);
  if (pt->decimal == '.' || pt->decimal == ',')
    settings_set_decimal_char (pt->decimal);

  if (pt->print_all_layers)
    {
      size_t *layer_indexes;

      PIVOT_AXIS_FOR_EACH (layer_indexes, &pt->axes[PIVOT_AXIS_LAYER])
        {
          if (pt->paginate_layers)
            text_item_submit (text_item_create (TEXT_ITEM_EJECT_PAGE, ""));
          pivot_table_submit_layer (pt, layer_indexes);
        }
    }
  else
    pivot_table_submit_layer (pt, pt->current_layer);

  settings_set_decimal_char (old_decimal);

  pivot_table_destroy (pt);
}
