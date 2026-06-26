#include <pebble.h>
#include <stdio.h>
#include <message_keys.auto.h>
#include "pblv_player.h"

// This project compiles app sources into a shared object dir (build/src/...), while resource IDs
// are generated per-platform (build/<platform>/src/...). To keep things simple and portable here,
// we hardcode the raw resource ID for BEACH_PBLV.
// If you add more resources later, consider adjusting the build to export a common header.
#ifndef RESOURCE_ID_BEACH_PBLV
#define RESOURCE_ID_BEACH_PBLV 1
#endif

#ifndef RESOURCE_ID_DIGITS
#define RESOURCE_ID_DIGITS 2
#endif

#ifndef RESOURCE_ID_LETTERS
#define RESOURCE_ID_LETTERS 3
#endif

#define DIGIT_COUNT 11
#define DIGIT_COLON_INDEX 10

#define DATE_ATLAS_SCALE 2

#define DATE_ATLAS_DIGIT_COUNT 11
#define DATE_ATLAS_ALPHA_COUNT 26
#define DATE_DIGIT_HEIGHT (8 * DATE_ATLAS_SCALE)
#define DATE_ALPHA_HEIGHT (10 * DATE_ATLAS_SCALE)
#define DATE_DIGIT_ROW_Y 0
#define DATE_UPPER_ROW_Y (8 * DATE_ATLAS_SCALE)
#define DATE_LOWER_ROW_Y (17 * DATE_ATLAS_SCALE)
#define DATE_SPACE_WIDTH (4 * DATE_ATLAS_SCALE)
#define DATE_LINE_HEIGHT (10 * DATE_ATLAS_SCALE)
#define DATE_GAP_ABOVE_TIME (2 * DATE_ATLAS_SCALE)

#ifdef PBL_PLATFORM_EMERY
#define TIME_OFFSET_Y -28
#elif PBL_PLATFORM_GABBRO
#define TIME_OFFSET_Y -12
#endif

#define LOOP_PAUSE_FRAME_INDEX 3
#define LOOP_PAUSE_DURATION_DEFAULT 5000
#define LOOP_PAUSE_DURATION_TICK_TRIGGER 60000

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

static PblvPlayer s_player;
static bool s_player_ready;
static uint32_t s_loop_pause_duration = LOOP_PAUSE_DURATION_DEFAULT;
static bool s_waiting_for_tick_loop;
static uint32_t s_waiting_for_tick_delay_ms;
static bool s_show_date = true;

static GBitmap *s_digits;
static GBitmap *s_digit_sub[DIGIT_COUNT];
static GBitmap *s_letters;
static GBitmap *s_letter_digit_sub[DATE_ATLAS_DIGIT_COUNT];
static GBitmap *s_letter_upper_sub[DATE_ATLAS_ALPHA_COUNT];
static GBitmap *s_letter_lower_sub[DATE_ATLAS_ALPHA_COUNT];

static const int16_t s_digit_x[DIGIT_COUNT] = { 0, 42, 72, 114, 156, 198, 240, 282, 324, 366, 408 };
static const uint8_t s_digit_w[DIGIT_COUNT] = { 40, 28, 40, 40, 40, 40, 40, 40, 40, 40, 16 };

static const int16_t s_letter_digit_x[DATE_ATLAS_DIGIT_COUNT] = {
  0, 20, 32, 48, 64, 80, 96, 114, 128, 146, 160
};
static const uint8_t s_letter_digit_w[DATE_ATLAS_DIGIT_COUNT] = {
  14, 10, 16, 16, 16, 16, 14, 14, 16, 14, 16
};

static const int16_t s_letter_upper_x[DATE_ATLAS_ALPHA_COUNT] = {
  2, 18, 34, 50, 66, 80, 96, 114, 132, 146, 162, 178, 192,
  208, 226, 242, 258, 274, 288, 306, 320, 338, 352, 368, 388, 402
};
static const uint8_t s_letter_upper_w[DATE_ATLAS_ALPHA_COUNT] = {
  14, 14, 14, 14, 16, 18, 16, 14, 10, 14, 12, 16, 18,
  16, 14, 14, 14, 16, 18, 16, 16, 16, 18, 16, 12, 14
};

