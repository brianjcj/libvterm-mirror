#include "vterm.h"
#include "vterm_internal.h"

#include <stdio.h>
#include <string.h>

#include "rect.h"
#include "utf8.h"

#if defined (_DEBUG) || !defined (NDEBUG)
#include <log.h>
#else
#define log_debug
#endif


#define UNICODE_SPACE 0x20
#define UNICODE_LINEFEED 0x0a


/* State of the pen at some moment in time, also used in a cell */
typedef struct
{
  /* After the bitfield */
  VTermColor   fg, bg;

  unsigned int bold      : 1;
  unsigned int underline : 2;
  unsigned int italic    : 1;
  unsigned int blink     : 1;
  unsigned int reverse   : 1;
  unsigned int conceal   : 1;
  unsigned int strike    : 1;
  unsigned int font      : 4; /* 0 to 9 */
  unsigned int small     : 1;
  unsigned int baseline  : 2;

  /* Extra state storage that isn't strictly pen-related */
  unsigned int protected_cell : 1;
  unsigned int dwl            : 1; /* on a DECDWL or DECDHL line */
  unsigned int dhl            : 2; /* on a DECDHL line (1=top 2=bottom) */
} ScreenPen;

/* Internal representation of a screen cell */
typedef struct
{
  uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
  ScreenPen pen;
} ScreenCell;

struct VTermScreen
{
  VTerm *vt;
  VTermState *state;

  const VTermScreenCallbacks *callbacks;
  void *cbdata;
  bool callbacks_has_pushline4;

  VTermDamageSize damage_merge;
  /* start_row == -1 => no damage */
  VTermRect damaged;
  VTermRect pending_scrollrect;
  int pending_scroll_downward, pending_scroll_rightward;

  int rows;
  int cols;

  unsigned int global_reverse : 1;
  unsigned int reflow : 1;
  unsigned int with_conpty : 1;

  /* Primary and Altscreen. buffers[1] is lazily allocated as needed */
  ScreenCell *buffers[2];

  /* buffer will == buffers[0] or buffers[1], depending on altscreen */
  ScreenCell *buffer;

  /* buffer for a single screen row used in scrollback storage callbacks */
  VTermScreenCell *sb_buffer;
  int sb_buffer_cols;

  ScreenPen pen;
};

static inline void clearcell(const VTermScreen *screen, ScreenCell *cell)
{
  cell->chars[0] = 0;
  /* cell->pen = screen->pen; // TODO! */
}

static inline ScreenCell *getcell(const VTermScreen *screen, int row, int col)
{
  if(row < 0 || row >= screen->rows)
    return NULL;
  if(col < 0 || col >= screen->cols)
    return NULL;
  return screen->buffer + (screen->cols * row) + col;
}

static ScreenCell *alloc_buffer(VTermScreen *screen, int rows, int cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * rows * cols);

  for(int row = 0; row < rows; row++) {
    for(int col = 0; col < cols; col++) {
      clearcell(screen, &new_buffer[row * cols + col]);
    }
  }

  return new_buffer;
}

static void damagerect(VTermScreen *screen, VTermRect rect)
{
  VTermRect emit;

  switch(screen->damage_merge) {
  case VTERM_DAMAGE_CELL:
    /* Always emit damage event */
    emit = rect;
    break;

  case VTERM_DAMAGE_ROW:
    /* Emit damage longer than one row. Try to merge with existing damage in
     * the same row */
    if(rect.end_row > rect.start_row + 1) {
      // Bigger than 1 line - flush existing, emit this
      vterm_screen_flush_damage(screen);
      emit = rect;
    }
    else if(screen->damaged.start_row == -1) {
      // None stored yet
      screen->damaged = rect;
      return;
    }
    else if(rect.start_row == screen->damaged.start_row) {
      // Merge with the stored line
      if(screen->damaged.start_col > rect.start_col)
        screen->damaged.start_col = rect.start_col;
      if(screen->damaged.end_col < rect.end_col)
        screen->damaged.end_col = rect.end_col;
      return;
    }
    else {
      // Emit the currently stored line, store a new one
      emit = screen->damaged;
      screen->damaged = rect;
    }
    break;

  case VTERM_DAMAGE_SCREEN:
  case VTERM_DAMAGE_SCROLL:
    /* Never emit damage event */
    if(screen->damaged.start_row == -1)
      screen->damaged = rect;
    else {
      rect_expand(&screen->damaged, &rect);
    }
    return;

  default:
    DEBUG_LOG("TODO: Maybe merge damage for level %d\n", screen->damage_merge);
    return;
  }

  if(screen->callbacks && screen->callbacks->damage)
    (*screen->callbacks->damage)(emit, screen->cbdata);
}

static void damagescreen(VTermScreen *screen)
{
  VTermRect rect = {
    .start_row = 0,
    .end_row   = screen->rows,
    .start_col = 0,
    .end_col   = screen->cols,
  };

  damagerect(screen, rect);
}

static int putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, pos.col);

  if(!cell)
    return 0;

  int i;
  for(i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++) {
    cell->chars[i] = info->chars[i];
    cell->pen = screen->pen;
  }
  if(i < VTERM_MAX_CHARS_PER_CELL)
    cell->chars[i] = 0;

  for(int col = 1; col < info->width; col++)
    getcell(screen, pos.row, pos.col + col)->chars[0] = (uint32_t)-1;

  VTermRect rect = {
    .start_row = pos.row,
    .end_row   = pos.row+1,
    .start_col = pos.col,
    .end_col   = pos.col+info->width,
  };

  cell->pen.protected_cell = info->protected_cell;
  cell->pen.dwl            = info->dwl;
  cell->pen.dhl            = info->dhl;

  damagerect(screen, rect);

  return 1;
}

static void sb_pushline_from_row_with_cols(VTermScreen *screen, int row, bool continuation, int cols)
{
  VTermPos pos = { .row = row };
  for(pos.col = 0; pos.col < cols; pos.col++)
    vterm_screen_get_cell(screen, pos, screen->sb_buffer + pos.col);

  if(screen->callbacks_has_pushline4 && screen->callbacks->sb_pushline4)
    (screen->callbacks->sb_pushline4)(cols, screen->sb_buffer, continuation, screen->cbdata);
  else
    (screen->callbacks->sb_pushline)(cols, screen->sb_buffer, screen->cbdata);
}

static void sb_pushline_from_row(VTermScreen *screen, int row, bool continuation)
{
  VTermPos pos = { .row = row };
  for(pos.col = 0; pos.col < screen->cols; pos.col++)
    vterm_screen_get_cell(screen, pos, screen->sb_buffer + pos.col);

  if(screen->callbacks_has_pushline4 && screen->callbacks->sb_pushline4)
    (screen->callbacks->sb_pushline4)(screen->cols, screen->sb_buffer, continuation, screen->cbdata);
  else
    (screen->callbacks->sb_pushline)(screen->cols, screen->sb_buffer, screen->cbdata);
}

