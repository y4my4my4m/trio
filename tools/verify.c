#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "snes/snes.h"
#include "script.h"
#include "game/recomp_rt.h"

//Lockstep verifier: instance A runs the interpreter only, instance B runs
//with the recompiled-code hook. Both get the same input; after every frame
//the whole Snes struct is compared except host pointers.
//
//usage: trio-verify [rom.sfc] [--frames N] [--input FILE] [--state FILE] [--srm FILE|none]
//                 [--no-overrides]

typedef struct { size_t off, size; const char *name; } Field;

#define F(member, label) { offsetof(Snes, member), sizeof(((Snes *)0)->member), label }

static const Field fields[] = {
  F(cpu.a, "cpu.a"), F(cpu.x, "cpu.x"), F(cpu.y, "cpu.y"), F(cpu.s, "cpu.s"), F(cpu.d, "cpu.d"),
  F(cpu.db, "cpu.db"), F(cpu.pb, "cpu.pb"), F(cpu.pc, "cpu.pc"), F(cpu.p, "cpu.p"),
  F(cpu.e, "cpu.e"), F(cpu.waiting, "cpu.waiting"), F(cpu.stopped, "cpu.stopped"),
  F(cpu.nmi_pending, "cpu.nmi_pending"), F(cpu.irq_line, "cpu.irq_line"),
  F(ppu.vram, "ppu.vram"), F(ppu.cgram, "ppu.cgram"), F(ppu.oam, "ppu.oam"),
  F(ppu.regs_opaque, "ppu.regs_opaque"), F(ppu.hcount_latch, "ppu.hcount_latch"),
  F(ppu.vcount_latch, "ppu.vcount_latch"), F(ppu.counter_latched, "ppu.counter_latched"),
  F(ppu.overscan, "ppu.overscan"), F(ppu.forced_blank, "ppu.forced_blank"),
  F(ppu.brightness, "ppu.brightness"), F(ppu.out_w, "ppu.out_w"),
  F(ppu.frame, "ppu.frame (framebuffer)"),
  F(apu.ram, "apu.ram"), F(apu.a, "apu.a"), F(apu.x, "apu.x"), F(apu.y, "apu.y"),
  F(apu.sp, "apu.sp"), F(apu.psw, "apu.psw"), F(apu.pc, "apu.pc"), F(apu.stopped, "apu.stopped"),
  F(apu.control, "apu.control"), F(apu.dsp_addr, "apu.dsp_addr"), F(apu.port_in, "apu.port_in"),
  F(apu.port_out, "apu.port_out"), F(apu.timer_target, "apu.timer_target"),
  F(apu.timer_stage, "apu.timer_stage"), F(apu.timer_counter, "apu.timer_counter"),
  F(apu.timer_enabled, "apu.timer_enabled"), F(apu.timer_div, "apu.timer_div"),
  F(apu.cycles, "apu.cycles"), F(apu.sample_div, "apu.sample_div"),
  F(apu.dsp_opaque, "apu.dsp_opaque"), F(apu.ring, "apu.ring (audio samples)"),
  F(apu.ring_w, "apu.ring_w"), F(apu.ring_r, "apu.ring_r"),
  F(wram, "wram"), F(sram, "sram"), F(sram_dirty, "sram_dirty"),
  F(cycles, "cycles"), F(line_start, "line_start"), F(vpos, "vpos"),
  F(frame_count, "frame_count"), F(frame_done, "frame_done"), F(open_bus, "open_bus"),
  F(wram_addr, "wram_addr"), F(nmitimen, "nmitimen"), F(wrio, "wrio"), F(memsel, "memsel"),
  F(wrmpya, "wrmpya"), F(wrmpyb, "wrmpyb"), F(wrdiv, "wrdiv"), F(rddiv, "rddiv"),
  F(rdmpy, "rdmpy"), F(htime, "htime"), F(vtime, "vtime"), F(nmi_flag, "nmi_flag"),
  F(irq_flag, "irq_flag"), F(in_vblank, "in_vblank"), F(hdma_done_line, "hdma_done_line"),
  F(refresh_done_line, "refresh_done_line"), F(hirq_done_line, "hirq_done_line"),
  F(in_events, "in_events"), F(hdmaen, "hdmaen"), F(dma, "dma"), F(pad, "pad"),
  F(joy_auto, "joy_auto"), F(joy_shift, "joy_shift"), F(joy_strobe, "joy_strobe"),
  F(autojoy_busy_lines, "autojoy_busy_lines"),
};