static const int16_t s_letter_lower_x[DATE_ATLAS_ALPHA_COUNT] = {
  0, 18, 34, 48, 64, 82, 98, 112, 130, 144, 162, 178, 194,
  208, 224, 242, 258, 274, 290, 306, 320, 338, 354, 370, 386, 402
};
static const uint8_t s_letter_lower_w[DATE_ATLAS_ALPHA_COUNT] = {
  14, 10, 16, 16, 14, 12, 16, 12, 12, 12, 10, 10, 16,
  14, 14, 10, 10, 12, 10, 14, 14, 10, 12, 10, 12, 10
};

static void prv_format_date_text(const struct tm *time_info, char *buffer, size_t buffer_size) {
  static const char *const s_weekdays[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  if(!buffer || buffer_size == 0) {
    return;
  }

  if(!time_info || time_info->tm_wday < 0 || time_info->tm_wday > 6) {
    buffer[0] = '\0';
    return;
  }

  snprintf(buffer, buffer_size, "%s %d/%d",
           s_weekdays[time_info->tm_wday], time_info->tm_mon + 1, time_info->tm_mday);
}

static GBitmap *prv_date_bitmap_for_char(char c, int16_t *width_out,
                                         int16_t *height_out, int16_t *y_offset_out) {
  if(width_out) {
    *width_out = 0;
  }
  if(height_out) {
    *height_out = 0;
  }
  if(y_offset_out) {
    *y_offset_out = 0;
  }

  if(c >= '0' && c <= '9') {
    const int index = c - '0';
    if(width_out) {
      *width_out = s_letter_digit_w[index];
    }
    if(height_out) {
      *height_out = DATE_DIGIT_HEIGHT;
    }
    if(y_offset_out) {
      *y_offset_out = (DATE_LINE_HEIGHT - DATE_DIGIT_HEIGHT) / 2;
    }
    return s_letter_digit_sub[index];
  }

  if(c == '/') {
    if(width_out) {
      *width_out = s_letter_digit_w[10];
    }
    if(height_out) {
      *height_out = DATE_DIGIT_HEIGHT;
    }
    if(y_offset_out) {
      *y_offset_out = (DATE_LINE_HEIGHT - DATE_DIGIT_HEIGHT) / 2;
    }
    return s_letter_digit_sub[10];
  }

  if(c >= 'A' && c <= 'Z') {
    const int index = c - 'A';
    if(width_out) {
      *width_out = s_letter_upper_w[index];
    }
    if(height_out) {
      *height_out = DATE_ALPHA_HEIGHT;
    }
    return s_letter_upper_sub[index];
  }

  if(c >= 'a' && c <= 'z') {
    const int index = c - 'a';
    if(width_out) {
      *width_out = s_letter_lower_w[index];
    }
    if(height_out) {
      *height_out = DATE_ALPHA_HEIGHT;
    }
    return s_letter_lower_sub[index];
  }

  return NULL;
}

static int16_t prv_measure_date_text(const char *text) {
  int16_t width = 0;

  if(!text) {
    return 0;
  }

  for(const char *cursor = text; *cursor; cursor++) {
    if(*cursor == ' ') {
      width = (int16_t)(width + DATE_SPACE_WIDTH);
      continue;
    }

    int16_t glyph_width = 0;
    if(prv_date_bitmap_for_char(*cursor, &glyph_width, NULL, NULL) && glyph_width > 0) {
      width = (int16_t)(width + glyph_width);
    }
  }

  return width;
}

static void prv_draw_date_text(GContext *ctx, int16_t time_right, int16_t time_y, const char *text) {
  if(!ctx || !text || !s_letters) {
    return;
  }

  const int16_t total_w = prv_measure_date_text(text);
  if(total_w <= 0) {
    return;
  }

  int16_t x = (int16_t)(time_right - total_w);
  const int16_t y = (int16_t)(time_y - DATE_LINE_HEIGHT - DATE_GAP_ABOVE_TIME);

  for(const char *cursor = text; *cursor; cursor++) {
    if(*cursor == ' ') {
      x = (int16_t)(x + DATE_SPACE_WIDTH);
      continue;
    }

    int16_t glyph_width = 0;
    int16_t glyph_height = 0;
    int16_t glyph_y_offset = 0;
    GBitmap *glyph = prv_date_bitmap_for_char(*cursor, &glyph_width, &glyph_height, &glyph_y_offset);
    if(!glyph || glyph_width <= 0 || glyph_height <= 0) {
      continue;
    }

    graphics_draw_bitmap_in_rect(ctx, glyph,
                                 GRect(x, y + glyph_y_offset, glyph_width, glyph_height));
    x = (int16_t)(x + glyph_width);
  }
}

static uint32_t prv_loop_pause_duration_from_int(int32_t candidate) {
  switch(candidate) {
    case 0:
    case 2000:
    case 5000:
    case 10000:
    case 15000:
    case 30000:
    case 60000:
      return (uint32_t)candidate;
    default:
      return LOOP_PAUSE_DURATION_DEFAULT;
  }
}

static void prv_set_loop_pause_duration(uint32_t duration_ms) {
  s_loop_pause_duration = duration_ms;
  persist_write_int(MESSAGE_KEY_ANIMATION_LOOP_DELAY, (int32_t)duration_ms);
}

static void prv_inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *loop_pause = dict_find(iter, MESSAGE_KEY_ANIMATION_LOOP_DELAY);
  if(loop_pause) {
    uint32_t duration_ms = LOOP_PAUSE_DURATION_DEFAULT;
    switch(loop_pause->type) {
      case TUPLE_INT:
        duration_ms = prv_loop_pause_duration_from_int(loop_pause->value->int32);
        break;
      case TUPLE_UINT:
        duration_ms = prv_loop_pause_duration_from_int((int32_t)loop_pause->value->uint32);
        break;
      default:
        break;
    }
    prv_set_loop_pause_duration(duration_ms);
  }

  Tuple *show_date = dict_find(iter, MESSAGE_KEY_SHOW_DATE);
  if(show_date) {
    bool enabled = false;
    switch(show_date->type) {
      case TUPLE_INT:
        enabled = show_date->value->int32 != 0;
        break;
      case TUPLE_UINT:
        enabled = show_date->value->uint32 != 0;
        break;
      default:
        break;
    }
    s_show_date = enabled;
    persist_write_bool(MESSAGE_KEY_SHOW_DATE, enabled);
    if(s_canvas_layer) {
      layer_mark_dirty(s_canvas_layer);
    }
  }
}

