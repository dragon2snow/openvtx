#include "ppu.hpp"
#include "mmu.hpp"
#include "util.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace VTxx {

static volatile uint8_t ppu_regs[256] = {0};
static uint8_t ppu_regs_shadow[256] = {0};
static mutex regs_mutex;

static volatile uint8_t vram[8192] = {0};
static volatile uint8_t spram[2048] = {0};

// Graphics layers
// These use a *very* unusual format to match - at least as close as possible -
// how the VT168 works. It consists of two 16-bit words, the MSW for palette
// bank 1 and the LSW for palette bank 0. Each word is in TRGB1555 format,
// where the MSb is 1 for transparent and 0 for solid
static uint32_t *layers[4];
static int layer_width, layer_height;

// Output buffer in ARGB8888 format
static uint32_t *obuf;
static int out_width, out_height;

static thread ppu_thread;

enum class ColourMode { IDX_4, IDX_16, IDX_64, IDX_256, ARGB1555 };

// Our custom (slow) blitting function
static void vt_blit(int src_width, int src_height, uint8_t *src, int dst_width,
                    int dst_height, int dst_stride, int dst_x, int dst_y,
                    uint32_t *dst, ColourMode fmt,
                    volatile uint8_t *pal0 = nullptr,
                    volatile uint8_t *pal1 = nullptr) {
  uint8_t *srcptr = src;
  int src_bit = 0;
  for (int sy = 0; sy < src_height; sy++) {
    int dy = dst_y + sy;
    for (int sx = 0; sx < src_width; sx++) {
      int dx = dst_x + sx;
      uint16_t argb0, argb1;
      if (fmt == ColourMode::ARGB1555) {
        argb0 = (*(srcptr + 1) << 8UL) | (*srcptr);
        argb1 = argb0;
        srcptr += 2;
      } else {
        uint8_t raw = 0;
        if (fmt == ColourMode::IDX_4) {
          raw = ((*srcptr) >> src_bit) & 0x03;
          src_bit += 2;
          if (src_bit >= 8) {
            src_bit = 0;
            srcptr++;
          }
        } else if (fmt == ColourMode::IDX_16) {
          raw = ((*srcptr) >> src_bit) & 0x0F;
          src_bit += 4;
          if (src_bit >= 8) {
            src_bit = 0;
            srcptr++;
          }
        } else if (fmt == ColourMode::IDX_64) {
          switch (src_bit) {
          case 0:
            raw = (*srcptr) & 0x3F;
            src_bit = 6;
            break;
          case 2:
            raw = ((*srcptr) >> 2) & 0x3F;
            src_bit = 0;
            srcptr++;
            break;
          case 4:
            raw = (((*srcptr) & 0xF0) >> 4) | ((*(srcptr + 1) & 0x03) << 4);
            src_bit = 2;
            srcptr++;
            break;
          case 6:
            raw = (((*srcptr) & 0xC0) >> 6) | ((*(srcptr + 1) & 0x0F) << 2);
            src_bit = 4;
            srcptr++;
            break;
          default:
            assert(false);
          }
        } else if (fmt == ColourMode::IDX_256) {
          raw = *srcptr;
          srcptr++;
        } else {
          assert(false);
        }
        if (raw == 0) {
          argb0 = 0x8000; // idx 0 is always transparent
          argb1 = 0x8000; // idx 0 is always transparent
        } else {
          if (pal0 != nullptr)
            argb0 = (pal0[2 * raw + 1] << 8) | pal0[2 * raw];
          if (pal1 != nullptr)
            argb1 = (pal1[2 * raw + 1] << 8) | pal1[2 * raw];
        }
      }
      if ((dx >= 0) && (dx < dst_width) && (dy >= 0) && (dy < dst_height)) {
        if (!(argb0 & 0x8000)) {
          dst[dy * dst_stride + dx] =
              (dst[dy * dst_stride + dx] & 0xFFFF0000) | argb0;
        }
        if (!(argb1 & 0x8000)) {
          dst[dy * dst_stride + dx] =
              (dst[dy * dst_stride + dx] & 0x0000FFFF) | (argb1 << 16UL);
        }
      }
    }
  }
};

const int reg_sp_seg_lsb = 0x1A;
const int reg_sp_seg_msb = 0x1B;
const int reg_sp_ctrl = 0x18;