//Host pointers, excluded from the comparison.
static const Field skip[] = {
  F(cpu.ctx, "cpu.ctx"), F(cpu.read, "cpu.read"), F(cpu.write, "cpu.write"),
  F(cpu.idle, "cpu.idle"), F(rom, "rom"), F(coverage, "coverage"), F(exec_hook, "exec_hook"),
};

#undef F

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)n);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
  fclose(f);
  if (size) *size = (size_t)n;
  return buf;
}

static bool skipped(size_t off) {
  for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
    if (off >= skip[i].off && off < skip[i].off + skip[i].size) return true;
  return false;
}

//First differing byte offset outside host pointers, or SIZE_MAX.
static size_t first_diff(const Snes *a, const Snes *b) {
  const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
  size_t off = 0, n = sizeof(Snes);
  while (off < n) {
    //Next skipped range at or after off bounds the memcmp chunk.
    size_t end = n;
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
      if (skip[i].off >= off && skip[i].off < end) end = skip[i].off;
    if (end > off && memcmp(pa + off, pb + off, end - off)) {
      for (size_t i = off; i < end; i++)
        if (pa[i] != pb[i]) return i;
    }
    off = end;
    while (off < n && skipped(off)) off++;
  }
  return SIZE_MAX;
}

static void print_cpu(const char *tag, const Snes *s) {
  const Cpu *c = &s->cpu;
  printf("  %s: PC=%02X:%04X A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X P=%02X E=%d "
         "nmi=%d cycles=%llu vpos=%u\n",
         tag, c->pb, c->pc, c->a, c->x, c->y, c->s, c->d, c->db, c->p, c->e, c->nmi_pending,
         (unsigned long long)s->cycles, s->vpos);
}

static void report(int frame, const Snes *a, const Snes *b, size_t off) {
  const Field *f = NULL;
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    if (off >= fields[i].off && off < fields[i].off + fields[i].size) { f = &fields[i]; break; }
  const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
  printf("DIVERGENCE after frame %d (overrides %s)\n", frame, rt_overrides_on ? "on" : "off");
  if (f) {
    size_t rel = off - f->off;
    printf("  field %s, byte offset $%zX", f->name, rel);
    if (f->off == offsetof(Snes, wram)) printf(" (address $%02zX:%04zX)", 0x7E + (rel >> 16), rel & 0xFFFF);
    if (f->off == offsetof(Snes, sram)) printf(" (address $70:%04zX)", rel);
    if (f->off == offsetof(Snes, ppu.frame))
      printf(" (column %zu row %zu)", (rel / 4) % PPU_MAXW, (rel / 4) / PPU_MAXW);
    printf(": interp=%02X recomp=%02X\n", pa[off], pb[off]);
  } else {
    printf("  struct offset $%zX (padding or unlisted field): interp=%02X recomp=%02X\n",
           off, pa[off], pb[off]);
  }
  print_cpu("interp", a);
  print_cpu("recomp", b);
}

static void usage(void) {
  fprintf(stderr,
    "usage: trio-verify [rom.sfc] [options]\n"
    "  --frames N       frames to run (default 2000)\n"
    "  --input FILE     input script (pad commands and quit; shot/save ignored)\n"
    "  --state FILE     load save state into both instances\n"
    "  --srm FILE|none  SRAM image (default: ROM path with .srm, if present)\n"
    "  --no-overrides   recompiled instance runs without overrides (tools/overrides.txt)\n");
}

