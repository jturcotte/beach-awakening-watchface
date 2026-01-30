#include "pblv_player.h"

#define PBLV_MAGIC 0x564c4250u // 'PBLV' little-endian
#define PBLV_KEYF  0x4659454bu // 'KEYF' little-endian

#define PBLV_TILE_PX 16u
#define PBLV_TILE_BYTES 64u

static bool prv_res_read(ResHandle res, uint32_t offset, void *out, size_t len) {
  const size_t got = resource_load_byte_range(res, offset, out, len);
  return got == len;
}

static uint16_t prv_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t prv_u32le(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }


bool pblv_player_init(PblvPlayer *player, ResHandle res) {
  if(!player) {
    return false;
  }

  memset(player, 0, sizeof(*player));
  player->res = res;

  uint8_t header[32];
  if(!prv_res_read(res, 0, header, sizeof(header))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read header");
    return false;
  }

  const uint32_t magic = prv_u32le(&header[0x00]);
  if(magic != PBLV_MAGIC) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: bad magic 0x%08lx", (unsigned long)magic);
    return false;
  }

  player->version = prv_u16le(&header[0x04]);
  const uint16_t header_size = prv_u16le(&header[0x06]);
  player->keyframe_offset = prv_u32le(&header[0x08]);
  player->frames_offset = prv_u32le(&header[0x0C]);
  player->n_frames = prv_u32le(&header[0x10]);
  player->map_w = prv_u16le(&header[0x14]);
  player->map_h = prv_u16le(&header[0x16]);
  player->flags = prv_u32le(&header[0x18]);

  if(player->version != 1 || header_size != 32) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: unsupported version=%u header=%u", (unsigned)player->version, (unsigned)header_size);
    return false;
  }

  if(player->map_w != 20 || player->map_h != 18) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PBLV: unexpected map size %ux%u (renderer assumes 20x18)",
            (unsigned)player->map_w, (unsigned)player->map_h);
  }

  // Keyframe marker + length
  uint8_t keyf_marker[8];
  if(!prv_res_read(res, player->keyframe_offset, keyf_marker, sizeof(keyf_marker))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read KEYF marker");
    return false;
  }
  const uint32_t keyf_tag = prv_u32le(&keyf_marker[0]);
  if(keyf_tag != PBLV_KEYF) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: missing KEYF tag 0x%08lx", (unsigned long)keyf_tag);
    return false;
  }

  uint32_t off = player->keyframe_offset + 8;

  // Keyframe params (8 bytes), then tileCount/bankCount/mapW/mapH/reserved (12 bytes)
  uint8_t keyf_params[8 + 12];
  if(!prv_res_read(res, off, keyf_params, sizeof(keyf_params))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read KEYF params");
    return false;
  }
  off += sizeof(keyf_params);

  const uint16_t tile_count = prv_u16le(&keyf_params[8]);
  const uint16_t bank_count = prv_u16le(&keyf_params[10]);

  if(tile_count != 384) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PBLV: tileCount=%u (expected 384)", (unsigned)tile_count);
  }
  if(bank_count < 1 || bank_count > 2) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: unsupported bankCount=%u", (unsigned)bank_count);
    return false;
  }

  // BG palettes
  uint8_t pal_hdr[2];
  if(!prv_res_read(res, off, pal_hdr, sizeof(pal_hdr))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read palette length");
    return false;
  }
  const uint16_t n_palette_bytes = prv_u16le(pal_hdr);
  off += 2;

  if(n_palette_bytes != sizeof(player->palette_bytes)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PBLV: nPaletteBytes=%u (expected %u)",
            (unsigned)n_palette_bytes, (unsigned)sizeof(player->palette_bytes));
  }

  const uint16_t pal_to_read = (n_palette_bytes < sizeof(player->palette_bytes)) ? n_palette_bytes : sizeof(player->palette_bytes);
  if(!prv_res_read(res, off, player->palette_bytes, pal_to_read)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read palette bytes");
    return false;
  }
  if(pal_to_read < sizeof(player->palette_bytes)) {
    memset(&player->palette_bytes[pal_to_read], 0, sizeof(player->palette_bytes) - pal_to_read);
  }
  off += n_palette_bytes;

  // Tile table
  const uint32_t tile_table_bytes = (uint32_t)bank_count * (uint32_t)tile_count * PBLV_TILE_BYTES;
  const uint32_t max_supported = 2u * 384u * PBLV_TILE_BYTES;
  if(tile_table_bytes > max_supported) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: tile table too large (%lu)", (unsigned long)tile_table_bytes);
    return false;
  }

  for(uint16_t bank = 0; bank < bank_count; bank++) {
    for(uint16_t t = 0; t < tile_count && t < 384; t++) {
      if(!prv_res_read(res, off, player->tiles[bank][t], PBLV_TILE_BYTES)) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed reading tile bank=%u idx=%u", (unsigned)bank, (unsigned)t);
        return false;
      }
      off += PBLV_TILE_BYTES;
    }
  }

  // Visible BG map (20x18), 4 bytes per cell
  const uint16_t mw = player->map_w;
  const uint16_t mh = player->map_h;
  const uint32_t n_cells = (uint32_t)mw * (uint32_t)mh;

  for(uint32_t i = 0; i < n_cells && i < (20u * 18u); i++) {
    uint8_t cell[4];
    if(!prv_res_read(res, off, cell, sizeof(cell))) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed reading map cell %lu", (unsigned long)i);
      return false;
    }
    player->map[i].tile_key = prv_u16le(&cell[0]);
    player->map[i].palette = cell[2];
    off += 4;
  }

  player->frame_index = 0;
  player->frame_offset = player->frames_offset;
  player->has_pending_header = false;

  APP_LOG(APP_LOG_LEVEL_INFO, "PBLV: loaded keyframe (frames=%lu flags=0x%lx)",
          (unsigned long)player->n_frames, (unsigned long)player->flags);
  return true;
}