// Get character data from ROM for an item
static void get_char_data(uint16_t seg, uint16_t vector, int w, int h,
                          ColourMode fmt, bool bmp, uint8_t *buf) {
  int spacing = 0;
  if (bmp || fmt == ColourMode::ARGB1555) {
    spacing = 16 * 16;
  } else {
    spacing = w * h;
  }
  int bpp = 0;
  switch (fmt) {
  case ColourMode::ARGB1555:
    bpp = 16;
    break;
  case ColourMode::IDX_256:
    bpp = 8;
    break;
  case ColourMode::IDX_64:
    bpp = 6;
    break;
  case ColourMode::IDX_16:
    bpp = 4;
    break;
  case ColourMode::IDX_4:
    bpp = 2;
    break;
  }
  if (bpp == 16)
    spacing *= 8;
  else
    spacing *= bpp;
  spacing /= 8;
  uint32_t pa = (seg << 13UL) + vector * spacing;
  // cout << "pa = 0x" << hex << pa << endl;
  int len = (w * h * bpp) / 8;
  for (int i = 0; i < len; i++)
    buf[i] = read_mem_physical(pa + i);
}

static void render_sprites() {
  // TODO: lots of rendering fixes, e.g. multi palette blending, sprite per line
  // limit, "dig"
  bool sp_en = get_bit(ppu_regs_shadow[reg_sp_ctrl], 2);
  if (!sp_en)
    return;
  bool spalsel = get_bit(ppu_regs_shadow[reg_sp_ctrl], 3);
  int sp_size = ppu_regs_shadow[reg_sp_ctrl] & 0x03;
  int sp_width = (sp_size == 2 || sp_size == 3) ? 16 : 8;
  int sp_height = (sp_size == 1 || sp_size == 3) ? 16 : 8;
  uint16_t sp_seg = (ppu_regs_shadow[reg_sp_seg_msb] & 0x0F) << 8 |
                    ppu_regs_shadow[reg_sp_seg_lsb];

  uint8_t tempbuf[16 * 16];
  for (int idx = 239; idx >= 0; idx--) {
    volatile uint8_t *spdata = spram + 8 * idx;
    uint16_t vector = ((spdata[1] & 0x0F) << 8UL) | spdata[0];
    if (vector == 0)
      continue;
    int layer = (spdata[3] >> 3) & 0x03;
    int palette = (spdata[1] >> 4) & 0x0F;
    bool psel = get_bit(spdata[5], 1);
    int x = unsigned(spdata[2]);
    if (get_bit(spdata[3], 0))
      x = x - 256;
    int y = unsigned(spdata[4]);
    if (get_bit(spdata[5], 0))
      y = y - 256;
    get_char_data(sp_seg, vector, sp_width, sp_height, ColourMode::IDX_16,
                  false, tempbuf);
    volatile uint8_t *pal0 = nullptr, *pal1 = nullptr;
    if (spalsel || !psel)
      pal0 = (vram + 0x1E00 + 32 * palette);
    if (spalsel || psel)
      pal1 = (vram + 0x1C00 + 32 * palette);
    vt_blit(sp_width, sp_height, tempbuf, layer_width, layer_height,
            layer_width, x, y, layers[layer], ColourMode::IDX_16, pal0, pal1);
  }
}

const int reg_bkg_x[2] = {0x10, 0x14};
const int reg_bkg_y[2] = {0x11, 0x15};
const int reg_bkg_ctrl1[2] = {0x12, 0x16};

// const int reg_bkg_linescroll = 0x20;
const int reg_bkg_ctrl2[2] = {0x13, 0x17};

const int reg_bkg_pal_sel = 0x0F;

const int reg_bkg_seg_lsb[2] = {0x1C, 0x1E};
const int reg_bkg_seg_msb[2] = {0x1D, 0x1F};

enum BkgScrollMode {
  SCROLL_FIX = 0,
  SCROLL_H = 1,
  SCROLL_V = 2,
  SCROLL_4P = 3
};

