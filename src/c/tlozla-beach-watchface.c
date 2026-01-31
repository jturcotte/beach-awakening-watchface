#include <pebble.h>

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

#define DIGIT_COUNT 11
#define DIGIT_COLON_INDEX 10

#define LOOP_PAUSE_FRAME_INDEX 3
#define LOOP_PAUSE_DURATION 2000

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

static PblvPlayer s_player;
static bool s_player_ready;

static GBitmap *s_digits;
static GBitmap *s_digit_sub[DIGIT_COUNT];

static const int16_t s_digit_x[DIGIT_COUNT] = { 0, 42, 72, 114, 156, 198, 240, 282, 324, 366, 408 };
static const uint8_t s_digit_w[DIGIT_COUNT] = { 40, 28, 40, 40, 40, 40, 40, 40, 40, 40, 16 };

static void prv_schedule_next_frame(void);

static void prv_draw_time(GContext *ctx, GRect bounds) {
  if(!s_digits || !s_digit_sub[DIGIT_COLON_INDEX]) {
    return;
  }

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if(!t) {
    return;
  }

  int hour = t->tm_hour;
  const int minute = t->tm_min;

  if(!clock_is_24h_style()) {
    hour %= 12;
    if(hour == 0) {
      hour = 12;
    }
  }

  const int h_tens = hour / 10;
  const int h_ones = hour % 10;
  const bool has_h_tens = (h_tens > 0);
  const int m_tens = minute / 10;
  const int m_ones = minute % 10;

  const GRect digits_bounds = gbitmap_get_bounds(s_digits);
  const int16_t digit_h = digits_bounds.size.h;

  const int16_t spacing = 2;
  const int16_t colon_w = s_digit_w[DIGIT_COLON_INDEX];
  const int16_t colon_offset_x = 0; // static balance tweak
  const int16_t colon_offset_y = -32; // static balance tweak
  const int16_t colon_x = (int16_t)(bounds.origin.x + (bounds.size.w - colon_w) / 2 + colon_offset_x);
  const int16_t y = (int16_t)(bounds.origin.y + (bounds.size.h - digit_h) / 2 + colon_offset_y);

  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  // Colon centered.
  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[DIGIT_COLON_INDEX],
                               GRect(colon_x, y, colon_w, digit_h));

  // Minutes to the right of colon.
  int16_t x = (int16_t)(colon_x + colon_w + spacing);
  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[m_tens],
                               GRect(x, y, s_digit_w[m_tens], digit_h));
  x = (int16_t)(x + s_digit_w[m_tens] + spacing);
  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[m_ones],
                               GRect(x, y, s_digit_w[m_ones], digit_h));

  // Hours to the left of colon.
  x = (int16_t)(colon_x - spacing - s_digit_w[h_ones]);
  graphics_draw_bitmap_in_rect(ctx, s_digit_sub[h_ones],
                               GRect(x, y, s_digit_w[h_ones], digit_h));
  if(has_h_tens) {
    x = (int16_t)(x - spacing - s_digit_w[h_tens]);
    graphics_draw_bitmap_in_rect(ctx, s_digit_sub[h_tens],
                                 GRect(x, y, s_digit_w[h_tens], digit_h));
  }
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
  uint32_t ms = pblv_player_delta_frames_to_ms(s_player.pending_delta_frames);
  if(s_player.pending_loop_count > 0 && s_player.pending_frame_index == LOOP_PAUSE_FRAME_INDEX) {
    ms += LOOP_PAUSE_DURATION;
  }
  s_timer = app_timer_register(ms, prv_timer_cb, NULL);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  const ResHandle res = resource_get_handle(RESOURCE_ID_BEACH_PBLV);
  s_player_ready = pblv_player_init(&s_player, res);

  s_digits = gbitmap_create_with_resource(RESOURCE_ID_DIGITS);
  if(s_digits) {
    const int16_t digits_h = gbitmap_get_bounds(s_digits).size.h;
    for(int i = 0; i < DIGIT_COUNT; i++) {
      s_digit_sub[i] = gbitmap_create_as_sub_bitmap(s_digits, GRect(s_digit_x[i], 0, s_digit_w[i], digits_h));
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to load digits.png");
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
  if(s_digits) {
    gbitmap_destroy(s_digits);
    s_digits = NULL;
  }

  layer_destroy(s_canvas_layer);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  const bool animated = true;
  window_stack_push(s_window, animated);
}

static void prv_deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  prv_init();

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p", s_window);

  app_event_loop();
  prv_deinit();
}
