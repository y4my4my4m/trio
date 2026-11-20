#include "overlay.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saveslots_ui.h"

namespace {

SDL_Renderer *g_ren;
bool g_open;
int g_capture = -1;        //bind index waiting for a key, second binding when >= BIND_COUNT
int g_pad_capture = -1;    //bind index waiting for a pad button

bool check(const char *label, bool *v) { return ImGui::Checkbox(label, v); }

void hint(const char *text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

bool tab_game(Settings *c, const OverlayInfo *info, int *act) {
  bool ch = false;
  if (ImGui::Button("Resume", ImVec2(220, 0))) g_open = false;
  if (ImGui::Button("Save to slot (F5)", ImVec2(220, 0))) *act |= OV_SAVE_STATE;
  if (ImGui::Button("Load slot (F9)", ImVec2(220, 0))) *act |= OV_LOAD_STATE;
  if (ImGui::Button("Reset (Ctrl+R)", ImVec2(220, 0))) { *act |= OV_RESET; g_open = false; }
  if (info->can_game_select && ImGui::Button("Game select", ImVec2(220, 0))) { *act |= OV_GAME_SELECT; g_open = false; }
  ImGui::Separator();
  ch |= ImGui::SliderInt("Fast-forward speed (Tab)", &c->ff_speed, 2, 16, "%dx");
  ch |= check("Rewind (hold R)", &c->rewind);
  if (c->rewind) {
    ch |= ImGui::SliderInt("Rewind length", &c->rewind_seconds, 5, 60, "%d s");
    ImGui::TextDisabled("History: %.1f s, %.1f MB", info->rewind_states * 2 / 60.0f,
                        info->rewind_bytes / (1024.0f * 1024.0f));
  }
  ch |= check("Pause when the window loses focus", &c->pause_unfocused);
  ImGui::Separator();
  if (ImGui::Button("Quit", ImVec2(220, 0))) *act |= OV_QUIT;
  return ch;
}

bool tab_gameplay(Settings *c) {
  bool ch = false;
  ImGui::TextUnformatted("Blaster");
  ch |= check("Auto-charge", &c->auto_charge);
  hint("The blaster charges by itself; tap shoot to release the charged shot. "
       "Active in stages only, menus see the real button.");
  ImGui::BeginDisabled(c->auto_charge);
  ch |= check("Turbo fire (hold shoot)", &c->turbo_fire);
  ch |= ImGui::SliderInt("Turbo rate", &c->turbo_rate, 2, 8, "%d frames");
  ImGui::EndDisabled();
  hint("Holding shoot keeps firing pellets instead of charging.");
  ImGui::Separator();
  ImGui::TextUnformatted("Assists");
  ch |= check("Infinite health", &c->inf_health);
  ch |= check("Infinite lives", &c->inf_lives);
  hint("Health refills after every hit (spikes and pits still count). A lost life is given back.");
  return ch;
}

bool tab_video(Settings *c, const OverlayInfo *info, int *act) {
  bool ch = false, win = false;
  static const char *wide_names[WIDE_COUNT] = {"Off (4:3)", "16:10", "16:9", "2:1"};
  static const char *filters[FILTER_COUNT] = {"Sharp (nearest)", "Smooth (linear)"};
  //The slider edits a copy; the window resizes once, when the drag ends.
  static int scale_edit;
  static bool scale_dragging;
  if (!scale_dragging) scale_edit = c->scale;
  ImGui::SliderInt("Window scale", &scale_edit, 1, 8);
  scale_dragging = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit() && scale_edit != c->scale) {
    c->scale = scale_edit;
    win = true;
  }
  win |= check("Fullscreen (F11)", &c->fullscreen);
  win |= check("Integer scaling", &c->integer_scale);
  win |= ImGui::Combo("Filter", &c->filter, filters, FILTER_COUNT);
  win |= check("SNES pixel aspect (8:7)", &c->pixel_aspect);
  win |= check("VSync", &c->vsync);
  ImGui::Separator();
  ImGui::BeginDisabled(!info->widescreen_ok);
  win |= ImGui::Combo("Widescreen", &c->widescreen, wide_names, WIDE_COUNT);
  ImGui::EndDisabled();
  if (!info->widescreen_ok)
    hint("Widescreen needs the recompiled build (not --interp) and the USA ROM.");
  else
    hint("Stages only; title, menus and the stage select stay 4:3 and are centered. "
         "Level tiles, enemy spawning and sprites are widened. Switching it on in the "
         "middle of a stage fills the sides as the camera moves (fully after the next "
         "room or respawn).");
  ImGui::BeginDisabled(!info->widescreen_ok || c->widescreen == WIDE_OFF);
  ch |= check("HUD at the screen edges", &c->wide_hud);
  ImGui::EndDisabled();
  ImGui::Separator();
  ch |= check("Remove sprite flicker (no per-line sprite limit)", &c->no_sprite_limit);
  ch |= ImGui::SliderInt("Scanlines", &c->scanlines, 0, 100, c->scanlines ? "%d%%" : "off");
  ch |= check("Show FPS", &c->show_fps);
  if (win) *act |= OV_WINDOW;
  return ch || win;
}

bool tab_audio(Settings *c) {
  bool ch = ImGui::SliderInt("Volume", &c->volume, 0, 100, "%d%%");
  ch |= check("Mute when the window loses focus", &c->mute_unfocused);
  return ch;
}

bool tab_controls(Settings *c) {
  bool ch = false;
  static const char *layouts[PADLAYOUT_COUNT] = {"By position (SNES layout)", "By label (A = A, B = B)"};
  int layout = c->pad_layout;
  if (ImGui::Combo("Gamepad preset", &layout, layouts, PADLAYOUT_COUNT)) { settings_pad_preset(c, layout); ch = true; }
  ch |= ImGui::SliderInt("Stick deadzone", &c->deadzone, 5, 90, "%d%%");
  ch |= check("Pad hotkeys with Back held", &c->pad_hotkeys);
  hint("Back + LB rewind, Back + RB fast forward, Back + Y save, Back + X load, "
       "Back + Start or the Guide button open this menu. A lone tap of Back is Select.");
  ImGui::TextUnformatted("Gamepad (click, then press a pad button or trigger; Esc cancels)");
  if (ImGui::BeginTable("pad", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    for (int b = 0; b < BIND_COUNT; b++) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(settings_bind_name(b));
      ImGui::TableNextColumn();
      char label[64];
      if (g_pad_capture == b) snprintf(label, sizeof(label), "press a button...##p%d", b);
      else snprintf(label, sizeof(label), "%s##p%d", settings_pad_button_name(c->pad_map[b]), b);
      if (ImGui::Button(label, ImVec2(-1, 0))) { g_pad_capture = b; g_capture = -1; }
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Keyboard (click a key to rebind, Esc cancels, Backspace clears)");
  if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Button");
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("Alternate");
    ImGui::TableHeadersRow();
    for (int b = 0; b < BIND_COUNT; b++) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(settings_bind_name(b));
      for (int k = 0; k < 2; k++) {
        ImGui::TableNextColumn();
        int idx = b + k * BIND_COUNT;
        int sc = k ? c->keys2[b] : c->keys[b];
        char label[64];
        if (g_capture == idx) snprintf(label, sizeof(label), "press a key...##%d", idx);
        else snprintf(label, sizeof(label), "%s##%d", sc ? SDL_GetScancodeName((SDL_Scancode)sc) : "-", idx);
        if (ImGui::Button(label, ImVec2(-1, 0))) g_capture = idx;
      }
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("Default keys")) {
    Settings d;
    settings_default(&d);
    memcpy(c->keys, d.keys, sizeof(c->keys));
    memcpy(c->keys2, d.keys2, sizeof(c->keys2));
    ch = true;
  }
  ImGui::Separator();
  if (ImGui::BeginTable("hot", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    auto row = [](const char *a, const char *b) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::TextUnformatted(a);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(b);
    };
    row("Menu", "F1 / Esc, pad Guide or Back + Start");
    row("Fast forward (hold)", "Tab, pad Back + RB");
    row("Rewind (hold)", "R, pad Back + LB");
    row("Pause / frame advance", "P / O");
    row("Save / load selected slot", "F5 / F9, pad Back + Y / X");
    row("Previous / next slot", "F6 / F7");
    row("Load newest autosave", "Shift+F9");
    row("Fullscreen", "F11 / Alt+Enter");
    row("Screenshot (PNG)", "F12");
    row("Reset", "Ctrl+R");
    ImGui::EndTable();
  }
  return ch;
}

}  // namespace

extern "C" bool overlay_init(SDL_Window *win, SDL_Renderer *ren) {
  g_ren = ren;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::StyleColorsDark();
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding = 6;
  st.FrameRounding = 4;
  st.Colors[ImGuiCol_WindowBg].w = 0.94f;
  return ImGui_ImplSDL2_InitForSDLRenderer(win, ren) && ImGui_ImplSDLRenderer2_Init(ren);
}

extern "C" void overlay_shutdown(void) {
  ssui_shutdown();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
}

extern "C" bool overlay_capturing_key(void) { return g_open && (g_capture >= 0 || g_pad_capture >= 0); }

extern "C" bool overlay_event(const SDL_Event *ev, Settings *cfg) {
  if (g_capture >= 0 && ev->type == SDL_KEYDOWN) {
    SDL_Scancode sc = ev->key.keysym.scancode;
    int b = g_capture % BIND_COUNT;
    int *slot = g_capture >= BIND_COUNT ? &cfg->keys2[b] : &cfg->keys[b];
    if (sc == SDL_SCANCODE_BACKSPACE) *slot = 0;
    else if (sc != SDL_SCANCODE_ESCAPE) *slot = sc;
    g_capture = -1;
    settings_save(cfg, "trio.ini");
    return true;
  }
  if (g_pad_capture >= 0) {
    int v = -2;
    if (ev->type == SDL_CONTROLLERBUTTONDOWN && ev->cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE) v = ev->cbutton.button;
    else if (ev->type == SDL_CONTROLLERAXISMOTION && ev->caxis.value > 24000) {
      if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) v = PAD_TRIGGER_L;
      if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) v = PAD_TRIGGER_R;
    } else if (ev->type == SDL_KEYDOWN && ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
      g_pad_capture = -1;
      return true;
    }
    if (v != -2) {
      cfg->pad_map[g_pad_capture] = v;
      g_pad_capture = -1;
      settings_save(cfg, "trio.ini");
      return true;
    }
  }
  ImGui_ImplSDL2_ProcessEvent(ev);
  return g_open;
}

