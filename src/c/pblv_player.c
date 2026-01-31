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

  player->scratch_tile = gbitmap_create_blank(GSize(PBLV_TILE_PX, PBLV_TILE_PX), GBitmapFormat2BitPalette);
  if(!player->scratch_tile) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to allocate scratch tile");
    return false;
  }

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

  player->lcdc = keyf_params[1];

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

  // OBJ palettes
  uint8_t obj_pal_hdr[2];
  if(!prv_res_read(res, off, obj_pal_hdr, sizeof(obj_pal_hdr))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read OBJ palette length");
    return false;
  }
  const uint16_t n_obj_palette_bytes = prv_u16le(obj_pal_hdr);
  off += 2;

  if(n_obj_palette_bytes != sizeof(player->obj_palette_bytes)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PBLV: nObjPaletteBytes=%u (expected %u)",
            (unsigned)n_obj_palette_bytes, (unsigned)sizeof(player->obj_palette_bytes));
  }

  const uint16_t obj_pal_to_read = (n_obj_palette_bytes < sizeof(player->obj_palette_bytes)) ? n_obj_palette_bytes : sizeof(player->obj_palette_bytes);
  if(!prv_res_read(res, off, player->obj_palette_bytes, obj_pal_to_read)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read OBJ palette bytes");
    return false;
  }
  if(obj_pal_to_read < sizeof(player->obj_palette_bytes)) {
    memset(&player->obj_palette_bytes[obj_pal_to_read], 0, sizeof(player->obj_palette_bytes) - obj_pal_to_read);
  }
  off += n_obj_palette_bytes;

  // OAM data
  uint8_t oam_hdr[2];
  if(!prv_res_read(res, off, oam_hdr, sizeof(oam_hdr))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read OAM length");
    return false;
  }
  const uint16_t oam_bytes = prv_u16le(oam_hdr);
  off += 2;

  if(oam_bytes != 160u) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PBLV: oamBytes=%u (expected 160)", (unsigned)oam_bytes);
  }

  const uint16_t oam_to_read = (oam_bytes < 160u) ? oam_bytes : 160u;
  if(!prv_res_read(res, off, player->oam, oam_to_read)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed to read OAM data");
    return false;
  }
  if(oam_to_read < 160u) {
    memset(((uint8_t *)player->oam) + oam_to_read, 0, 160u - oam_to_read);
  }
  off += oam_bytes;

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
  if(player->scratch_tile) {
    gbitmap_destroy(player->scratch_tile);
    player->scratch_tile = NULL;
  }
}