void pblv_player_deinit(PblvPlayer *player) {
  if(!player) {
    return;
  }
  // Nothing heap-owned inside player.
}

bool pblv_player_load_next_header(PblvPlayer *player) {
  if(!player || player->frames_offset == 0 || player->n_frames == 0) {
    return false;
  }

  // Loop frames forever. This intentionally does NOT re-apply the keyframe;
  // well-formed PBLV files can make the frame stream cyclic.
  if(player->frame_index >= player->n_frames) {
    player->frame_index = 0;
    player->frame_offset = player->frames_offset;
  }

  uint8_t hdr[8];
  if(!prv_res_read(player->res, player->frame_offset, hdr, sizeof(hdr))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed reading frame header idx=%lu", (unsigned long)player->frame_index);
    return false;
  }

  player->pending_delta_frames = prv_u16le(&hdr[0]);
  player->pending_n_pal = prv_u16le(&hdr[2]);
  player->pending_n_tile = prv_u16le(&hdr[4]);
  player->pending_n_map = prv_u16le(&hdr[6]);
  player->pending_updates_offset = player->frame_offset + 8;
  player->has_pending_header = true;

  return true;
}

bool pblv_player_apply_pending(PblvPlayer *player) {
  if(!player || !player->has_pending_header) {
    return false;
  }

  uint32_t off = player->pending_updates_offset;

  // Palette updates
  for(uint16_t i = 0; i < player->pending_n_pal; i++) {
    uint8_t rec[4];
    if(!prv_res_read(player->res, off, rec, sizeof(rec))) {
      return false;
    }
    const uint8_t pal = rec[0];
    const uint8_t col = rec[1];
    const uint8_t gcol = rec[2];
    if(pal < 8 && col < 4) {
      player->palette_bytes[pal * 4 + col] = gcol;
    }
    off += 4;
  }

  // Tile updates
  for(uint16_t i = 0; i < player->pending_n_tile; i++) {
    uint8_t rec[4 + PBLV_TILE_BYTES];
    if(!prv_res_read(player->res, off, rec, sizeof(rec))) {
      return false;
    }
    const uint16_t tile_key = prv_u16le(&rec[0]);
    const uint16_t tile_index = (uint16_t)(tile_key & 0x7FFF);
    const uint16_t bank = (uint16_t)((tile_key >> 15) & 1);
    if(bank < 2 && tile_index < 384) {
      memcpy(player->tiles[bank][tile_index], &rec[4], PBLV_TILE_BYTES);
    }
    off += (uint32_t)sizeof(rec);
  }

  // Map updates
  for(uint16_t i = 0; i < player->pending_n_map; i++) {
    uint8_t rec[6];
    if(!prv_res_read(player->res, off, rec, sizeof(rec))) {
      return false;
    }
    const uint16_t map_index = prv_u16le(&rec[0]);
    const uint16_t tile_key = prv_u16le(&rec[2]);
    const uint8_t pal = rec[4];
    if(map_index < (20u * 18u)) {
      player->map[map_index].tile_key = tile_key;
      player->map[map_index].palette = pal;
    }
    off += 6;
  }

  // Advance
  player->frame_offset = off;
  player->frame_index++;
  player->has_pending_header = false;

  return true;
}

