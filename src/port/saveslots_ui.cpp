#include "saveslots_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "imgui.h"
#include "saveslots.h"

namespace {

struct Thumb {
  SDL_Texture *tex;
  uint32_t gen;            //SlotInfo.gen the texture was built from; 0 = never
  int w, h;
};

Thumb g_thumb[SS_COUNT];
int g_sel = -1;            //highlighted slot
int g_cfg_slot = -1;       //cfg->save_slot seen last frame
int g_req_op, g_req_slot;
int g_confirm_op, g_confirm_slot;  //modal question pending
bool g_confirm_open;       //OpenPopup due this frame

const Thumb &thumb(int slot, SDL_Renderer *ren) {
  Thumb &t = g_thumb[slot];
  const SlotInfo *in = ss_info(slot);
  if (t.gen == in->gen) return t;
  if (t.tex) SDL_DestroyTexture(t.tex);
  t = Thumb{nullptr, in->gen, 0, 0};
  uint8_t *rgb;
  int w, h;
  if (in->exists && ss_thumb(slot, &rgb, &w, &h)) {
    t.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t.tex) {
      SDL_UpdateTexture(t.tex, nullptr, rgb, w * 3);
      SDL_SetTextureScaleMode(t.tex, SDL_ScaleModeLinear);
      t.w = w;
      t.h = h;
    }
    free(rgb);
  }
  return t;
}

void fmt_time(int64_t ts, char *buf, size_t n) {
  time_t t = (time_t)ts;
  struct tm *tm = localtime(&t);
  if (!tm || !strftime(buf, n, "%Y-%m-%d %H:%M", tm)) snprintf(buf, n, "-");
}

void fmt_play(uint64_t frames, char *buf, size_t n) {
  uint64_t s = (uint64_t)(frames / 60.0988);
  snprintf(buf, n, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
           (unsigned long long)(s % 60));
}

void request(int op, int slot) {
  g_req_op = op;
  g_req_slot = slot;
}

//One grid cell: thumbnail box plus three text lines. Returns true on click.
bool cell(int slot, float w, float box_h, SDL_Renderer *ren, bool *dbl) {
  const SlotInfo *in = ss_info(slot);
  ImGuiStyle &st = ImGui::GetStyle();
  float line = ImGui::GetTextLineHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = box_h + 3 * line + st.ItemInnerSpacing.y * 2 + 6;
  ImGui::PushID(slot);
  bool click = ImGui::InvisibleButton("cell", ImVec2(w, h));
  *dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  bool hover = ImGui::IsItemHovered();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  bool sel = slot == g_sel;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(hover || sel ? ImGuiCol_FrameBgHovered
                                                                                  : ImGuiCol_FrameBg), 4);
  if (sel) dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_CheckMark), 4.0f, 0, 2.0f);
  ImVec2 b0(p.x + 3, p.y + 3), b1(p.x + w - 3, p.y + 3 + box_h);
  dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 255));
  const Thumb &t = thumb(slot, ren);
  if (t.tex) {
    //Fit keeping the display aspect of the box (box_h already includes pixel aspect).
    float bw = b1.x - b0.x, bh = b1.y - b0.y;
    float img_w = bw * t.w / 128.0f, img_h = bh;
    if (img_w > bw) { img_h = bh * bw / img_w; img_w = bw; }
    ImVec2 i0(b0.x + (bw - img_w) / 2, b0.y + (bh - img_h) / 2);
    ImU32 tint = in->compatible ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 255);
    dl->AddImage((ImTextureID)(intptr_t)t.tex, i0, ImVec2(i0.x + img_w, i0.y + img_h), ImVec2(0, 0),
                 ImVec2(1, 1), tint);
  } else {
    const char *msg = in->exists ? "no thumbnail" : "empty";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2((b0.x + b1.x - ts.x) / 2, (b0.y + b1.y - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                msg);
  }
  char name[16], l1[64], l2[64], l3[80];
  ss_name(slot, name, sizeof(name));
  ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  ImU32 bad = IM_COL32(240, 110, 90, 255);
  float y = b1.y + 3;
  if (in->exists) {
    char when[32];
    fmt_time(in->timestamp, when, sizeof(when));
    snprintf(l1, sizeof(l1), "%s  %s", name, when);
    if (in->compatible) {
      char play[24];
      fmt_play(in->frames, play, sizeof(play));
      snprintf(l2, sizeof(l2), "%s", in->location);
      snprintf(l3, sizeof(l3), "play %s", play);
    } else {
      snprintf(l2, sizeof(l2), "incompatible");
      snprintf(l3, sizeof(l3), "%s", in->why ? in->why : "");
    }
  } else {
    snprintf(l1, sizeof(l1), "%s", name);
    l2[0] = l3[0] = 0;
  }
  ImVec4 clip(p.x, p.y, p.x + w - 3, p.y + h);
  dl->AddText(nullptr, 0, ImVec2(p.x + 4, y), fg, l1, nullptr, 0, &clip);
  dl->AddText(nullptr, 0, ImVec2(p.x + 4, y + line), in->compatible || !in->exists ? dim : bad, l2, nullptr, 0, &clip);
  dl->AddText(nullptr, 0, ImVec2(p.x + 4, y + 2 * line), in->compatible || !in->exists ? dim : bad, l3, nullptr, 0,
              &clip);
  return click;
}

}  // namespace