extern "C" void overlay_toggle(void) { g_open = !g_open; g_capture = -1; g_pad_capture = -1; }
extern "C" bool overlay_is_open(void) { return g_open; }

extern "C" int overlay_frame(Settings *cfg, const OverlayInfo *info) {
  int act = 0;
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  ImGuiIO &io = ImGui::GetIO();
  if (cfg->show_fps || (info->status && *info->status) || info->rewinding) {
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (cfg->show_fps) ImGui::Text("%.1f fps", info->fps);
    if (info->rewinding) ImGui::TextUnformatted("<< rewind");
    if (info->status && *info->status) ImGui::TextUnformatted(info->status);
    ImGui::End();
  }

  if (g_open) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 480), ImGuiCond_Appearing);
    bool open = true;
    ImGui::Begin("game 3", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    bool ch = false;
    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Game")) { ch |= tab_game(cfg, info, &act); ImGui::EndTabItem(); }
      static bool tab_env = getenv("TRIO_UI_TAB") != nullptr;
      const char *want = tab_env ? getenv("TRIO_UI_TAB") : "";
      if (ImGui::BeginTabItem("Save/Load", nullptr, tab_env && !strcmp(want, "saves") ? ImGuiTabItemFlags_SetSelected : 0)) {
        ch |= ssui_tab(cfg, g_ren);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Gameplay", nullptr, tab_env && !strcmp(want, "gameplay") ? ImGuiTabItemFlags_SetSelected : 0)) {
        ch |= tab_gameplay(cfg);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Video", nullptr, tab_env && !strcmp(want, "video") ? ImGuiTabItemFlags_SetSelected : 0)) {
        ch |= tab_video(cfg, info, &act);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Audio")) { ch |= tab_audio(cfg); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Controls", nullptr, tab_env && !strcmp(want, "controls") ? ImGuiTabItemFlags_SetSelected : 0)) {
        ch |= tab_controls(cfg);
        ImGui::EndTabItem();
      }
      tab_env = false;
      ImGui::EndTabBar();
    }
    ImGui::End();
    if (!open) g_open = false;
    if (ch) act |= OV_SETTINGS;
  }

  ImGui::Render();
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
  return act;
}