// Return the address of a tile given index, tile size and scroll mode
// and whether or not a tile actually exists
// This needs checking as the datasheet is fairly poor for this
static pair<uint16_t, bool> get_tile_addr(int tx, int ty, bool y8, bool x8,
                                          int size, bool bmp, int layer,
                                          BkgScrollMode scrl) {
  if (size == 8) {
    uint16_t base = 0;
    uint16_t offset = ((tx % 32) + 32 * (ty % 32)) * 2;
    bool mapped = false;
    switch (scrl) {
    case SCROLL_FIX:
      base = (y8 == 0 && x8 == 0) ? 0x000 : 0x800;
      mapped = (tx < 32 && ty < 32);
      break;
    case SCROLL_H:
      base = ((tx > 32) != x8) ? 0x800 : 0x000;
      mapped = ty < 32;
      break;
    case SCROLL_V:
      base = ((ty > 32) != y8) ? 0x800 : 0x000;
      mapped = tx < 32;
      break;
    case SCROLL_4P:
      assert(false);
      break;
    }
    return make_pair(base + offset, mapped);
  } else if (size == 16) {
    uint16_t base = 0;
    uint16_t offset = ((tx % 16) + 16 * (ty % 16)) * 2;
    bool mapped = false;
    switch (scrl) {
    case SCROLL_FIX:
      base = (layer << 11) | (y8 << 10) | (x8 << 9);
      mapped = (tx < 16 && ty < 16);
      break;
    case SCROLL_H:
      base = ((tx > 16) != x8) ? 0x200 : 0x000;
      base |= (layer << 11);
      mapped = ty < 16;
      break;
    case SCROLL_V:
      base = ((ty > 16) != y8) ? 0x200 : 0x000;
      base |= (layer << 11);
      mapped = tx < 16;
      break;
    case SCROLL_4P:
      base = ((tx > 16) != x8) ? 0x200 : 0x000;
      base |= ((ty > 16) != y8) ? 0x400 : 0x000;
      base |= (layer << 11);
      mapped = true;
      break;
    }
    return make_pair(base + offset, mapped);
  } else if (bmp) {
    assert(layer == 0);
    uint16_t base = 0;
    uint16_t offset = (ty % 256) * 2;
    bool mapped = false;
    switch (scrl) {
    case SCROLL_FIX:
      base = (layer << 11) | (y8 << 10) | (x8 << 9);
      mapped = (tx < 1 && ty < 256);
      break;
    case SCROLL_H:
      base = ((tx > 1) != x8) ? 0x200 : 0x000;
      mapped = ty < 256;
      break;
    case SCROLL_V:
      base = ((ty > 256) != y8) ? 0x200 : 0x000;
      mapped = tx < 1;
      break;
    case SCROLL_4P:
      base = ((tx > 1) != x8) ? 0x200 : 0x000;
      base |= ((ty > 256) != y8) ? 0x400 : 0x000;
      mapped = true;
      break;
    }
    return make_pair(base + offset, mapped);
  } else {
    assert(false);
  }
}