static int premove(VTermRect rect, void *user)
{
  VTermScreen *screen = user;

  if(((screen->callbacks && screen->callbacks->sb_pushline) ||
        (screen->callbacks_has_pushline4 && screen->callbacks && screen->callbacks->sb_pushline4)) &&
     rect.start_row == 0 && rect.start_col == 0 &&        // starts top-left corner
     rect.end_col == screen->cols &&                      // full width
     screen->buffer == screen->buffers[BUFIDX_PRIMARY]) { // not altscreen
    for(int row = 0; row < rect.end_row; row++) {
      const VTermLineInfo *lineinfo = vterm_state_get_lineinfo(screen->state, row);
      sb_pushline_from_row(screen, row, lineinfo->continuation);
    }
  }

  return 1;
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  int cols = src.end_col - src.start_col;
  int downward = src.start_row - dest.start_row;

  int init_row, test_row, inc_row;
  if(downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row  = -1;
  }
  else {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row  = +1;
  }

  for(int row = init_row; row != test_row; row += inc_row)
    memmove(getcell(screen, row, dest.start_col),
            getcell(screen, row + downward, src.start_col),
            cols * sizeof(ScreenCell));

  return 1;
}

static int moverect_user(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->moverect) {
    if(screen->damage_merge != VTERM_DAMAGE_SCROLL)
      // Avoid an infinite loop
      vterm_screen_flush_damage(screen);

    if((*screen->callbacks->moverect)(dest, src, screen->cbdata))
      return 1;
  }

  damagerect(screen, dest);

  return 1;
}

static int erase_internal(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  for(int row = rect.start_row; row < screen->state->rows && row < rect.end_row; row++) {
    const VTermLineInfo *info = vterm_state_get_lineinfo(screen->state, row);

    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(selective && cell->pen.protected_cell)
        continue;

      cell->chars[0] = 0;
      cell->pen = (ScreenPen){
        /* Only copy .fg and .bg; leave things like rv in reset state */
        .fg = screen->pen.fg,
        .bg = screen->pen.bg,
      };
      cell->pen.dwl = info->doublewidth;
      cell->pen.dhl = info->doubleheight;
    }
  }

  return 1;
}

static int erase_user(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  damagerect(screen, rect);

  return 1;
}

static int erase(VTermRect rect, int selective, void *user)
{
  erase_internal(rect, selective, user);
  return erase_user(rect, 0, user);
}

static int scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if(screen->damage_merge != VTERM_DAMAGE_SCROLL) {
    vterm_scroll_rect(rect, downward, rightward,
        moverect_internal, erase_internal, screen);

    vterm_screen_flush_damage(screen);

    vterm_scroll_rect(rect, downward, rightward,
        moverect_user, erase_user, screen);

    return 1;
  }

  if(screen->damaged.start_row != -1 &&
     !rect_intersects(&rect, &screen->damaged)) {
    vterm_screen_flush_damage(screen);
  }

  if(screen->pending_scrollrect.start_row == -1) {
    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }
  else if(rect_equal(&screen->pending_scrollrect, &rect) &&
     ((screen->pending_scroll_downward  == 0 && downward  == 0) ||
      (screen->pending_scroll_rightward == 0 && rightward == 0))) {
    screen->pending_scroll_downward  += downward;
    screen->pending_scroll_rightward += rightward;
  }
  else {
    vterm_screen_flush_damage(screen);

    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }

  vterm_scroll_rect(rect, downward, rightward,
      moverect_internal, erase_internal, screen);

  if(screen->damaged.start_row == -1)
    return 1;

  if(rect_contains(&rect, &screen->damaged)) {
    /* Scroll region entirely contains the damage; just move it */
    vterm_rect_move(&screen->damaged, -downward, -rightward);
    rect_clip(&screen->damaged, &rect);
  }
  /* There are a number of possible cases here, but lets restrict this to only
   * the common case where we might actually gain some performance by
   * optimising it. Namely, a vertical scroll that neatly cuts the damage
   * region in half.
   */
  else if(rect.start_col <= screen->damaged.start_col &&
          rect.end_col   >= screen->damaged.end_col &&
          rightward == 0) {
    if(screen->damaged.start_row >= rect.start_row &&
       screen->damaged.start_row  < rect.end_row) {
      screen->damaged.start_row -= downward;
      if(screen->damaged.start_row < rect.start_row)
        screen->damaged.start_row = rect.start_row;
      if(screen->damaged.start_row > rect.end_row)
        screen->damaged.start_row = rect.end_row;
    }
    if(screen->damaged.end_row >= rect.start_row &&
       screen->damaged.end_row  < rect.end_row) {
      screen->damaged.end_row -= downward;
      if(screen->damaged.end_row < rect.start_row)
        screen->damaged.end_row = rect.start_row;
      if(screen->damaged.end_row > rect.end_row)
        screen->damaged.end_row = rect.end_row;
    }
  }
  else {
    DEBUG_LOG("TODO: Just flush and redo damaged=" STRFrect " rect=" STRFrect "\n",
        ARGSrect(screen->damaged), ARGSrect(rect));
  }

  return 1;
}

static int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->movecursor)
    return (*screen->callbacks->movecursor)(pos, oldpos, visible, screen->cbdata);

  return 0;
}

static int setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(attr) {
  case VTERM_ATTR_BOLD:
    screen->pen.bold = val->boolean;
    return 1;
  case VTERM_ATTR_UNDERLINE:
    screen->pen.underline = val->number;
    return 1;
  case VTERM_ATTR_ITALIC:
    screen->pen.italic = val->boolean;
    return 1;
  case VTERM_ATTR_BLINK:
    screen->pen.blink = val->boolean;
    return 1;
  case VTERM_ATTR_REVERSE:
    screen->pen.reverse = val->boolean;
    return 1;
  case VTERM_ATTR_CONCEAL:
    screen->pen.conceal = val->boolean;
    return 1;
  case VTERM_ATTR_STRIKE:
    screen->pen.strike = val->boolean;
    return 1;
  case VTERM_ATTR_FONT:
    screen->pen.font = val->number;
    return 1;
  case VTERM_ATTR_FOREGROUND:
    screen->pen.fg = val->color;
    return 1;
  case VTERM_ATTR_BACKGROUND:
    screen->pen.bg = val->color;
    return 1;
  case VTERM_ATTR_SMALL:
    screen->pen.small = val->boolean;
    return 1;
  case VTERM_ATTR_BASELINE:
    screen->pen.baseline = val->number;
    return 1;

  case VTERM_N_ATTRS:
    return 0;
  }

  return 0;
}