static void prv_schedule_next_frame(void);
static void prv_timer_cb(void *context);

static void prv_schedule_frame_after_delay(uint32_t ms) {
  s_timer = app_timer_register(ms, prv_timer_cb, NULL);
}

static void update_face(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (t) {
    const bool is_24h = clock_is_24h_style();
    int hour = t->tm_hour;
    const int minute = t->tm_min;

    if(!is_24h) {
      hour %= 12;
      if(hour == 0) {
        hour = 12;
      }
    }
    // Update the sprites tile_id and h_flip flag to show Marin looking left
    if ((minute == 00 && hour == 0)
        || (minute == 11 && hour == 11)
        || (minute == 22 && hour == 22)
        || (!is_24h &&
          ((minute == 11 && hour == 1)
          || (minute == 22 && hour == 2)
          || (minute == 33 && hour == 3)
          || (minute == 44 && hour == 4)
          || (minute == 55 && hour == 5)))) {
      s_player.oam[12][2] = 70;
      s_player.oam[13][2] = 72;
      s_player.oam[13][3] = 0x01;
    } else {
      s_player.oam[12][2] = 64;
      s_player.oam[13][2] = 64;
      s_player.oam[13][3] = 0x21;
    }
  }
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if(s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }

  update_face();

  if(!s_waiting_for_tick_loop) {
    return;
  }

  s_waiting_for_tick_loop = false;
  prv_schedule_frame_after_delay(s_waiting_for_tick_delay_ms);
}