static uint8_t prv_tile_px_16x16(const uint8_t tile[PBLV_TILE_BYTES], uint8_t x, uint8_t y) {
  // 16 pixels/row -> 4 bytes/row, 4 pixels/byte.
  const uint8_t b = tile[y * 4 + (x >> 2)];
  const uint8_t shift = (uint8_t)(6 - (x & 3) * 2);
  return (b >> shift) & 0x03;
}

static uint8_t prv_gcolor_to_bw(uint8_t gcolor8) {
  // gcolor8 = 0bAARRGGBB (2 bits per channel). Use simple luminance-ish threshold.
  const uint8_t r = (gcolor8 >> 4) & 0x03;
  const uint8_t g = (gcolor8 >> 2) & 0x03;
  const uint8_t b = (gcolor8 >> 0) & 0x03;
  const uint8_t lum = (uint8_t)(r + g + b); // 0..9
  // Return 1 for "dark" pixels.
  return lum <= 4;
}

void pblv_player_render(const PblvPlayer *player, GBitmap *out) {
  if(!player || !out) {
    return;
  }

  const GSize size = gbitmap_get_bounds(out).size;

  if(size.w <= 0 || size.h <= 0) {
    return;
  }

  uint8_t *data = (uint8_t *)gbitmap_get_data(out);
  const int row_bytes = gbitmap_get_bytes_per_row(out);

  const GBitmapFormat fmt = gbitmap_get_format(out);

  // Clear the whole output buffer so areas outside the map don't keep old pixels.
  memset(data, 0, (size_t)row_bytes * (size_t)size.h);

  const uint16_t map_w = (player->map_w <= 20) ? player->map_w : 20;
  const uint16_t map_h = (player->map_h <= 18) ? player->map_h : 18;

  // Render the visible portion of the map from origin (0,0) and clip to the output size.
  const uint16_t out_tiles_w = (uint16_t)(((uint16_t)size.w + (PBLV_TILE_PX - 1u)) / PBLV_TILE_PX);
  const uint16_t out_tiles_h = (uint16_t)(((uint16_t)size.h + (PBLV_TILE_PX - 1u)) / PBLV_TILE_PX);
  const uint16_t draw_tiles_w = (out_tiles_w < map_w) ? out_tiles_w : map_w;
  const uint16_t draw_tiles_h = (out_tiles_h < map_h) ? out_tiles_h : map_h;

  for(uint16_t ty = 0; ty < draw_tiles_h; ty++) {
    for(uint16_t tx = 0; tx < draw_tiles_w; tx++) {
      const uint32_t map_index = (uint32_t)tx + (uint32_t)ty * 20u;
      const uint16_t tile_key = player->map[map_index].tile_key;
      const uint16_t tile_index = (uint16_t)(tile_key & 0x7FFF);
      const uint16_t bank = (uint16_t)((tile_key >> 15) & 1);
      const uint8_t pal = (uint8_t)(player->map[map_index].palette & 7);

      const uint8_t *tile = (bank < 2 && tile_index < 384) ? player->tiles[bank][tile_index] : player->tiles[0][0];

      for(uint8_t py = 0; py < PBLV_TILE_PX; py++) {
        const int y = (int)ty * (int)PBLV_TILE_PX + py;
        if(y < 0 || y >= size.h) {
          break;
        }

        if(fmt == GBitmapFormat8Bit) {
          uint8_t *row = data + y * row_bytes;
          for(uint8_t px = 0; px < PBLV_TILE_PX; px++) {
            const int x = (int)tx * (int)PBLV_TILE_PX + px;
            if(x < 0 || x >= size.w) {
              break;
            }

            const uint8_t p = prv_tile_px_16x16(tile, px, py);
            const uint8_t gcol = player->palette_bytes[pal * 4 + p];
            row[x] = gcol;
          }
        } else if(fmt == GBitmapFormat1Bit) {
          uint8_t *row = data + y * row_bytes;
          for(uint8_t px = 0; px < PBLV_TILE_PX; px++) {
            const int x = (int)tx * (int)PBLV_TILE_PX + px;
            if(x < 0 || x >= size.w) {
              break;
            }

            const uint8_t p = prv_tile_px_16x16(tile, px, py);
            const uint8_t gcol = player->palette_bytes[pal * 4 + p];
            const uint8_t dark = prv_gcolor_to_bw(gcol);

            const int byte_index = x >> 3;
            const uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            if(dark) {
              row[byte_index] |= bit;
            } else {
              row[byte_index] &= (uint8_t)~bit;
            }
          }
        }
      }
    }
  }
}