// Render the given background layer (idx = [0, 1])
static void render_background(int idx) {
  bool en = get_bit(ppu_regs_shadow[reg_bkg_ctrl2[idx]], 7);
  if (!en)
    return;
  bool bkx_pal = get_bit(ppu_regs_shadow[reg_bkg_ctrl2[idx]], 6);
  ColourMode fmt;
  bool hclr =
      (idx == 0) ? get_bit(ppu_regs_shadow[reg_bkg_ctrl1[idx]], 4) : false;
  int bkx_clr = (ppu_regs_shadow[reg_bkg_ctrl2[idx]] >> 2) & 0x03;
  if (hclr) {
    fmt = ColourMode::ARGB1555;
  } else {
    switch (bkx_clr) { // check, datasheet doesn't specify
    case 0:
      fmt = ColourMode::IDX_4;
      break;
    case 1:
      fmt = ColourMode::IDX_16;
      break;
    case 2:
      fmt = ColourMode::IDX_64;
      break;
    case 3:
      fmt = ColourMode::IDX_256;
      break;
    }
  }
  bool x8 = get_bit(ppu_regs_shadow[reg_bkg_ctrl1[idx]], 0);
  bool y8 = get_bit(ppu_regs_shadow[reg_bkg_ctrl1[idx]], 1);
  bool render_pal0 = get_bit(ppu_regs_shadow[reg_bkg_pal_sel], 0 + 2 * idx);
  bool render_pal1 = get_bit(ppu_regs_shadow[reg_bkg_pal_sel], 1 + 2 * idx);

  int xoff = unsigned(ppu_regs_shadow[reg_bkg_x[idx]]);
  if (x8)
    xoff = xoff - 256;
  int yoff = unsigned(ppu_regs_shadow[reg_bkg_y[idx]]);
  if (y8)
    yoff = yoff - 256;
  // cout << "BKG" << idx << " loc " << xoff << " " << yoff << endl;

  bool bmp =
      (idx == 0) ? get_bit(ppu_regs_shadow[reg_bkg_ctrl2[idx]], 1) : false;
  BkgScrollMode scrl_mode =
      (BkgScrollMode)((ppu_regs_shadow[reg_bkg_ctrl1[idx]] >> 2) & 0x03);
  // bool line_scroll = get_bit(ppu_regs_shadow[reg_bkg_linescroll], 4 + idx);
  // int line_scroll_bank = ppu_regs_shadow[reg_bkg_linescroll] & 0x0F;
  bool bkx_size = get_bit(ppu_regs_shadow[reg_bkg_ctrl2[idx]], 0);
  int tile_height = bmp ? 1 : (bkx_size ? 16 : 8);
  int tile_width = bmp ? 256 : (bkx_size ? 16 : 8);
  int y0 =
      ((scrl_mode == SCROLL_V || scrl_mode == SCROLL_4P) && !bmp) ? -256 : 0;
  int x0 =
      ((scrl_mode == SCROLL_H || scrl_mode == SCROLL_4P) && !bmp) ? -256 : 0;
  int yn = 256;
  int xn = 256;
  uint8_t char_buf[512];

  uint16_t seg = ((ppu_regs_shadow[reg_bkg_seg_msb[idx]] & 0x0F) << 8UL) |
                 ppu_regs_shadow[reg_bkg_seg_lsb[idx]];

  for (int y = y0; y < yn; y += tile_height) {
    for (int x = x0; x < xn; x += tile_width) {
      int lx = x + xoff;
      int ly = y + yoff;
      int tx = (x - x0) / tile_width;
      int ty = (y - y0) / tile_height;
      // Various inefficiencies here, should not draw unless at least part
      // visible
      auto tile_d =
          get_tile_addr(tx, ty, y8, x8, tile_width, bmp, idx, scrl_mode);
      uint16_t tile_addr = tile_d.first;
      bool tile_mapped = tile_d.second;
      if (!tile_mapped)
        continue;
      uint16_t cell = (vram[tile_addr + 1] << 8UL) | vram[tile_addr];
      uint16_t vector = cell & 0xFFF;
      uint8_t cell_pal_bk = (cell >> 12) & 0x0F;
      if (vector == 0) // transparent
        continue;
      uint8_t pal_bank = 0;
      uint8_t depth = 0;
      if (bkx_pal) {
        depth = (ppu_regs_shadow[reg_bkg_ctrl2[idx]] >> 4) & 0x03;
        pal_bank = (fmt == ColourMode::IDX_16)
                       ? cell_pal_bk
                       : ((fmt == ColourMode::IDX_64) ? (cell_pal_bk >> 2) : 0);
      } else {
        depth = cell_pal_bk & 0x03;
        pal_bank = (fmt == ColourMode::IDX_16)
                       ? (((ppu_regs_shadow[reg_bkg_ctrl2[idx]] >> 4) & 0x03) |
                          (cell_pal_bk >> 2))
                       : ((fmt == ColourMode::IDX_64) ? (cell_pal_bk >> 2) : 0);
      }

      get_char_data(seg, vector, tile_width, tile_height, fmt, bmp, char_buf);
      // TODO: line scrolling
      uint16_t palette_offset =
          (fmt == ColourMode::IDX_16)
              ? (pal_bank * 32)
              : (fmt == ColourMode::IDX_64 ? (pal_bank * 128) : 0);
      volatile uint8_t *pal0 = nullptr, *pal1 = nullptr;
      if (render_pal0)
        pal0 = (vram + 0x1E00 + palette_offset);
      if (render_pal1)
        pal1 = (vram + 0x1C00 + palette_offset);
      vt_blit(tile_width, tile_height, char_buf, layer_width, layer_height,
              layer_width, lx, ly, layers[depth & 0x03], fmt, pal0, pal1);
    }
  }
}