static void prv_draw_time(GContext *ctx, GRect bounds) {
  if(!s_digits || !s_digit_sub[DIGIT_COLON_INDEX]) {
    return;
  }

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if(!t) {
    return;
  }

  const bool is_24h = clock_is_24h_style();
  int hour = t->tm_hour;
  const int minute = t->tm_min;

  if(!is_24h) {
    hour %= 12;
    if(hour == 0) {
      hour = 12;
    }
  }

  const int h_tens = hour / 10;
  const int h_ones = hour % 10;
  const bool has_h_tens = is_24h || (h_tens > 0);
  const int m_tens = minute / 10;
  const int m_ones = minute % 10;

  const GRect digits_bounds = gbitmap_get_bounds(s_digits);
  const int16_t digit_h = digits_bounds.size.h;

  const int16_t spacing = 2;
  const int16_t colon_w = s_digit_w[DIGIT_COLON_INDEX];
  const int16_t total_w = (int16_t)(
      (has_h_tens ? (s_digit_w[h_tens] + spacing) : 0) +
      s_digit_w[h_ones] + spacing +
      colon_w + spacing +
      s_digit_w[m_tens] + spacing +
      s_digit_w[m_ones]);

  int16_t x = (int16_t)(bounds.origin.x + (bounds.size.w - total_w) / 2);
  const int16_t time_right = (int16_t)(x + total_w);
  const int16_t y = (int16_t)(bounds.origin.y + (bounds.size.h - digit_h) / 2 + TIME_OFFSET_Y);

  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  if(s_show_date) {
    char date_text[16];
    prv_format_date_text(t, date_text, sizeof(date_text));
    prv_draw_date_text(ctx, time_right, y, date_text);
  }

  if(has_h_tens) {
    graphics_draw_bitmap_in_rect(ctx, s_digit_sub[h_tens],
                                 GRect(x, y, s_digit_w[h_tens], digit_h));
    x = (int16_t)(x + s_digit_w[h_tens] + spacing);
  }

  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[h_ones],
                               GRect(x, y, s_digit_w[h_ones], digit_h));
  x = (int16_t)(x + s_digit_w[h_ones] + spacing);

  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[DIGIT_COLON_INDEX],
                               GRect(x, y, colon_w, digit_h));
  x = (int16_t)(x + colon_w + spacing);

  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[m_tens],
                               GRect(x, y, s_digit_w[m_tens], digit_h));
  x = (int16_t)(x + s_digit_w[m_tens] + spacing);

  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[m_ones],
                               GRect(x, y, s_digit_w[m_ones], digit_h));
}

static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  if(!s_player_ready) {
    return;
  }

  const GRect bounds = layer_get_bounds(layer);
  pblv_player_render(&s_player, ctx, bounds);
  prv_draw_time(ctx, bounds);
}

static void prv_timer_cb(void *context) {
  s_timer = NULL;

  if(!s_player_ready) {
    return;
  }

  // Apply the frame that just became due.
  if(!pblv_player_apply_pending(&s_player)) {
    return;
  }

  // Request a redraw; we render into the system framebuffer in update_proc.
  layer_mark_dirty(s_canvas_layer);

  // Queue next frame.
  prv_schedule_next_frame();
}