extern "C" bool ssui_tab(Settings *cfg, SDL_Renderer *ren) {
  bool ch = false;
  if (cfg->save_slot != g_cfg_slot) g_sel = g_cfg_slot = cfg->save_slot;
  if (g_sel < 0 || g_sel >= SS_COUNT) g_sel = 0;

  //1..9, 0 select manual slots 1..10.
  if (!ImGui::GetIO().WantTextInput)
    for (int k = 0; k < 10; k++)
      if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + k), false)) g_sel = k ? k : 10;

  ImGuiStyle &st = ImGui::GetStyle();
  float line = ImGui::GetFrameHeightWithSpacing();
  float footer = 3 * line + ImGui::GetTextLineHeightWithSpacing() + st.ItemSpacing.y * 2;
  ImGui::BeginChild("slots", ImVec2(0, -footer), false);
  float avail = ImGui::GetContentRegionAvail().x;
  int cols = (int)((avail + st.ItemSpacing.x) / (150 + st.ItemSpacing.x));
  if (cols < 1) cols = 1;
  float w = (avail - (cols - 1) * st.ItemSpacing.x) / cols;
  float par = cfg->pixel_aspect ? 8.0f / 7.0f : 1.0f;
  float box_h = (w - 6) * 224.0f / (256.0f * par);
  for (int i = 0, col = 0; i < SS_COUNT; i++, col++) {
    if (i == SS_AUTO_FIRST) { ImGui::SeparatorText("Autosaves"); col = 0; }
    if (col % cols) ImGui::SameLine();
    bool dbl;
    if (cell(i, w, box_h, ren, &dbl)) g_sel = i;
    if (dbl && ss_info(i)->exists && ss_info(i)->compatible) request(SS_OP_LOAD, i);
  }
  ImGui::EndChild();

  if (g_sel <= SS_MANUAL && g_sel != cfg->save_slot) {
    cfg->save_slot = g_cfg_slot = g_sel;
    ch = true;
  }

  const SlotInfo *in = ss_info(g_sel);
  char name[16];
  ss_name(g_sel, name, sizeof(name));
  if (in->exists && in->compatible) {
    char play[24];
    fmt_play(in->frames, play, sizeof(play));
    ImGui::Text("%s: %s, play time %s", name, in->location, play);
  } else if (in->exists) {
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s: incompatible (%s)", name, in->why ? in->why : "?");
  } else {
    ImGui::TextDisabled("%s: empty", name);
  }

  bool is_auto = g_sel >= SS_AUTO_FIRST;
  ImGui::BeginDisabled(is_auto);
  if (ImGui::Button("Save", ImVec2(100, 0))) {
    if (in->exists) { g_confirm_op = SS_OP_SAVE; g_confirm_slot = g_sel; g_confirm_open = true; }
    else request(SS_OP_SAVE, g_sel);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!in->exists || !in->compatible);
  if (ImGui::Button("Load", ImVec2(100, 0))) request(SS_OP_LOAD, g_sel);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!in->exists);
  if (ImGui::Button("Delete", ImVec2(100, 0))) {
    g_confirm_op = SS_OP_DELETE;
    g_confirm_slot = g_sel;
    g_confirm_open = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("F5/F9 save/load slot, F6/F7 pick");

  ch |= ImGui::Checkbox("Autosave on stage entry", &cfg->autosave);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  ch |= ImGui::SliderInt("Timed autosave", &cfg->autosave_minutes, 0, 60,
                         cfg->autosave_minutes ? "every %d min" : "off");

  if (g_confirm_open) { ImGui::OpenPopup("Confirm##slots"); g_confirm_open = false; }
  if (ImGui::BeginPopupModal("Confirm##slots", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    char cname[16];
    ss_name(g_confirm_slot, cname, sizeof(cname));
    ImGui::Text(g_confirm_op == SS_OP_SAVE ? "Overwrite %s?" : "Delete %s?", cname);
    if (ImGui::Button("Yes", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
      request(g_confirm_op, g_confirm_slot);
      g_confirm_op = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("No", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      g_confirm_op = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  return ch;
}

extern "C" bool ssui_take_request(int *op, int *slot) {
  if (!g_req_op) return false;
  *op = g_req_op;
  *slot = g_req_slot;
  g_req_op = 0;
  return true;
}

extern "C" void ssui_shutdown(void) {
  for (Thumb &t : g_thumb) {
    if (t.tex) SDL_DestroyTexture(t.tex);
    t = Thumb{nullptr, 0, 0, 0};
  }
}