static int settermprop(VTermProp prop, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(prop) {
  case VTERM_PROP_ALTSCREEN:
    if(val->boolean && !screen->buffers[BUFIDX_ALTSCREEN])
      return 0;

    screen->buffer = val->boolean ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];
    /* only send a damage event on disable; because during enable there's an
     * erase that sends a damage anyway
     */
    if(!val->boolean)
      damagescreen(screen);
    break;
  case VTERM_PROP_REVERSE:
    screen->global_reverse = val->boolean;
    damagescreen(screen);
    break;
  default:
    ; /* ignore */
  }

  if(screen->callbacks && screen->callbacks->settermprop)
    return (*screen->callbacks->settermprop)(prop, val, screen->cbdata);

  return 1;
}

static int bell(void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->bell)
    return (*screen->callbacks->bell)(screen->cbdata);

  return 0;
}

/* How many cells are non-blank
 * Returns the position of the first blank cell in the trailing blank end */
static int line_popcount(ScreenCell *buffer, int row, int cols)
{
  int col = cols - 1;
  while(col >= 0 && buffer[row * cols + col].chars[0] == 0)
    col--;
  return col + 1;
}

/* How many cells are non-blank
 * Returns the position of the first blank cell in the trailing blank end */
static int sb_line_popcount(const VTermScreenCell *buffer, int cols)
{
  int col = cols - 1;
  while(col >= 0 && buffer[col].chars[0] == 0)
    col--;
  if (buffer[col].width == 2)
    col++;
  return col + 1;
}

static void alloc_sb_buffer(VTermScreen *screen, int cols) {
  if(screen->sb_buffer)
    vterm_allocator_free(screen->vt, screen->sb_buffer);

  screen->sb_buffer_cols = cols;
  screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * cols);
}

static void ensure_sb_buffer_cols(VTermScreen *screen, int cols) {
  if (screen->sb_buffer_cols < cols) {
    alloc_sb_buffer(screen, cols);
  }
}

static void copy_sb_cell_to_screen_cell(VTermScreen *screen, ScreenCell *dst, const VTermScreenCell *src) {
  for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
    dst->chars[i] = src->chars[i];
    if (!src->chars[i])
      break;
  }

  dst->pen.bold = src->attrs.bold;
  dst->pen.underline = src->attrs.underline;
  dst->pen.italic = src->attrs.italic;
  dst->pen.blink = src->attrs.blink;
  dst->pen.reverse = src->attrs.reverse ^ screen->global_reverse;
  dst->pen.conceal = src->attrs.conceal;
  dst->pen.strike = src->attrs.strike;
  dst->pen.font = src->attrs.font;
  dst->pen.small = src->attrs.small;
  dst->pen.baseline = src->attrs.baseline;

  dst->pen.fg = src->fg;
  dst->pen.bg = src->bg;
}

static void reflow_line(VTermScreen *screen,
                        ScreenCell *old_buffer, int old_row_start,
                        int old_row_end, int old_cols, int new_cols,
                        VTermPos *out_rect,
                        ScreenCell *out_buffer, int skip_rows,
                        VTermPos *old_cursor, VTermPos *new_cursor, int new_row_start) {
  /* log_debug("reflow line entry: for old rows: %d,%d", old_row_start, old_row_end); */
  int new_row = 0;
  int old_row = old_row_start;

  // get a old line
  int old_line_cells = line_popcount(old_buffer, old_row, old_cols);
  int old_line_taken = 0;
  int new_line_filled = 0;

  while (1) {
    int new_line_need_cells = new_cols - new_line_filled;
    int old_line_have = old_line_cells - old_line_taken;

    if (old_line_have <= new_line_need_cells) {

      if (out_buffer != NULL && new_row >= skip_rows) {
        /* log_debug("copy1: %d:%d, count: %d", new_row, new_line_filled, old_line_have); */
        memcpy(&out_buffer[(new_row - skip_rows) * new_cols + new_line_filled],
               &old_buffer[old_row * old_cols + old_line_taken],
               old_line_have * sizeof(ScreenCell));

        /* update cursor */
        if (old_cursor->row == old_row && old_cursor->col >= old_line_taken) {
          new_cursor->row = new_row_start + new_row;
          new_cursor->col = new_line_filled + (old_cursor->col - old_line_taken);
        }
      }

      // current new line still has room
      // next old line

      new_line_filled += old_line_have;

      old_row++;
      old_line_taken = 0;

      if (old_row > old_row_end)
        break;

      if (old_line_have == new_line_need_cells) {
        // happy! next line together!
        new_row++;
        new_line_filled = 0;
      }

      old_line_cells = line_popcount(old_buffer, old_row, old_cols);

    } else {
      // > new_line_need_cells
      // fill the current new buffer line.

      if (out_buffer != NULL && new_row >= skip_rows) {
        /* log_debug("copy3: %d:%d, count: %d", new_row, new_line_filled, new_line_need_cells); */
        memcpy(&out_buffer[(new_row - skip_rows) * new_cols + new_line_filled],
               &old_buffer[old_row * old_cols + old_line_taken],
               new_line_need_cells * sizeof(ScreenCell));

        /* update cursor */
        if (old_cursor->row == old_row && old_cursor->col >= old_line_taken &&
            old_cursor->col < old_line_taken + new_line_need_cells) {
          new_cursor->row = new_row_start + new_row;
          new_cursor->col = new_line_filled + (old_cursor->col - old_line_taken);
        }
      }

      old_line_taken += new_line_need_cells;

      ScreenCell *cell = &old_buffer[old_row * old_cols + old_line_taken];
      if (cell->chars[0] == (uint32_t)-1) {
        // width > 1. don't break it
        old_line_taken--;
        if (out_buffer != NULL && new_row >= skip_rows) {
          /* log_debug("clear cell: %d:%d", new_row, new_cols - 1); */
          clearcell(screen, &out_buffer[(new_row - skip_rows) * new_cols + new_cols - 1]);
        }
      }

      // next newline
      new_row++;
      new_line_filled = 0;
    }
  }

  out_rect->col = new_line_filled - 1;  // maybe -1
  out_rect->row = new_row;
  if (new_line_filled > 0) {
    if (out_buffer != NULL && new_row >= skip_rows) {
      for (int c = new_line_filled; c < new_cols; c++) {
        /* log_debug("clear cell2: %d:%d", new_row, c); */
        clearcell(screen, &out_buffer[(new_row - skip_rows) * new_cols + c]);
      }
    }
  }
}