const int reg_pal_sel = 0x0E;
// const int reg_v_scale = 0x19;
static inline uint16_t blend_argb1555(uint16_t a, uint16_t b) {
  if (a & 0x8000)
    return b;
  if (b & 0x8000)
    return a;
  uint16_t x = 0;
  x |= (((a & 0x1F) + (b & 0x1F)) / 2) & 0x1F;
  x |= (((((a >> 5) & 0x1F) + ((b >> 5) & 0x1)) / 2) & 0x1F) << 5;
  x |= (((((a >> 11) & 0x1F) + ((b >> 11) & 0x1F)) / 2) & 0x1F) << 10;
  return x;
}

static inline uint8_t c5_to_8(uint8_t x) {
  bool lsb = get_bit(x, 0);
  return (x << 3) | (lsb ? 0x7 : 0x0);
}

static inline uint32_t argb1555_to_rgb8888(uint16_t x) {
  uint8_t r = x & 0x1F;
  uint8_t g = (x >> 5) & 0x1F;
  uint8_t b = (x >> 10) & 0x1F;
  bool a = get_bit(x, 15);
  if (a)
    return 0xFF000000;
  uint32_t y = 0;
  y |= 0xFF000000;
  y |= c5_to_8(r) << 16UL;
  y |= c5_to_8(g) << 8UL;
  y |= c5_to_8(b);
  return y;
}

// Merge the layers and convert to ARGB8888. Set lcd to true to merge for LCD
// rather than TV output
static void merge_layers(bool lcd = false) {
  bool output_pal0 = get_bit(ppu_regs_shadow[reg_pal_sel], lcd ? 0 : 1);
  bool output_pal1 = get_bit(ppu_regs_shadow[reg_pal_sel], lcd ? 2 : 3);
  bool blend_pal = get_bit(ppu_regs_shadow[reg_pal_sel], lcd ? 5 : 4);
  for (int y = 0; y < out_height; y++) {
    for (int x = 0; x < out_width; x++) {
      uint16_t pal0 = 0x8000, pal1 = 0x8000;
      for (int l = 3; l >= 0; l--) {
        uint32_t raw = layers[l][y * layer_width + x];
        if (!(raw & 0x8000)) {
          pal0 = raw & 0xFFFF;
        }
        if (!(raw & 0x80000000)) {
          pal1 = (raw >> 16) & 0xFFFF;
        }
      }
      uint16_t res = 0x8000;
      if (blend_pal && output_pal0 && output_pal1) {
        res = blend_argb1555(pal0, pal1);
      }
      if (output_pal0 && !(pal0 & 0x8000)) {
        res = pal0;
      }
      if (output_pal1 && !(pal1 & 0x8000)) {
        res = pal1;
      }
      obuf[y * out_width + x] = argb1555_to_rgb8888(res);
    }
  }
}

static void clear_layer(uint32_t *ptr, int w, int h) {
  fill(ptr, ptr + (w * h), 0x80008000); // fill with transparent
}

static void clear_layers() {
  for (int i = 0; i < 4; i++)
    clear_layer(layers[i], layer_width, layer_height);
}

static atomic<bool> render_done(false);

// Render and merge all layers
static void do_render() {
  render_done = false;
  // Make a shadow copy of the PPU registers for thread safety - the CPU
  // shouldn't really be accessing them though anyway
  {
    lock_guard<std::mutex> guard(regs_mutex);
    copy(ppu_regs, ppu_regs + 256, ppu_regs_shadow);
  }
  // Fill all layers with transparent
  clear_layers();
  // Render background layers (lower index has priority)
  for (int i = 1; i >= 0; i--)
    render_background(i);
  // Render sprites
  render_sprites();
  // Merge to output
  merge_layers(false);
  render_done = true;
};

static atomic<bool> kill_renderer(false);
static bool render_ready = false;
static condition_variable do_render_cv;
static mutex do_render_m;

void ppu_render_thread() {
  while (!kill_renderer) {
    std::unique_lock<std::mutex> lk(do_render_m);
    do_render_cv.wait(lk, [] { return render_ready; });
    render_ready = false;
    // Signal might be to die rather than render again
    if (!kill_renderer) {
      do_render();
    }
    lk.unlock();
  }
}
// Defaults to PAL
static uint32_t vblank_start = 0;
static uint32_t vblank_len = 22036;
static uint32_t v_total = 106392;