int main(int argc, char **argv) {
  const char *rom_path = "rom/g3.sfc", *input = NULL, *state = NULL, *srm_path = NULL;
  int frames = 2000;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (!strcmp(a, "--frames") && more) frames = atoi(argv[++i]);
    else if (!strcmp(a, "--input") && more) input = argv[++i];
    else if (!strcmp(a, "--state") && more) state = argv[++i];
    else if (!strcmp(a, "--srm") && more) srm_path = argv[++i];
    else if (!strcmp(a, "--no-overrides")) rt_overrides_on = false;
    else if (a[0] == '-') { usage(); return 2; }
    else rom_path = a;
  }

  size_t raw_size;
  uint8_t *raw = read_file(rom_path, &raw_size);
  uint32_t rom_size = 0;
  uint8_t *rom = raw ? snes_rom_image(raw, raw_size, &rom_size) : NULL;
  if (!rom) { fprintf(stderr, "cannot read ROM %s\n", rom_path); return 2; }

  char srm_default[1024];
  if (!srm_path) {
    snprintf(srm_default, sizeof(srm_default), "%s", rom_path);
    char *dot = strrchr(srm_default, '.');
    if (dot) *dot = 0;
    strncat(srm_default, ".srm", sizeof(srm_default) - strlen(srm_default) - 1);
    srm_path = srm_default;
  }
  size_t srm_n = 0;
  uint8_t *srm = strcmp(srm_path, "none") ? read_file(srm_path, &srm_n) : NULL;

  Snes *sa = malloc(sizeof(Snes)), *sb = malloc(sizeof(Snes));
  if (!sa || !sb) return 2;
  Snes *inst[2] = {sa, sb};
  for (int k = 0; k < 2; k++) {
    snes_init(inst[k], rom, (uint32_t)rom_size);
    if (srm && srm_n == sizeof(inst[k]->sram)) memcpy(inst[k]->sram, srm, srm_n);
    snes_reset(inst[k]);
  }
  rt_install(sb, true);
  if (state) {
    size_t n;
    uint8_t *buf = read_file(state, &n);
    if (!buf || n != snes_state_size() || !snes_state_load(sa, buf) || !snes_state_load(sb, buf)) {
      fprintf(stderr, "bad state file %s\n", state);
      return 2;
    }
    free(buf);
  }

  int script_n = 0, script_i = 0;
  ScriptCmd *script = NULL;
  if (input && !(script = script_load(input, &script_n))) return 2;
  uint16_t script_pad = 0;
  int hold_until = -1;

  size_t d = first_diff(sa, sb);
  if (d != SIZE_MAX) { report(0, sa, sb, d); return 1; }

  static int16_t samples[2][APU_RING * 2];
  double t_a = 0, t_b = 0;
  int frame = 0;
  bool quit = false;
  for (; frame < frames && !quit; frame++) {
    while (script_i < script_n && script[script_i].frame <= frame) {
      ScriptCmd *c = &script[script_i++];
      if (c->kind == CMD_PAD) { script_pad = c->buttons; hold_until = frame + c->hold; }
      else if (c->kind == CMD_QUIT) quit = true;
    }
    if (quit) break;
    if (frame >= hold_until) script_pad = 0;
    sa->pad[0] = sb->pad[0] = script_pad;

    clock_t t0 = clock();
    snes_run_frame(sa);
    clock_t t1 = clock();
    snes_run_frame(sb);
    clock_t t2 = clock();
    t_a += (double)(t1 - t0) / CLOCKS_PER_SEC;
    t_b += (double)(t2 - t1) / CLOCKS_PER_SEC;
    int na = apu_pop_samples(&sa->apu, samples[0], APU_RING);
    int nb = apu_pop_samples(&sb->apu, samples[1], APU_RING);
    if (na != nb || memcmp(samples[0], samples[1], (size_t)na * 4)) {
      printf("DIVERGENCE after frame %d: audio output differs\n", frame + 1);
      print_cpu("interp", sa);
      print_cpu("recomp", sb);
      return 1;
    }
    d = first_diff(sa, sb);
    if (d != SIZE_MAX) { report(frame + 1, sa, sb, d); return 1; }
  }

  uint64_t tot = rt_insn_count + rt_interp_count;
  printf("OK: %d frames identical (overrides %s)\n", frame, rt_overrides_on ? "on" : "off");
  printf("recompiled instructions: %llu / %llu (%.2f%%), interpreter steps: %llu\n",
         (unsigned long long)rt_insn_count, (unsigned long long)tot,
         tot ? 100.0 * (double)rt_insn_count / (double)tot : 0.0,
         (unsigned long long)rt_interp_count);
  printf("time: interp %.3f s, recomp %.3f s\n", t_a, t_b);
  free(script);
  free(srm);
  free(sa);
  free(sb);
  free(rom);
  return 0;
}