static void reflow_sb_line(VTermScreen *screen,
                           const VTermScreenCell *sb_line,
                           int sb_line_len,
                           int new_cols,
                           VTermPos *out_rect,
                           ScreenCell *out_buffer, int skip_rows,
                           bool clear_free_cells) {
  int new_row = 0;
  int sb_cell_taken = 0;

  while (1) {
    int sb_cell_have = sb_line_len - sb_cell_taken;

    if (sb_cell_have <= new_cols) {
      // ok! done

      if (out_buffer != NULL && new_row >= skip_rows) {
        // [sb_cell_taken, to end]
        for (int col = sb_cell_taken; col < sb_line_len; col += sb_line[col].width) {
          const VTermScreenCell *src = &sb_line[col];
          ScreenCell *dst = &out_buffer[(new_row -skip_rows) * new_cols + (col - sb_cell_taken)];
          copy_sb_cell_to_screen_cell(screen, dst, src);
          if(src->width == 2 && col < (new_cols-1))
            (dst + 1)->chars[0] = (uint32_t) -1;
        }
      }

      // clear rest cell.
      if (out_buffer != NULL && new_row >= skip_rows) {
        if (clear_free_cells)
          for (int col = sb_cell_have; col < new_cols; col++) {
            clearcell(screen, &out_buffer[(new_row - skip_rows) * new_cols + col]);
          }
      }

      out_rect->row = new_row;
      out_rect->col = sb_cell_have - 1;
      break;
    } else {
      if (out_buffer != NULL && new_row >= skip_rows) {
        // [sb_cell_taken...] for new_cols size
        for (int col = sb_cell_taken; col < sb_cell_taken + new_cols; col += sb_line[col].width) {
          const VTermScreenCell *src = &sb_line[col];
          ScreenCell *dst = &out_buffer[(new_row - skip_rows) * new_cols + (col - sb_cell_taken)];
          copy_sb_cell_to_screen_cell(screen, dst, src);
          if(src->width == 2 && col < (new_cols-1))
            (dst + 1)->chars[0] = (uint32_t) -1;
        }
      }

      sb_cell_taken += new_cols;

      if (sb_cell_taken == sb_line_len) {
        // done!
        out_rect->row = new_row;
        out_rect->col = new_cols - 1;
        break;
      }

      const VTermScreenCell *cell = &sb_line[sb_cell_taken - 1];
      if (cell->width > 1) {
        sb_cell_taken--;
        if (out_buffer != NULL && new_row >= skip_rows) {
          // clear the line end cell
          clearcell(screen, &out_buffer[(new_row - skip_rows) * new_cols + new_cols - 1]);
        }
      }
      // next new row
      new_row++;
    }
  }
}

// to fill the first line free cells, shift the rest continuation lines up.
static int combine_contination_lines(VTermScreen *screen, ScreenCell *buffer, int row_start,
                                     int rows, int cols,
                                     VTermLineInfo *lineinfo,
                                     int *empty_row) {
  log_debug("combine_contination_lines entry for new_row_start: %d", row_start);
  int delta_count = 0;

  int target_row = row_start;
  int target_row_line_count = line_popcount(buffer, target_row, cols);

  if (target_row_line_count == cols) {
    // the first line already full. do nothing
    return delta_count;
  }

  int src_row = target_row + 1;

  while (1) {
    if (src_row >= rows)
      break;

    if (!lineinfo[src_row].continuation) {
      break;
    }

    int target_line_spare = cols - target_row_line_count;

    ScreenCell *current_src_line = &buffer[src_row * cols];

    int src_line_count = line_popcount(buffer, src_row, cols);

    int move_up_count = target_line_spare;

    log_debug("src_row: %d: target_line_spare: %d, src_line_count: %d, target_row_line_count: %d, cols: %d",
              src_row, target_line_spare, src_line_count, target_row_line_count, cols);

    if (src_line_count <= move_up_count) {
      log_debug("combine whole line. done");
      // move/copy the whole line up.  target_row, target_row_line_count;
      memmove(&buffer[target_row * cols + target_row_line_count],
              &buffer[src_row * cols], src_line_count * sizeof(ScreenCell));

      target_row_line_count += src_line_count;

      delta_count--;

      if (empty_row != NULL)
        *empty_row = src_row;

      /* done */
      break;

    } else {
      log_debug("combine split, target_line_spare: %d, src_line_count: %d, target_row_line_count: %d, new_cols: %d",
                target_line_spare, src_line_count, target_row_line_count, cols);
      // long source line. split it.
      ScreenCell *cell = &current_src_line[move_up_count];
      bool wrap_line_end = false;
      if (cell->chars[0] == (uint32_t)-1) {
        move_up_count--;
        wrap_line_end = true;
      }

      if (move_up_count == 0) {
        log_debug("combine split? not split");
        // no split! target line can not fill with one wide cell.
        // the clear the end cell
        clearcell(screen, &buffer[target_row * cols + cols - 1]);
        target_row++;
        /* done */
        break;
      }

      log_debug("combine. split");
      int part2_count = src_line_count - move_up_count;

      // move first part line: target_row, target_row_line_count;
      memmove(&buffer[target_row * cols + target_row_line_count],
              &buffer[src_row * cols], move_up_count * sizeof(ScreenCell));
      if (wrap_line_end) {
        clearcell(screen, &buffer[target_row * cols + cols - 1]);
      }

      // move the rest to line begin: target_row + 1;
      memmove(&buffer[(target_row + 1) * cols],
              &buffer[src_row * cols + move_up_count], part2_count * sizeof(ScreenCell));

      target_row++;
      target_row_line_count = part2_count;
      src_row++;
    }
  }

  for (int i = target_row_line_count; i < cols; ++i) {
    clearcell(screen, &buffer[target_row * cols + i]);
  }

  return delta_count;
}

static void move_lines_down(VTermScreen *screen, ScreenCell *buffer,
                            int row_start, int down_line_step,
                            int move_line_count, /* how many line to be moved */
                            int cols, VTermLineInfo *lineinfo) {
  log_debug("move_line_count: row_start: %d, down_line_step: %d, move_line_count: %d, cols: %d",
            row_start, down_line_step, move_line_count, cols);
  // move the line down.
  memmove(&buffer[(row_start + down_line_step) * cols],
          &buffer[row_start * cols],
          (move_line_count * cols) * sizeof(ScreenCell));

  // update line info.
  memmove(&lineinfo[row_start + down_line_step], &lineinfo[row_start],
          move_line_count * sizeof(VTermLineInfo));

  // clear the empty line
  for (int i = 0; i < down_line_step; i++) {
    for (int j = 0; j < cols; ++j) {
      clearcell(screen, &buffer[(row_start + i) * cols + j]);
    }
  }
}

static bool shift_down_continuation_lines(
    VTermScreen *screen, ScreenCell *line_buffer, int row_index, int rows,
    int cols, int shift_count, VTermLineInfo *lineinfo) {

  if (row_index >= rows)
    return false;

  if (!lineinfo[row_index].continuation) {
    return false;
  }

  int cell_count = line_popcount(line_buffer, row_index, cols);

  if (cols - cell_count >= shift_count) {
    // done!
    // memmove and clear cell if needed!
    memmove(&line_buffer[row_index * cols + shift_count],
            &line_buffer[row_index * cols],
            sizeof(ScreenCell) * cell_count);
    for (int i = shift_count + cell_count; i < cols; i++) {
      clearcell(screen, &line_buffer[row_index * cols + i]);
    }

    return true;
  }

  // split the line
  int part1_count = cols - cell_count;
  bool clear_line_end = false;
  ScreenCell *cell = &line_buffer[row_index * cols + part1_count];
  if (cell->chars[0] ==  (uint32_t)-1) {
    part1_count--;
    clear_line_end = true;
  }

  int part2_count = cell_count - part1_count;

  if (!shift_down_continuation_lines(screen, line_buffer, row_index + 1, rows,
                                     cols, part2_count, lineinfo))
    return false;

  // copy the part2 to next rows
  memmove(&line_buffer[row_index * cols + part1_count],
          &line_buffer[(row_index + 1) * cols],
          sizeof(ScreenCell) * part2_count);

  // memmov the part1.
  memmove(&line_buffer[row_index * cols + shift_count],
          &line_buffer[row_index * cols], sizeof(ScreenCell) * part1_count);
  for (int i = shift_count + part1_count; i < cols; i++) {
    clearcell(screen, &line_buffer[row_index * cols + i]);
  }

  return true;
}