static uint32_t ticks = 0;
// Called once every CPU clock
void ppu_tick() {
  ticks += 1;
  if (ticks >= v_total) {
    ticks = 0;
    // TODO: signal vblank NMI
  } else if (ticks == vblank_len) {
    // Render begins at end of VBLANK
    {
      lock_guard<mutex> lk(do_render_m);
      render_ready = true;
    }
    do_render_cv.notify_one();
  }
}

bool ppu_is_render_done() { return render_done; }

bool ppu_is_vblank() { return (ticks >= vblank_start && ticks < vblank_len); }

uint32_t *get_render_buffer() { return obuf; }

void ppu_init() {
  layer_width = 256;
  layer_height = 256;

  for (int i = 0; i < 4; i++) {
    layers[i] = new uint32_t[layer_width * layer_height];
  }
  out_width = 256;
  out_height = 240;
  obuf = new uint32_t[out_width * out_height];
  ppu_thread = thread(ppu_render_thread);
}

void ppu_stop() {
  {
    lock_guard<mutex> lk(do_render_m);
    render_ready = true;
    kill_renderer = true;
  }
  do_render_cv.notify_one();
}

const uint8_t reg_ppu_stat = 0x01;

const uint8_t reg_spram_addr_msb = 0x02;
const uint8_t reg_spram_addr_lsb = 0x03;
const uint8_t reg_spram_data = 0x04;

const uint8_t reg_vram_addr_msb = 0x06;
const uint8_t reg_vram_addr_lsb = 0x05;
const uint8_t reg_vram_data = 0x07;

uint8_t ppu_read(uint8_t address) {
  switch (address) {
  case reg_spram_data: {
    uint16_t spram_addr = ((ppu_regs[reg_spram_addr_msb] & 0x07) << 8) |
                          ppu_regs[reg_spram_addr_lsb];
    return spram[spram_addr]; // TODO: are SPRAM and VRAM reads swapped?
  }
  case reg_vram_data: {
    uint16_t vram_addr = ((ppu_regs[reg_vram_addr_msb] & 0x1F) << 8) |
                         ppu_regs[reg_vram_addr_lsb];
    // cout << "vram rd " << hex << vram_addr;
    return vram[vram_addr]; // TODO: are SPRAM and VRAM reads
                            // swapped?
  }
  case reg_ppu_stat: {
    // Clear VBLANK IRQ here
    return (ppu_is_vblank() << 7);
  }
  default:
    return ppu_regs[address];
  }
}

void ppu_write(uint8_t address, uint8_t data) {
  switch (address) {
  case reg_spram_data: {
    uint16_t spram_addr = ((ppu_regs[reg_spram_addr_msb] & 0x07) << 8) |
                          ppu_regs[reg_spram_addr_lsb];
    spram[spram_addr++] = data;
    if ((spram_addr & 0x07) >= 6) { // TODO: check, is this just for DMA?
      spram_addr &= ~0x07;
      spram_addr += 8;
    }
    ppu_regs[reg_spram_addr_msb] = (spram_addr >> 8) & 0x07;
    ppu_regs[reg_spram_addr_lsb] = spram_addr & 0xFF;
    break;
  }
  case reg_vram_data: {
    uint16_t vram_addr = ((ppu_regs[reg_vram_addr_msb] & 0x1F) << 8) |
                         ppu_regs[reg_vram_addr_lsb];
    // cout << "vram wr " << hex << vram_addr << " d=" << int(data) << endl;
    vram[vram_addr++] = data;
    ppu_regs[reg_vram_addr_msb] = (vram_addr >> 8) & 0x1F;
    ppu_regs[reg_vram_addr_lsb] = vram_addr & 0xFF;
    break;
  }
  default:

    ppu_regs[address] = data;
    if (address == reg_vram_addr_msb || address == reg_vram_addr_lsb) {
      /*cout << "vram set addr 0x" << hex
           << ((ppu_regs[reg_vram_addr_msb] << 8) | ppu_regs[reg_vram_addr_lsb])
           << endl;*/
    }
    break;
  }
}

bool ppu_nmi_enabled() { return get_bit(ppu_regs[0], 0); }

} // namespace VTxx
