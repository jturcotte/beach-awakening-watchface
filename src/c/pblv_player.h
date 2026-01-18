#pragma once

#include <pebble.h>

// Minimal PBLV (v1) player for rendering 20x18 GB BG maps.
// Designed to stream frame updates from a Pebble raw resource.

typedef struct {
  uint16_t tile_key;
  uint8_t palette;
} PblvMapCell;

typedef struct {
  ResHandle res;

  uint16_t version;
  uint32_t keyframe_offset;
  uint32_t frames_offset;
  uint32_t n_frames;

  uint16_t map_w;
  uint16_t map_h;
  uint32_t flags;

  // Decoded state
  uint8_t palette_bytes[32];           // 8 palettes x 4 colors (GColor8)
  uint8_t tiles[2][384][16];           // up to 2 banks, 384 tiles each
  PblvMapCell map[20 * 18];            // visible map cells

  // Playback cursor
  uint32_t frame_index;
  uint32_t frame_offset;

  // Next frame header cached for scheduling
  bool has_pending_header;
  uint16_t pending_delta_frames;
  uint16_t pending_n_pal;
  uint16_t pending_n_tile;
  uint16_t pending_n_map;
  uint32_t pending_updates_offset;
} PblvPlayer;

bool pblv_player_init(PblvPlayer *player, ResHandle res);
void pblv_player_deinit(PblvPlayer *player);

// Loads the next frame header into player->pending_* and returns false on EOF/error.
bool pblv_player_load_next_header(PblvPlayer *player);

// Applies the pending frame updates (must have pending header loaded).
bool pblv_player_apply_pending(PblvPlayer *player);

// Renders the current state into an output bitmap.
// - On color platforms, provide a GBitmapFormat8Bit bitmap sized 160x144.
// - On B/W (aplite), provide a GBitmapFormat1Bit bitmap sized 160x144.
void pblv_player_render(const PblvPlayer *player, GBitmap *out);

static inline uint32_t pblv_player_delta_frames_to_ms(uint16_t delta_frames) {
  const uint32_t ms = (uint32_t)delta_frames * 1000u / 60u;
  return ms == 0 ? 1 : ms;
}