static void resize_buffer(VTermScreen *screen, int bufidx, int new_rows, int new_cols, bool active, VTermStateFields *statefields)
{
  log_debug("resize_buffer: bufidx: %d ------------------", bufidx);
  int old_rows = screen->rows;
  int old_cols = screen->cols;

  ScreenCell *old_buffer = screen->buffers[bufidx];
  VTermLineInfo *old_lineinfo = statefields->lineinfos[bufidx];

  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * new_rows * new_cols);
  VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);

  int old_row = old_rows - 1;
  int new_row = new_rows - 1;

  VTermPos old_cursor = statefields->pos;
  /* VTermPos new_cursor = { -1, -1 }; */
  VTermPos new_cursor = { 0, 0 };

  bool non_copy_row_met = false;

  while(old_row >= 0) {
    int old_row_end = old_row;
    /* TODO: Stop if dwl or dhl */
    while(old_lineinfo && old_row >= 0 && old_lineinfo[old_row].continuation)
      old_row--;
    if (old_row < 0) {
      /* the first line is continuation */
      old_row = 0;
    }
    int old_row_start = old_row;

    VTermPos out_rect;
    reflow_line(screen, old_buffer, old_row_start, old_row_end, old_cols,
                new_cols, &out_rect, NULL, 0, NULL, NULL, 0);

    int width = new_cols * out_rect.row + out_rect.col + 1;

    if (bufidx == BUFIDX_PRIMARY)
      log_debug("line width: %d, old_row_start: %d, %d", width, old_row_start,
                old_row_end);

    if (width == 0) {
      if (!non_copy_row_met) {
        /* skip this blank line */
        int cc = line_popcount(old_buffer, old_row_start, old_cols);
        log_debug("skip blank line: old_row_start: %d-%d, out_rect: %d:%d, cc:%d",
                  old_row_start,
                  old_row_end,
                  out_rect.row, out_rect.col,
                  cc);
        old_row = old_row_start - 1;
        continue;
      }
    } else {
      non_copy_row_met = true;
    }

    /* if(final_blank_row == (new_row + 1) && width == 0) */
    /*   final_blank_row = new_row; */

    int new_height = out_rect.row + 1;

    int new_row_end = new_row;
    int new_row_start = new_row - new_height + 1;

    old_row = old_row_start;
    int old_col = 0;

    if(new_row_start < 0) {
      if(old_row_start <= old_cursor.row && old_cursor.row < old_row_end) {
        new_cursor.row = 0;
        new_cursor.col = old_cursor.col;
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols-1;
      }

      // TODO: take partially and push the rest back.
      old_row = old_row_end;
      new_row = new_row_end;
      log_debug("break here? new_row_start < 0? %d, old_row_start: %d, old_row_end: %d, old_row: %d",
                new_row_start, old_row_start, old_row_end, old_row);
      break;
    }

    /* log_debug("refline ooo: new_row_start: %d:%d", new_row_start, new_row_end); */
    int skip_rows = 0;
    reflow_line(screen, old_buffer, old_row_start, old_row_end, old_cols,
                new_cols, &out_rect,
                &new_buffer[new_row_start * new_cols], skip_rows, &old_cursor,
                &new_cursor, new_row_start);
    for (new_row = new_row_start + 1; new_row <= new_row_end; ++new_row) {
      new_lineinfo[new_row].continuation = true;
    }

    new_lineinfo[new_row_start].continuation = old_lineinfo[old_row_start].continuation || skip_rows > 0;

    /* log_debug("rs continuation: %d, new_row_start: %d, old_row_start: %d ", */
    /*           new_lineinfo[new_row_start].continuation, */
    /*           new_row_start, old_row_start); */

    old_row = old_row_start - 1;
    new_row = new_row_start - 1;
  }

  if(old_cursor.row <= old_row) {
    /* cursor would have moved entirely off the top of the screen; lets just
     * bring it within range */
    new_cursor.row = 0, new_cursor.col = old_cursor.col;
    if(new_cursor.col >= new_cols)
      new_cursor.col = new_cols-1;
  }

  /* We really expect the cursor position to be set by now */
  if(active && (new_cursor.row == -1 || new_cursor.col == -1)) {
    fprintf(stderr, "screen_resize failed to update cursor position\n");
    abort();
  }

  if(old_row >= 0 && bufidx == BUFIDX_PRIMARY) {
    /* Push spare lines to scrollback buffer */
    if((screen->callbacks && screen->callbacks->sb_pushline) ||
       (screen->callbacks_has_pushline4 && screen->callbacks && screen->callbacks->sb_pushline4)) {
      /* TODO */
      ScreenCell *tmp = screen->buffer;
      int tmp_rows = screen->rows;
      int tmp_cols = screen->cols;
      screen->buffer = old_buffer;  /* screen->buffer right now might point to alt screen buffer */
      screen->rows = old_rows;
      screen->cols = old_cols;
      for(int row = 0; row <= old_row; row++) {
        const VTermLineInfo *lineinfo = old_lineinfo + row;
        sb_pushline_from_row_with_cols(screen, row, lineinfo->continuation, old_cols);
      }
      screen->buffer = tmp;
      screen->rows = tmp_rows;
      screen->cols = tmp_cols;
    }
    if(active)
      statefields->pos.row -= (old_row + 1);
  }

  if(!screen->with_conpty &&
      new_row >= 0 && bufidx == BUFIDX_PRIMARY &&
      screen->callbacks && screen->callbacks->sb_popline &&
      screen->callbacks->sb_peek) {
    /* Try to backfill rows by popping scrollback buffer */
    while(new_row >= 0) {
      int pop_cols = old_cols;
      bool continuation = false;
      const VTermScreenCell *sb_buffer;
      if (!(screen->callbacks->sb_peek(&pop_cols, &sb_buffer, &continuation, screen->cbdata)))
        break;

      /* calc the real sb line count */
      pop_cols = sb_line_popcount(sb_buffer, pop_cols);

      /* reflow the pop line */
      bool below_new_row_contination = (new_row < new_rows -1) && new_lineinfo[new_row + 1].continuation;
      int below_row_index = new_row + 1;

      log_debug("new_row: %d, pop_line: count: %d, continuation: %d, below_new_row_contination: %d",
                new_row, pop_cols, continuation, below_new_row_contination);

      if (pop_cols > new_cols) {
        VTermPos out_rect;
        reflow_sb_line(screen, sb_buffer, pop_cols, new_cols, &out_rect,
                       NULL, 0, true);
        int height = out_rect.row + 1;
        if (new_row < out_rect.row) {
          // TODO: no enough room! take partial line
          log_debug("(todo) reflow_sb_line: long line. no enough room. todo: "
                    "take partial line and push back the rest. or merg with "
                    "below contination line");
          break;
        } else {
          log_debug("reflow_sb_line: long line: easy case");
          int start_row = new_row - out_rect.row;
          reflow_sb_line(screen, sb_buffer, pop_cols, new_cols,
                         &out_rect, &new_buffer[start_row * new_cols], 0, false);
          for (int i = start_row + 1; i <= new_row; i++) {
            new_lineinfo[i].continuation = true;
          }
          new_lineinfo[start_row].continuation = continuation;

          int delta = 0;
          if (below_new_row_contination) {
            int empty_row;
            delta = combine_contination_lines(
                screen, new_buffer, below_row_index - 1, new_rows, new_cols,
                new_lineinfo, &empty_row);
            log_debug("combine(1): delta: %d, empty_row: %d", delta, empty_row);

            if (delta < 0) {
              move_lines_down(screen, new_buffer, start_row, -delta,
                              empty_row - start_row, new_cols, new_lineinfo);
            }

            new_row -= delta;
            log_debug("combined(1) line: row: %d continuation: %d", new_row,
                      new_lineinfo[new_row].continuation);
          }

          new_row -= (out_rect.row + 1);
          if (active)
            statefields->pos.row += (out_rect.row + 1) + delta;
        }
      } else {
        // short line. copy to new_buffer directly
        VTermPos out_rect;
        reflow_sb_line(screen, sb_buffer, pop_cols, new_cols, &out_rect,
                       &new_buffer[new_row * new_cols], 0, true);
        new_lineinfo[new_row].continuation = continuation;

        int delta = 0;
        if (below_new_row_contination) {
          // short line. need to combine.
          log_debug("reflow_sb_line: short: need combine lines below");

          int empty_row;
          delta = combine_contination_lines(screen, new_buffer, new_row, new_rows,
                                            new_cols, new_lineinfo, &empty_row);
          log_debug("combine: delta: %d, empty_row: %d", delta, empty_row);

          if (delta < 0) {
            int start_row = new_row;
            move_lines_down(screen, new_buffer, start_row, -delta,
                            empty_row - start_row, new_cols, new_lineinfo);
          }

          new_row -= delta;
          log_debug("combined line: row: %d continuation: %d", new_row, new_lineinfo[new_row].continuation);
        }

        new_row--;
        if(active)
          statefields->pos.row += 1 + delta;
      }

      screen->callbacks->sb_popline(pop_cols, NULL, screen->cbdata);

    }  // while (new_row >= 0)
  }
  if(new_row >= 0) {
    /* Scroll new rows back up to the top and fill in blanks at the bottom */
    int moverows = new_rows - new_row - 1;
    memmove(&new_buffer[0], &new_buffer[(new_row + 1) * new_cols], moverows * new_cols * sizeof(ScreenCell));
    memmove(&new_lineinfo[0], &new_lineinfo[new_row + 1], moverows * sizeof(new_lineinfo[0]));

    new_cursor.row -= (new_row + 1);

    for(new_row = moverows; new_row < new_rows; new_row++) {
      for(int col = 0; col < new_cols; col++)
        clearcell(screen, &new_buffer[new_row * new_cols + col]);
      new_lineinfo[new_row] = (VTermLineInfo){ 0 };
    }
  } else if(bufidx == BUFIDX_PRIMARY &&
            screen->callbacks && screen->callbacks->sb_popline &&
            screen->callbacks->sb_peek) {

    // case new_row < 0 and new_lineinfo[0].continuation == true
    // may able to pop line
    while (1) {
      int pop_cols = old_cols;
      bool continuation = false;
      const VTermScreenCell *sb_buffer;

      if (!(screen->callbacks->sb_peek(&pop_cols, &sb_buffer, &continuation,
                                       screen->cbdata)))
        break;

      /* calc the real sb line count */
      pop_cols = sb_line_popcount(sb_buffer, pop_cols);

      VTermPos out_rect;
      reflow_sb_line(screen, sb_buffer, pop_cols, new_cols, &out_rect,
                     NULL, 0, true);
      if (out_rect.row > 0)
        break;

      if (!shift_down_continuation_lines(screen, new_buffer, 0, new_rows,
                                         new_cols, out_rect.col + 1,
                                         new_lineinfo))
        break;

      reflow_sb_line(screen, sb_buffer, pop_cols, new_cols, &out_rect,
                     &new_buffer[0], 0, false);

      screen->callbacks->sb_popline(pop_cols, NULL, screen->cbdata);

      new_lineinfo[0].continuation = continuation;
    }
  }

  vterm_allocator_free(screen->vt, old_buffer);
  screen->buffers[bufidx] = new_buffer;

  vterm_allocator_free(screen->vt, old_lineinfo);
  statefields->lineinfos[bufidx] = new_lineinfo;

  /* log_debug("new cursor %d:%d, active: %d", new_cursor.row, new_cursor.col, active); */
  if(active)
    statefields->pos = new_cursor;

  /* if (bufidx == BUFIDX_PRIMARY) { */
  /*   for (int i = 0; i < new_rows; i++) { */
  /*     log_debug("after resize: lineinfo: row: %d: width: %d", */
  /*               i, line_popcount(new_buffer, i, new_cols)); */
  /*   } */
  /* } */

  return;
}