static void prv_schedule_next_frame(void) {
  if(!s_player_ready) {
    return;
  }
  if(!pblv_player_load_next_header(&s_player)) {
    return;
  }

  const uint32_t frame_delay_ms = pblv_player_delta_frames_to_ms(s_player.pending_delta_frames);

  if(s_loop_pause_duration == LOOP_PAUSE_DURATION_TICK_TRIGGER && s_player.pending_looped) {
    s_waiting_for_tick_loop = true;
    s_waiting_for_tick_delay_ms = frame_delay_ms;
    return;
  }

  uint32_t extra_delay_ms = 0;
  if(s_loop_pause_duration != LOOP_PAUSE_DURATION_TICK_TRIGGER
      && s_player.pending_loop_count > 0
      && s_player.pending_frame_index == LOOP_PAUSE_FRAME_INDEX) {
    extra_delay_ms = s_loop_pause_duration;
  }
  prv_schedule_frame_after_delay(frame_delay_ms + extra_delay_ms);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  const ResHandle res = resource_get_handle(RESOURCE_ID_BEACH_PBLV);
  s_player_ready = pblv_player_init(&s_player, res);

  // Hide the 3px of trees seen on the edges of non-round pebbles after loading by copying
  // tile data from 2 rows inwards to the edge rows.
#ifndef PBL_ROUND
  if(s_player_ready) {
    for (int y = 6; y <= 10; y++) {
      s_player.map[3 + y * 20] = s_player.map[5 + y * 20];
      s_player.map[16 + y * 20] = s_player.map[14 + y * 20];
    }
  }
#endif

  s_digits = gbitmap_create_with_resource(RESOURCE_ID_DIGITS);
  if(s_digits) {
    const int16_t digits_h = gbitmap_get_bounds(s_digits).size.h;
    for(int i = 0; i < DIGIT_COUNT; i++) {
      s_digit_sub[i] = gbitmap_create_as_sub_bitmap(s_digits, GRect(s_digit_x[i], 0, s_digit_w[i], digits_h));
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to load digits.png");
  }

  s_letters = gbitmap_create_with_resource(RESOURCE_ID_LETTERS);
  if(s_letters) {
    for(int i = 0; i < DATE_ATLAS_DIGIT_COUNT; i++) {
      s_letter_digit_sub[i] = gbitmap_create_as_sub_bitmap(
          s_letters, GRect(s_letter_digit_x[i], DATE_DIGIT_ROW_Y,
                           s_letter_digit_w[i], DATE_DIGIT_HEIGHT));
    }

    for(int i = 0; i < DATE_ATLAS_ALPHA_COUNT; i++) {
      s_letter_upper_sub[i] = gbitmap_create_as_sub_bitmap(
          s_letters, GRect(s_letter_upper_x[i], DATE_UPPER_ROW_Y,
                           s_letter_upper_w[i], DATE_ALPHA_HEIGHT));
      s_letter_lower_sub[i] = gbitmap_create_as_sub_bitmap(
          s_letters, GRect(s_letter_lower_x[i], DATE_LOWER_ROW_Y,
                           s_letter_lower_w[i], DATE_ALPHA_HEIGHT));
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to load letters.png");
  }

  if(s_player_ready) {
    // Render keyframe immediately.
    layer_mark_dirty(s_canvas_layer);

    // Schedule the first frame.
    prv_schedule_next_frame();
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to init PBLV player");
  }
}

static void prv_window_unload(Window *window) {
  if(s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }

  if(s_player_ready) {
    pblv_player_deinit(&s_player);
    s_player_ready = false;
  }

  for(int i = 0; i < DIGIT_COUNT; i++) {
    if(s_digit_sub[i]) {
      gbitmap_destroy(s_digit_sub[i]);
      s_digit_sub[i] = NULL;
    }
  }

  for(int i = 0; i < DATE_ATLAS_DIGIT_COUNT; i++) {
    if(s_letter_digit_sub[i]) {
      gbitmap_destroy(s_letter_digit_sub[i]);
      s_letter_digit_sub[i] = NULL;
    }
  }

  for(int i = 0; i < DATE_ATLAS_ALPHA_COUNT; i++) {
    if(s_letter_upper_sub[i]) {
      gbitmap_destroy(s_letter_upper_sub[i]);
      s_letter_upper_sub[i] = NULL;
    }
    if(s_letter_lower_sub[i]) {
      gbitmap_destroy(s_letter_lower_sub[i]);
      s_letter_lower_sub[i] = NULL;
    }
  }

  if(s_letters) {
    gbitmap_destroy(s_letters);
    s_letters = NULL;
  }

  if(s_digits) {
    gbitmap_destroy(s_digits);
    s_digits = NULL;
  }

  layer_destroy(s_canvas_layer);
}

static void prv_init(void) {
  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_open(128, 16);
  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);

  if(persist_exists(MESSAGE_KEY_ANIMATION_LOOP_DELAY)) {
    s_loop_pause_duration = prv_loop_pause_duration_from_int(
        persist_read_int(MESSAGE_KEY_ANIMATION_LOOP_DELAY));
  }

  if(persist_exists(MESSAGE_KEY_SHOW_DATE)) {
    s_show_date = persist_read_bool(MESSAGE_KEY_SHOW_DATE);
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  const bool animated = true;
  window_stack_push(s_window, animated);
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", s_window);

  app_event_loop();
  prv_deinit();
}