bool pblv_player_load_next_header(PblvPlayer *player) {
  if(!player || player->frames_offset == 0 || player->n_frames == 0) {
    return false;
  }

  // Loop frames forever. This intentionally does NOT re-apply the keyframe;
  // well-formed PBLV files can make the frame stream cyclic.
  bool looped = false;
  if(player->frame_index >= player->n_frames) {
    player->frame_index = 0;
    player->frame_offset = player->frames_offset;
    looped = true;
  }

  uint8_t hdr[12];
  if(!prv_res_read(player->res, player->frame_offset, hdr, sizeof(hdr))) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "PBLV: failed reading frame header idx=%lu", (unsigned long)player->frame_index);
    return false;
  }

  player->pending_delta_frames = prv_u16le(&hdr[0]);
  player->pending_n_pal = prv_u16le(&hdr[2]);
  player->pending_n_tile = prv_u16le(&hdr[4]);
  player->pending_n_map = prv_u16le(&hdr[6]);
  player->pending_n_obj_pal = prv_u16le(&hdr[8]);
  player->pending_n_oam = prv_u16le(&hdr[10]);
  player->pending_looped = looped;
  player->pending_updates_offset = player->frame_offset + 12;
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

  // OBJ palette updates
  for(uint16_t i = 0; i < player->pending_n_obj_pal; i++) {
    uint8_t rec[4];
    if(!prv_res_read(player->res, off, rec, sizeof(rec))) {
      return false;
    }
    const uint8_t pal = rec[0];
    const uint8_t col = rec[1];
    const uint8_t gcol = rec[2];
    if(pal < 8 && col < 4) {
      player->obj_palette_bytes[pal * 4 + col] = gcol;
    }
    off += 4;
  }

  // OAM updates
  for(uint16_t i = 0; i < player->pending_n_oam; i++) {
    uint8_t rec[8];
    if(!prv_res_read(player->res, off, rec, sizeof(rec))) {
      return false;
    }
    const uint16_t index = prv_u16le(&rec[0]);
    if(index < 40u) {
      memcpy(player->oam[index], &rec[4], 4);
    }
    off += 8;
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

static void prv_tile_set_px_16x16(uint8_t tile[PBLV_TILE_BYTES], uint8_t x, uint8_t y, uint8_t p) {
  const uint8_t idx = (uint8_t)(y * 4 + (x >> 2));
  const uint8_t shift = (uint8_t)(6 - (x & 3) * 2);
  tile[idx] = (uint8_t)((tile[idx] & ~(0x03 << shift)) | ((p & 0x03) << shift));
}

static void prv_copy_tile_flipped(uint8_t dst[PBLV_TILE_BYTES], const uint8_t src[PBLV_TILE_BYTES], bool xflip, bool yflip) {
  memset(dst, 0, PBLV_TILE_BYTES);
  for(uint8_t y = 0; y < PBLV_TILE_PX; y++) {
    for(uint8_t x = 0; x < PBLV_TILE_PX; x++) {
      const uint8_t sx = xflip ? (uint8_t)(PBLV_TILE_PX - 1 - x) : x;
      const uint8_t sy = yflip ? (uint8_t)(PBLV_TILE_PX - 1 - y) : y;
      const uint8_t p = prv_tile_px_16x16(src, sx, sy);
      prv_tile_set_px_16x16(dst, x, y, p);
    }
  }
}

void pblv_player_render(PblvPlayer *player, GContext *ctx, GRect bounds) {
  if(!player || !ctx || !player->scratch_tile) {
    return;
  }

  const GSize size = bounds.size;
  if(size.w <= 0 || size.h <= 0) {
    return;
  }

  const uint16_t map_w = (player->map_w <= 20) ? player->map_w : 20;
  const uint16_t map_h = (player->map_h <= 18) ? player->map_h : 18;

  const uint16_t draw_tiles_w = map_w;
  const uint16_t draw_tiles_h = map_h;

  graphics_context_set_compositing_mode(ctx, GCompOpAssign);

  const int16_t offset_x = -60;
  const int16_t offset_y = -48;

  uint8_t last_pal = 0xFF;
  GColor palette[4];

  for(uint16_t ty = 0; ty < draw_tiles_h; ty++) {
    for(uint16_t tx = 0; tx < draw_tiles_w; tx++) {
      const uint32_t map_index = (uint32_t)tx + (uint32_t)ty * 20u;
      const uint16_t tile_key = player->map[map_index].tile_key;
      const uint16_t tile_index = (uint16_t)(tile_key & 0x7FFF);
      const uint16_t bank = (uint16_t)((tile_key >> 15) & 1);
      const uint8_t pal = (uint8_t)(player->map[map_index].palette & 7);

      const uint8_t *tile = (bank < 2 && tile_index < 384) ? player->tiles[bank][tile_index] : player->tiles[0][0];

      if(pal != last_pal) {
        const uint8_t *pal_bytes = &player->palette_bytes[pal * 4];
        palette[0].argb = pal_bytes[0];
        palette[1].argb = pal_bytes[1];
        palette[2].argb = pal_bytes[2];
        palette[3].argb = pal_bytes[3];
        const bool free_on_destroy = false;
        gbitmap_set_palette(player->scratch_tile, palette, free_on_destroy);
        last_pal = pal;
      }

      gbitmap_set_data(player->scratch_tile, (uint8_t *)tile, GBitmapFormat2BitPalette, 4, false);

      const int16_t x = (int16_t)(bounds.origin.x + offset_x + (int16_t)tx * (int16_t)PBLV_TILE_PX);
      const int16_t y = (int16_t)(bounds.origin.y + offset_y + (int16_t)ty * (int16_t)PBLV_TILE_PX);
      graphics_draw_bitmap_in_rect(ctx, player->scratch_tile, GRect(x, y, PBLV_TILE_PX, PBLV_TILE_PX));
    }
  }

  // Sprites (OBJ)
  graphics_context_set_compositing_mode(ctx, GCompOpSet);

  uint8_t last_obj_pal = 0xFF;
  for(uint16_t i = 0; i < 40; i++) {
    const uint8_t oy = player->oam[i][0];
    const uint8_t ox = player->oam[i][1];
    const uint8_t tile_id = player->oam[i][2];
    const uint8_t attr = player->oam[i][3];

    if(ox == 0 || oy == 0) {
      continue;
    }

    const uint8_t pal = (uint8_t)(attr & 0x07);
    const uint8_t bank = (uint8_t)((attr >> 3) & 0x01);
    const bool xflip = (attr & 0x20) != 0;
    const bool yflip = (attr & 0x40) != 0;

    if(pal != last_obj_pal) {
      const uint8_t *pal_bytes = &player->obj_palette_bytes[pal * 4];
      palette[0].argb = (uint8_t)(pal_bytes[0] & 0x3F); // transparent color 0
      palette[1].argb = pal_bytes[1];
      palette[2].argb = pal_bytes[2];
      palette[3].argb = pal_bytes[3];
      const bool free_on_destroy = false;
      gbitmap_set_palette(player->scratch_tile, palette, free_on_destroy);
      last_obj_pal = pal;
    }

    const bool tall = (player->lcdc & 0x04) != 0; // OBJ size: 8x16
    const uint16_t base_tile = tall ? (uint16_t)(tile_id & 0xFE) : tile_id;

    const int16_t x = (int16_t)(bounds.origin.x + offset_x + ((int16_t)ox - 8) * 2);
    const int16_t y = (int16_t)(bounds.origin.y + offset_y + ((int16_t)oy - 16) * 2);

    const uint16_t tile0_index = base_tile;
    const uint16_t tile1_index = (uint16_t)(base_tile + 1);

    const uint8_t *tile0 = (bank < 2 && tile0_index < 384) ? player->tiles[bank][tile0_index] : player->tiles[0][0];
    const uint8_t *tile1 = (bank < 2 && tile1_index < 384) ? player->tiles[bank][tile1_index] : player->tiles[0][0];

    if(tall && yflip) {
      const uint8_t *tmp = tile0;
      tile0 = tile1;
      tile1 = tmp;
    }

    if(xflip || yflip) {
      prv_copy_tile_flipped(player->scratch_pixels, tile0, xflip, yflip);
      gbitmap_set_data(player->scratch_tile, player->scratch_pixels, GBitmapFormat2BitPalette, 4, false);
    } else {
      gbitmap_set_data(player->scratch_tile, (uint8_t *)tile0, GBitmapFormat2BitPalette, 4, false);
    }

    graphics_draw_bitmap_in_rect(ctx, player->scratch_tile, GRect(x, y, PBLV_TILE_PX, PBLV_TILE_PX));

    if(tall) {
      const int16_t y2 = (int16_t)(y + (int16_t)PBLV_TILE_PX);

      if(xflip || yflip) {
        prv_copy_tile_flipped(player->scratch_pixels, tile1, xflip, yflip);
        gbitmap_set_data(player->scratch_tile, player->scratch_pixels, GBitmapFormat2BitPalette, 4, false);
      } else {
        gbitmap_set_data(player->scratch_tile, (uint8_t *)tile1, GBitmapFormat2BitPalette, 4, false);
      }

      graphics_draw_bitmap_in_rect(ctx, player->scratch_tile, GRect(x, y2, PBLV_TILE_PX, PBLV_TILE_PX));
    }
  }
}