static int resize(int new_rows, int new_cols, VTermStateFields *fields, void *user)
{
  VTermScreen *screen = user;

  int altscreen_active = (screen->buffers[BUFIDX_ALTSCREEN] && screen->buffer == screen->buffers[BUFIDX_ALTSCREEN]);

  int old_rows = screen->rows;
  int old_cols = screen->cols;

  /* Ensure that ->sb_buffer is large enough for a new or and old row */
  ensure_sb_buffer_cols(screen, new_cols);

  resize_buffer(screen, 0, new_rows, new_cols, !altscreen_active, fields);
  if(screen->buffers[BUFIDX_ALTSCREEN])
    resize_buffer(screen, 1, new_rows, new_cols, altscreen_active, fields);
  else if(new_rows != old_rows) {
    /* We don't need a full resize of the altscreen because it isn't enabled
     * but we should at least keep the lineinfo the right size */
    if (fields->lineinfos[BUFIDX_ALTSCREEN])
      vterm_allocator_free(screen->vt, fields->lineinfos[BUFIDX_ALTSCREEN]);

    VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);
    for(int row = 0; row < new_rows; row++)
      new_lineinfo[row] = (VTermLineInfo){ 0 };

    fields->lineinfos[BUFIDX_ALTSCREEN] = new_lineinfo;
  }

  screen->buffer = altscreen_active ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];

  screen->rows = new_rows;
  screen->cols = new_cols;

  alloc_sb_buffer(screen, new_cols);

  /* TODO: Maaaaybe we can optimise this if there's no reflow happening */
  damagescreen(screen);

  if(screen->callbacks && screen->callbacks->resize)
    return (*screen->callbacks->resize)(new_rows, new_cols, screen->cbdata);

  return 1;
}

