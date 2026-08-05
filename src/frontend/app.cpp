/* app.cpp - the window, the menus and the main loop
 *
 * The interface is Dear ImGui drawn over SDL's renderer, so there is no toolkit
 * to find and nothing to install beside the binary. The emulated screen is
 * drawn by the renderer directly, below the menubar, so it stays pixel-exact
 * under integer scaling.
 */

extern "C" {
#include "app.h"
#include "config.h"
#include "session.h"
#include "savestate.h"
#include "audio.h"
#include "paths.h"
}

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <string>
#include <vector>

namespace {

SDL_Window   *win;
SDL_Renderer *ren;
SDL_Texture  *screen_tex;
SDL_GameController *pad;

AppOptions opts;
bool running = true;
bool paused;
int  last_frame = -1;
bool key_down[BTN_COUNT];

char   status[192];
Uint32 status_until;

bool show_open, show_input, show_about;
std::string message;

void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_until = SDL_GetTicks() + 4000;
}

void update_title(void)
{
    char t[192];
    SDL_snprintf(t, sizeof t, "%s - gwemu",
                 session.open ? session.title : "No firmware");
    SDL_SetWindowTitle(win, t);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

void load_firmware(const char *ip, const char *ep)
{
    const char *err = session_load(ip, ep, &opts);
    if (err) { message = err; gwlog("[ui] %s\n", err); return; }

    emu_set_autostart(true, 0);
    if (config.rtc_host) rtc_sync_host();
    config_push_recent(session.int_path, session.ext_path, session.title);

    char dir[CFG_PATH_MAX];
    path_dirname(dir, sizeof dir, session.int_path);
    SDL_strlcpy(config.last_dir, dir, sizeof config.last_dir);

    last_frame = -1;
    paused = false;
    update_title();
    set_status("Loaded %s", session.title);
}

void act_reset(void)
{
    if (!session.open) return;
    emu_reset();
    emu_set_autostart(true, 0);
    last_frame = -1;
    set_status("Reset");
}

void act_screenshot(void)
{
    if (!session.open) return;

    int w = emu_fb_width(), h = emu_fb_height();
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    memcpy(s->pixels, emu_framebuffer(), (size_t)w * h * 4);

    char path[SESSION_PATH_MAX], name[128];
    for (int n = 1; n < 10000; n++) {
        SDL_snprintf(name, sizeof name, "%s-%04d.bmp",
                     session.title[0] ? session.title : "gwemu", n);
        user_path(path, sizeof path, name);
        if (file_size(path) < 0) break;
    }
    if (SDL_SaveBMP(s, path) == 0) set_status("Saved %s", path_basename(path));
    else message = "Could not write the screenshot.";
    SDL_FreeSurface(s);
}

void act_save_state(void)
{
    if (!session.open) return;
    int n = savestate_slot();
    if (savestate_save(n)) set_status("Saved state %d", n);
    else message = "Could not write that save state.";
}

void act_load_state(void)
{
    if (!session.open) return;
    int n = savestate_slot();
    if (!savestate_exists(n)) { set_status("Slot %d is empty", n); return; }
    if (savestate_load(n)) { last_frame = -1; set_status("Loaded state %d", n); }
    else message = "Could not read that save state. See the log for why.";
}

int menubar_height(void) { return (int)ImGui::GetFrameHeight(); }

void apply_window_size(void)
{
    if (config.fullscreen) return;
    SDL_SetWindowSize(win, 320 * config.scale, 240 * config.scale + menubar_height());
}

void toggle_fullscreen(void)
{
    config.fullscreen = !config.fullscreen;
    SDL_SetWindowFullscreen(win, config.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

bool pad_held(const Binding *b)
{
    if (!pad) return false;
    if (b->type == BIND_PAD)
        return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b->code);
    if (b->type == BIND_AXIS) {
        int v = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)AXIS_OF(b->code));
        return AXIS_POSITIVE(b->code) ? v > AXIS_THRESHOLD : v < -AXIS_THRESHOLD;
    }
    return false;
}

void apply_input(void)
{
    if (!session.open) return;
    if (pad) SDL_GameControllerUpdate();

    /* Typing into a field must not also drive the D-pad. */
    bool keys_free = !ImGui::GetIO().WantCaptureKeyboard;

    for (int i = 0; i < BTN_COUNT; i++) {
        if (i == BTN_POWER) continue;   /* held by the menu and by autostart */
        const Binding *b = &config.bind[i];
        bool down = (b->type == BIND_KEY) ? (keys_free && key_down[i]) : pad_held(b);
        emu_set_button((EmuButton)i, down);
    }
}

