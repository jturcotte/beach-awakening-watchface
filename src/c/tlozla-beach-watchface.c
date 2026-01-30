#include <pebble.h>

#include "pblv_player.h"

// This project compiles app sources into a shared object dir (build/src/...), while resource IDs
// are generated per-platform (build/<platform>/src/...). To keep things simple and portable here,
// we hardcode the raw resource ID for BEACH_PBLV.
// If you add more resources later, consider adjusting the build to export a common header.
#ifndef RESOURCE_ID_BEACH_PBLV
#define RESOURCE_ID_BEACH_PBLV 1
#endif

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

static PblvPlayer s_player;
static bool s_player_ready;

static void prv_schedule_next_frame(void);

static void prv_canvas_update_proc(Layer *layer, GContext *ctx) {
  if(!s_player_ready) {
    return;
  }

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if(!fb) {
    return;
  }

  pblv_player_render(&s_player, fb);

  graphics_release_frame_buffer(ctx, fb);
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
  const uint32_t ms = pblv_player_delta_frames_to_ms(s_player.pending_delta_frames);
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