static int setlineinfo(int row, const VTermLineInfo *newinfo, const VTermLineInfo *oldinfo, void *user)
{
  VTermScreen *screen = user;

  if(newinfo->doublewidth != oldinfo->doublewidth ||
     newinfo->doubleheight != oldinfo->doubleheight) {
    for(int col = 0; col < screen->cols; col++) {
      ScreenCell *cell = getcell(screen, row, col);
      cell->pen.dwl = newinfo->doublewidth;
      cell->pen.dhl = newinfo->doubleheight;
    }

    VTermRect rect = {
      .start_row = row,
      .end_row   = row + 1,
      .start_col = 0,
      .end_col   = newinfo->doublewidth ? screen->cols / 2 : screen->cols,
    };
    damagerect(screen, rect);

    if(newinfo->doublewidth) {
      rect.start_col = screen->cols / 2;
      rect.end_col   = screen->cols;

      erase_internal(rect, 0, user);
    }
  }

  return 1;
}

static int sb_clear(void *user) {
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->sb_clear)
    if((*screen->callbacks->sb_clear)(screen->cbdata))
      return 1;

  return 0;
}

static VTermStateCallbacks state_cbs = {
  .putglyph    = &putglyph,
  .movecursor  = &movecursor,
  .premove     = &premove,
  .scrollrect  = &scrollrect,
  .erase       = &erase,
  .setpenattr  = &setpenattr,
  .settermprop = &settermprop,
  .bell        = &bell,
  .resize      = &resize,
  .setlineinfo = &setlineinfo,
  .sb_clear    = &sb_clear,
};

static VTermScreen *screen_new(VTerm *vt)
{
  VTermState *state = vterm_obtain_state(vt);
  if(!state)
    return NULL;

  VTermScreen *screen = vterm_allocator_malloc(vt, sizeof(VTermScreen));
  int rows, cols;

  vterm_get_size(vt, &rows, &cols);

  screen->vt = vt;
  screen->state = state;

  screen->damage_merge = VTERM_DAMAGE_CELL;
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;

  screen->rows = rows;
  screen->cols = cols;

  screen->global_reverse = false;
  screen->reflow = false;

  screen->callbacks = NULL;
  screen->cbdata    = NULL;
  screen->callbacks_has_pushline4 = false;

  screen->buffers[BUFIDX_PRIMARY] = alloc_buffer(screen, rows, cols);

  screen->buffer = screen->buffers[BUFIDX_PRIMARY];

  screen->sb_buffer = NULL;
  alloc_sb_buffer(screen, cols);

  vterm_state_set_callbacks(screen->state, &state_cbs, screen);
  vterm_state_callbacks_has_premove(screen->state);

  return screen;
}

INTERNAL void vterm_screen_free(VTermScreen *screen)
{
  vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_PRIMARY]);
  if(screen->buffers[BUFIDX_ALTSCREEN])
    vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_ALTSCREEN]);

  vterm_allocator_free(screen->vt, screen->sb_buffer);

  vterm_allocator_free(screen->vt, screen);
}

void vterm_screen_reset(VTermScreen *screen, int hard)
{
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;
  vterm_state_reset(screen->state, hard);
  vterm_screen_flush_damage(screen);
}

static size_t _get_chars(const VTermScreen *screen, const int utf8, void *buffer, size_t len, const VTermRect rect)
{
  size_t outpos = 0;
  int padding = 0;

#define PUT(c)                                             \
  if(utf8) {                                               \
    size_t thislen = utf8_seqlen(c);                       \
    if(buffer && outpos + thislen <= len)                  \
      outpos += fill_utf8((c), (char *)buffer + outpos);   \
    else                                                   \
      outpos += thislen;                                   \
  }                                                        \
  else {                                                   \
    if(buffer && outpos + 1 <= len)                        \
      ((uint32_t*)buffer)[outpos++] = (c);                 \
    else                                                   \
      outpos++;                                            \
  }

  for(int row = rect.start_row; row < rect.end_row; row++) {
    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(cell->chars[0] == 0)
        // Erased cell, might need a space
        padding++;
      else if(cell->chars[0] == (uint32_t)-1)
        // Gap behind a double-width char, do nothing
        ;
      else {
        while(padding) {
          PUT(UNICODE_SPACE);
          padding--;
        }
        for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++) {
          PUT(cell->chars[i]);
        }
      }
    }

    if(row < rect.end_row - 1) {
      PUT(UNICODE_LINEFEED);
      padding = 0;
    }
  }

  return outpos;
}

size_t vterm_screen_get_chars(const VTermScreen *screen, uint32_t *chars, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 0, chars, len, rect);
}

size_t vterm_screen_get_text(const VTermScreen *screen, char *str, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 1, str, len, rect);
}

/* Copy internal to external representation of a screen cell */
int vterm_screen_get_cell(const VTermScreen *screen, VTermPos pos, VTermScreenCell *cell)
{
  ScreenCell *intcell = getcell(screen, pos.row, pos.col);
  if(!intcell)
    return 0;

  if (intcell->chars[0] == (uint32_t) -1) {
    return 0;
  }

  for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
    cell->chars[i] = intcell->chars[i];
    if(!intcell->chars[i])
      break;
  }

  cell->attrs.bold      = intcell->pen.bold;
  cell->attrs.underline = intcell->pen.underline;
  cell->attrs.italic    = intcell->pen.italic;
  cell->attrs.blink     = intcell->pen.blink;
  cell->attrs.reverse   = intcell->pen.reverse ^ screen->global_reverse;
  cell->attrs.conceal   = intcell->pen.conceal;
  cell->attrs.strike    = intcell->pen.strike;
  cell->attrs.font      = intcell->pen.font;
  cell->attrs.small     = intcell->pen.small;
  cell->attrs.baseline  = intcell->pen.baseline;

  cell->attrs.dwl = intcell->pen.dwl;
  cell->attrs.dhl = intcell->pen.dhl;

  cell->fg = intcell->pen.fg;
  cell->bg = intcell->pen.bg;

  if(pos.col < (screen->cols - 1) &&
     getcell(screen, pos.row, pos.col + 1)->chars[0] == (uint32_t)-1)
    cell->width = 2;
  else
    cell->width = 1;

  return 1;
}