void key_event(SDL_Keycode k, bool down)
{
    for (int i = 0; i < BTN_COUNT; i++)
        if (config.bind[i].type == BIND_KEY && config.bind[i].code == k) {
            key_down[i] = down;
            return;
        }
}

void open_pad(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        pad = SDL_GameControllerOpen(i);
        if (pad) { gwlog("[input] gamepad: %s\n", SDL_GameControllerName(pad)); return; }
    }
}

/* ------------------------------------------------------------------ */
/* menus                                                               */
/* ------------------------------------------------------------------ */

void draw_menubar(void)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Firmware...", "Ctrl+O")) show_open = true;
        if (ImGui::MenuItem("Close Firmware", nullptr, false, session.open)) {
            session_close(true);
            update_title();
            set_status("Firmware closed");
        }
        if (ImGui::BeginMenu("Open Recent", config.recent_count > 0)) {
            for (int i = 0; i < config.recent_count; i++) {
                if (ImGui::MenuItem(config.recent[i].label))
                    load_firmware(config.recent[i].int_path,
                                  config.recent[i].ext_path[0] ? config.recent[i].ext_path : nullptr);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", config.recent[i].int_path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear List")) config_clear_recent();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F12", false, session.open)) act_screenshot();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) running = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Emulator")) {
        if (ImGui::MenuItem("Power", nullptr, false, session.open)) emu_tap_power();
        if (ImGui::MenuItem("Reset", "Ctrl+R", false, session.open)) act_reset();
        if (ImGui::MenuItem("Pause", "Ctrl+P", paused, session.open)) paused = !paused;
        ImGui::Separator();
        if (ImGui::MenuItem("Save State", "F5", false, session.open)) act_save_state();
        if (ImGui::MenuItem("Load State", "F8", false, session.open)) act_load_state();
        if (ImGui::BeginMenu("Save Slot")) {
            for (int i = 0; i < SAVESTATE_SLOTS; i++) {
                char l[40];
                SDL_snprintf(l, sizeof l, "Slot %d%s", i, savestate_exists(i) ? "" : "  (empty)");
                if (ImGui::MenuItem(l, nullptr, savestate_slot() == i)) savestate_set_slot(i);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Speed")) {
            static const int speeds[] = { 25, 50, 100, 200, 400 };
            for (int i = 0; i < 5; i++) {
                char l[16];
                SDL_snprintf(l, sizeof l, "%d%%", speeds[i]);
                if (ImGui::MenuItem(l, nullptr, config.speed_percent == speeds[i]))
                    config.speed_percent = speeds[i];
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Config")) {
        if (ImGui::BeginMenu("Window Size")) {
            for (int s = 1; s <= 6; s++) {
                char l[8];
                SDL_snprintf(l, sizeof l, "%dx", s);
                if (ImGui::MenuItem(l, nullptr, config.scale == s)) {
                    config.scale = s;
                    apply_window_size();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Integer Scaling", nullptr, &config.integer_scale);
        if (ImGui::MenuItem("Linear Filter", nullptr, &config.linear_filter))
            SDL_SetTextureScaleMode(screen_tex,
                config.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        if (ImGui::MenuItem("Fullscreen", "F11", config.fullscreen)) toggle_fullscreen();
        ImGui::Separator();

        if (ImGui::MenuItem("Sound", nullptr, &config.audio_enabled)) audio_reconfigure();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderInt("Volume", &config.volume, 0, 150, "%d%%");
        ImGui::Separator();

        if (ImGui::MenuItem("Input...")) show_input = true;
        if (ImGui::MenuItem("Sync clock with host", nullptr, &config.rtc_host)) {
            opt_rtc_host = config.rtc_host;
            if (config.rtc_host && session.open) {
                rtc_sync_host();
                set_status("Clock set from host time");
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) show_about = true;
        ImGui::EndMenu();
    }

    /* Running state, right-aligned, the way a status bar reads. */
    if (session.open) {
        char right[128] = "";
        if (emu_halted())               SDL_strlcat(right, "HALTED  ", sizeof right);
        else if (paused)                SDL_strlcat(right, "PAUSED  ", sizeof right);
        if (config.speed_percent != 100) {
            char sp[24];
            SDL_snprintf(sp, sizeof sp, "%d%%", config.speed_percent);
            SDL_strlcat(right, sp, sizeof right);
        }
        if (right[0]) {
            float w = ImGui::CalcTextSize(right).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
            ImGui::TextUnformatted(right);
        }
    }

    ImGui::EndMainMenuBar();
}

/* ------------------------------------------------------------------ */
/* dialogs                                                             */
/* ------------------------------------------------------------------ */

void draw_open_dialog(void)
{
    static std::string dir;
    static std::vector<DirEntry> entries;
    static int sel = -1;
    static bool loaded;

    auto reload = [&](const char *d) {
        dir = d;
        DirEntry *list = nullptr;
        int n = dir_list(dir.c_str(), &list);
        entries.assign(list, list + n);
        free(list);
        sel = -1;
    };

    if (!loaded) {
        reload(config.last_dir[0] ? config.last_dir : ".");
        loaded = true;
    }

    ImGui::OpenPopup("Open Firmware");
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Open Firmware", nullptr, ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextDisabled("%s", dir.c_str());
    ImGui::Separator();

    auto choose = [&](int i) {
        if (i < 0 || i >= (int)entries.size()) return;
        if (entries[i].is_dir) {
            char next[CFG_PATH_MAX];
            if (!strcmp(entries[i].name, "..")) path_dirname(next, sizeof next, dir.c_str());
            else SDL_snprintf(next, sizeof next, "%s/%s", dir.c_str(), entries[i].name);
            reload(next);
        } else {
            char full[CFG_PATH_MAX];
            SDL_snprintf(full, sizeof full, "%s/%s", dir.c_str(), entries[i].name);
            SDL_strlcpy(config.last_dir, dir.c_str(), sizeof config.last_dir);
            show_open = false;
            loaded = false;
            ImGui::CloseCurrentPopup();
            load_firmware(full, nullptr);
        }
    };

    ImGui::BeginChild("files", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 8), true);
    for (int i = 0; i < (int)entries.size(); i++) {
        char label[320];
        if (entries[i].is_dir)
            SDL_snprintf(label, sizeof label, "[%s]", entries[i].name);
        else {
            char kind = session_classify(entries[i].size);
            SDL_snprintf(label, sizeof label, "%s%s", entries[i].name,
                         kind == 'i' ? "   - internal flash"
                                     : kind == 'e' ? "   - external flash" : "");
        }
        /* Anything that is not one of the two flash images is greyed, so a
           folder of dumps reads at a glance. */
        bool useful = entries[i].is_dir || session_classify(entries[i].size) != 0;
        if (!useful)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Selectable(label, sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            sel = i;
            if (ImGui::IsMouseDoubleClicked(0)) choose(i);
        }
        if (!useful) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    if (ImGui::Button("Open", ImVec2(110, 0))) choose(sel);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        show_open = false;
        loaded = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("either half of the pair will do");

    ImGui::EndPopup();
}

void draw_input_dialog(void)
{
    static int listening = -1;
    static const EmuButton order[] = {
        BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B,
        BTN_GAME, BTN_TIME, BTN_PAUSE, BTN_SELECT, BTN_START,
    };

    ImGui::OpenPopup("Input");
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("Input", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextDisabled("Click a binding, then press a key, button, trigger or stick.");
    ImGui::Separator();

    /* Whatever arrives first while listening becomes the binding. Keys are
       taken from SDL rather than ImGui so the stored code is an SDL keycode. */
    if (listening >= 0) {
        int type = 0, code = 0;
        const Uint8 *keys = SDL_GetKeyboardState(nullptr);
        for (int sc = 0; sc < SDL_NUM_SCANCODES && !type; sc++) {
            if (!keys[sc]) continue;
            if (sc == SDL_SCANCODE_ESCAPE) { listening = -1; break; }
            type = BIND_KEY;
            code = (int)SDL_GetKeyFromScancode((SDL_Scancode)sc);
        }
        if (!type && pad) {
            SDL_GameControllerUpdate();
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && !type; b++)
                if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b)) {
                    type = BIND_PAD; code = b;
                }
            for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX && !type; a++) {
                int v = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a);
                if (v >  AXIS_BIND_THRESHOLD) { type = BIND_AXIS; code = AXIS_CODE(a, true); }
                if (v < -AXIS_BIND_THRESHOLD) { type = BIND_AXIS; code = AXIS_CODE(a, false); }
            }
        }
        if (type) {
            /* One binding wins, so take it from whoever had it. */
            for (int k = 0; k < BTN_COUNT; k++)
                if (k != listening && config.bind[k].type == type && config.bind[k].code == code) {
                    config.bind[k].type = BIND_NONE;
                    config.bind[k].code = 0;
                }
            config.bind[listening].type = type;
            config.bind[listening].code = code;
            listening = -1;
        }
    }

    if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < (int)(sizeof order / sizeof order[0]); i++) {
            EmuButton b = order[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(b == BTN_PAUSE ? "Pause / Set" : emu_button_name(b));
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID((int)b);
            const char *label = (listening == (int)b) ? "press..." : config_bind_name(b);
            if (ImGui::Button(label, ImVec2(-1, 0))) listening = (int)b;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("Defaults", ImVec2(110, 0))) {
        Config saved = config;
        config_defaults();
        memcpy(saved.bind, config.bind, sizeof saved.bind);
        config = saved;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110, 0))) {
        show_input = false;
        listening = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_about(void)
{
    ImGui::OpenPopup("About gwemu");
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("About gwemu", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextUnformatted("gwemu");
    ImGui::Separator();
    ImGui::PushTextWrapPos(430.0f);
    ImGui::TextUnformatted(
        "An emulator for the Nintendo Game & Watch (2020), an STM32H7B0 running "
        "its unmodified factory firmware. Nothing is reimplemented at the game "
        "level: the firmware decrypts its own flash and paints real frames.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Text("Screen 320x240.  Core clock %u MHz.", opt_core_hz / 1000000);
    ImGui::TextDisabled("No Nintendo code or assets included.");
    ImGui::TextDisabled("Firmware dumps are not distributed; use your own.");
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        show_about = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_message(void)
{
    ImGui::OpenPopup("Cannot load firmware");
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("Cannot load firmware", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::PushTextWrapPos(430.0f);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        message.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ------------------------------------------------------------------ */
/* presentation                                                        */
/* ------------------------------------------------------------------ */

void present(int bar_h)
{
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    if (session.open && screen_tex) {
        int frame = emu_frame_counter();
        if (frame != last_frame) {
            last_frame = frame;
            SDL_UpdateTexture(screen_tex, NULL, emu_framebuffer(), emu_fb_width() * 4);
        }

        float fw = (float)emu_fb_width(), fh = (float)emu_fb_height();
        float availw = (float)ww, availh = (float)(wh - bar_h);
        if (availh < 1.0f) availh = 1.0f;
        float s = availw / fw < availh / fh ? availw / fw : availh / fh;
        if (config.integer_scale && s >= 1.0f) s = (float)(int)s;

        SDL_Rect dst;
        dst.w = (int)(fw * s);
        dst.h = (int)(fh * s);
        dst.x = (ww - dst.w) / 2;
        dst.y = bar_h + ((int)availh - dst.h) / 2;
        SDL_RenderCopy(ren, screen_tex, NULL, &dst);
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
    SDL_RenderPresent(ren);
}

void hotkey(SDL_Keycode k, Uint16 mod)
{
    bool ctrl = (mod & KMOD_CTRL) != 0;
    switch (k) {
    case SDLK_F5:  act_save_state(); break;
    case SDLK_F8:  act_load_state(); break;
    case SDLK_F11: toggle_fullscreen(); break;
    case SDLK_F12: act_screenshot(); break;
    case SDLK_ESCAPE: if (config.fullscreen) toggle_fullscreen(); break;
    case SDLK_o: if (ctrl) show_open = true; break;
    case SDLK_q: if (ctrl) running = false; break;
    case SDLK_p: if (ctrl) paused = !paused; break;
    case SDLK_r: if (ctrl) act_reset(); break;
    default:
        if (ctrl && k >= SDLK_0 && k <= SDLK_9) savestate_set_slot((int)(k - SDLK_0));
        break;
    }
}

void style_dark(void)
{
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding   = 2.0f;
    s.FrameRounding    = 2.0f;
    s.GrabRounding     = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding     = ImVec2(6, 4);
    s.ItemSpacing      = ImVec2(8, 5);
    s.Colors[ImGuiCol_WindowBg]       = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);
    s.Colors[ImGuiCol_PopupBg]        = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);
    s.Colors[ImGuiCol_MenuBarBg]      = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    s.Colors[ImGuiCol_Header]         = ImVec4(0.24f, 0.49f, 0.75f, 1.00f);
    s.Colors[ImGuiCol_HeaderHovered]  = ImVec4(0.29f, 0.56f, 0.84f, 1.00f);
    s.Colors[ImGuiCol_Button]         = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    s.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.31f, 0.31f, 0.36f, 1.00f);
    s.Colors[ImGuiCol_FrameBg]        = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    s.Colors[ImGuiCol_SliderGrab]     = ImVec4(0.29f, 0.56f, 0.84f, 1.00f);
}

} /* namespace */

/* ------------------------------------------------------------------ */

int app_run(const AppOptions *o)
{
    opts = *o;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    config_load();
    if (o->scale > 0 && o->scale != 3) config.scale = o->scale;  /* --scale wins */
    if (o->fullscreen) config.fullscreen = true;
    if (o->rtc_host)   config.rtc_host = true;
    opts.rtc_host = config.rtc_host;

    win = SDL_CreateWindow("gwemu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           320 * config.scale, 240 * config.scale + 24,
                           SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   /* window layout is not worth keeping */
    style_dark();
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    screen_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, 320, 240);
    SDL_SetTextureScaleMode(screen_tex,
        config.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    if (config.fullscreen) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);

    open_pad();
    audio_open();
    update_title();

    if (o->int_path || o->ext_path) load_firmware(o->int_path, o->ext_path);

    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 origin = SDL_GetPerformanceCounter();
    u64 emu_base = 0;
    int bar_h = 24;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (!ImGui::GetIO().WantCaptureKeyboard) {
                    if (!e.key.repeat) key_event(e.key.keysym.sym, true);
                    hotkey(e.key.keysym.sym, e.key.keysym.mod);
                }
                break;
            case SDL_KEYUP:
                key_event(e.key.keysym.sym, false);
                break;
            case SDL_DROPFILE:
                load_firmware(e.drop.file, nullptr);
                SDL_free(e.drop.file);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!pad) open_pad();
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad) { SDL_GameControllerClose(pad); pad = nullptr; }
                break;
            default:
                break;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        draw_menubar();
        bar_h = menubar_height();

        if (show_open)  draw_open_dialog();
        if (show_input) draw_input_dialog();
        if (show_about) draw_about();
        if (!message.empty()) draw_message();

        bool modal = show_open || show_input || show_about || !message.empty();
        bool halted = paused || !session.open || emu_halted() || modal;

        if (!session.open && !modal) {
            ImGuiViewport *vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                           vp->Pos.y + vp->Size.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.0f);
            if (ImGui::Begin("##empty", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
                ImGui::TextDisabled("No firmware loaded - File > Open Firmware");
            ImGui::End();
        }

        if (status[0] && SDL_GetTicks() < status_until) {
            ImGuiViewport *vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8, vp->Pos.y + vp->Size.y - 8),
                                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.55f);
            if (ImGui::Begin("##status", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
                ImGui::TextUnformatted(status);
            ImGui::End();
        }

        audio_set_paused(halted);
        apply_input();

        /* Emulated time follows the wall clock, scaled by the speed setting. */
        double secs = (double)(SDL_GetPerformanceCounter() - origin) / (double)freq;
        u64 want = (u64)(secs * (double)opt_core_hz * config.speed_percent / 100.0);

        if (halted) {
            emu_base = want;
        } else {
            u64 budget = want > emu_base ? want - emu_base : 0;
            /* Give up on more than a fifth of a second of arrears rather than
               sprinting to catch up after a stall. */
            u64 debt = opt_core_hz / 5;
            if (budget > debt) { emu_base = want - debt; budget = debt; }
            /* One pass has to cover a whole display refresh or the emulator
               falls behind the wall clock for good; the cap is only here to
               bound how long the interface can go unserviced. */
            u64 slice = opt_core_hz / 20;
            if (budget > slice) budget = slice;
            emu_base += emu_step((u32)budget);
        }

        present(bar_h);
        if (halted) SDL_Delay(8);
    }

    if (session.open) session_close(true);
    config_save();

    audio_close();
    if (pad) SDL_GameControllerClose(pad);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (screen_tex) SDL_DestroyTexture(screen_tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