int vterm_screen_is_eol(const VTermScreen *screen, VTermPos pos)
{
  /* This cell is EOL if this and every cell to the right is black */
  for(; pos.col < screen->cols; pos.col++) {
    ScreenCell *cell = getcell(screen, pos.row, pos.col);
    if(cell->chars[0] != 0)
      return 0;
  }

  return 1;
}

VTermScreen *vterm_obtain_screen(VTerm *vt)
{
  if(vt->screen)
    return vt->screen;

  VTermScreen *screen = screen_new(vt);
  vt->screen = screen;

  return screen;
}

void vterm_screen_enable_reflow(VTermScreen *screen, bool reflow)
{
  screen->reflow = reflow;
}

#undef vterm_screen_set_reflow
void vterm_screen_set_reflow(VTermScreen *screen, bool reflow)
{
  vterm_screen_enable_reflow(screen, reflow);
}

void vterm_screen_set_with_conpty(VTermScreen *screen, bool with_conpty)
{
  screen->with_conpty = with_conpty;
}

void vterm_screen_enable_altscreen(VTermScreen *screen, int altscreen)
{
  if(!screen->buffers[BUFIDX_ALTSCREEN] && altscreen) {
    int rows, cols;
    vterm_get_size(screen->vt, &rows, &cols);

    screen->buffers[BUFIDX_ALTSCREEN] = alloc_buffer(screen, rows, cols);
  }
}

void vterm_screen_set_callbacks(VTermScreen *screen, const VTermScreenCallbacks *callbacks, void *user)
{
  screen->callbacks = callbacks;
  screen->cbdata = user;
}

void *vterm_screen_get_cbdata(VTermScreen *screen)
{
  return screen->cbdata;
}

void vterm_screen_callbacks_has_pushline4(VTermScreen *screen)
{
  screen->callbacks_has_pushline4 = true;
}

void vterm_screen_set_unrecognised_fallbacks(VTermScreen *screen, const VTermStateFallbacks *fallbacks, void *user)
{
  vterm_state_set_unrecognised_fallbacks(screen->state, fallbacks, user);
}

void *vterm_screen_get_unrecognised_fbdata(VTermScreen *screen)
{
  return vterm_state_get_unrecognised_fbdata(screen->state);
}

void vterm_screen_flush_damage(VTermScreen *screen)
{
  if(screen->pending_scrollrect.start_row != -1) {
    vterm_scroll_rect(screen->pending_scrollrect, screen->pending_scroll_downward, screen->pending_scroll_rightward,
        moverect_user, erase_user, screen);

    screen->pending_scrollrect.start_row = -1;
  }

  if(screen->damaged.start_row != -1) {
    if(screen->callbacks && screen->callbacks->damage)
      (*screen->callbacks->damage)(screen->damaged, screen->cbdata);

    screen->damaged.start_row = -1;
  }
}

void vterm_screen_set_damage_merge(VTermScreen *screen, VTermDamageSize size)
{
  vterm_screen_flush_damage(screen);
  screen->damage_merge = size;
}

static int attrs_differ(VTermAttrMask attrs, ScreenCell *a, ScreenCell *b)
{
  if((attrs & VTERM_ATTR_BOLD_MASK)       && (a->pen.bold != b->pen.bold))
    return 1;
  if((attrs & VTERM_ATTR_UNDERLINE_MASK)  && (a->pen.underline != b->pen.underline))
    return 1;
  if((attrs & VTERM_ATTR_ITALIC_MASK)     && (a->pen.italic != b->pen.italic))
    return 1;
  if((attrs & VTERM_ATTR_BLINK_MASK)      && (a->pen.blink != b->pen.blink))
    return 1;
  if((attrs & VTERM_ATTR_REVERSE_MASK)    && (a->pen.reverse != b->pen.reverse))
    return 1;
  if((attrs & VTERM_ATTR_CONCEAL_MASK)    && (a->pen.conceal != b->pen.conceal))
    return 1;
  if((attrs & VTERM_ATTR_STRIKE_MASK)     && (a->pen.strike != b->pen.strike))
    return 1;
  if((attrs & VTERM_ATTR_FONT_MASK)       && (a->pen.font != b->pen.font))
    return 1;
  if((attrs & VTERM_ATTR_FOREGROUND_MASK) && !vterm_color_is_equal(&a->pen.fg, &b->pen.fg))
    return 1;
  if((attrs & VTERM_ATTR_BACKGROUND_MASK) && !vterm_color_is_equal(&a->pen.bg, &b->pen.bg))
    return 1;
  if((attrs & VTERM_ATTR_SMALL_MASK)    && (a->pen.small != b->pen.small))
    return 1;
  if((attrs & VTERM_ATTR_BASELINE_MASK)    && (a->pen.baseline != b->pen.baseline))
    return 1;

  return 0;
}

int vterm_screen_get_attrs_extent(const VTermScreen *screen, VTermRect *extent, VTermPos pos, VTermAttrMask attrs)
{
  ScreenCell *target = getcell(screen, pos.row, pos.col);

  // TODO: bounds check
  extent->start_row = pos.row;
  extent->end_row   = pos.row + 1;

  if(extent->start_col < 0)
    extent->start_col = 0;
  if(extent->end_col < 0)
    extent->end_col = screen->cols;

  int col;

  for(col = pos.col - 1; col >= extent->start_col; col--)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->start_col = col + 1;

  for(col = pos.col + 1; col < extent->end_col; col++)
    if(attrs_differ(attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->end_col = col - 1;

  return 1;
}

void vterm_screen_convert_color_to_rgb(const VTermScreen *screen, VTermColor *col)
{
  vterm_state_convert_color_to_rgb(screen->state, col);
}

static void reset_default_colours(VTermScreen *screen, ScreenCell *buffer)
{
  for(int row = 0; row <= screen->rows - 1; row++)
    for(int col = 0; col <= screen->cols - 1; col++) {
      ScreenCell *cell = &buffer[row * screen->cols + col];
      if(VTERM_COLOR_IS_DEFAULT_FG(&cell->pen.fg))
        cell->pen.fg = screen->pen.fg;
      if(VTERM_COLOR_IS_DEFAULT_BG(&cell->pen.bg))
        cell->pen.bg = screen->pen.bg;
    }
}

void vterm_screen_set_default_colors(VTermScreen *screen, const VTermColor *default_fg, const VTermColor *default_bg)
{
  vterm_state_set_default_colors(screen->state, default_fg, default_bg);

  if(default_fg && VTERM_COLOR_IS_DEFAULT_FG(&screen->pen.fg)) {
    screen->pen.fg = *default_fg;
    screen->pen.fg.type = (screen->pen.fg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_FG;
  }

  if(default_bg && VTERM_COLOR_IS_DEFAULT_BG(&screen->pen.bg)) {
    screen->pen.bg = *default_bg;
    screen->pen.bg.type = (screen->pen.bg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_BG;
  }

  reset_default_colours(screen, screen->buffers[0]);
  if(screen->buffers[1])
    reset_default_colours(screen, screen->buffers[1]);
}
