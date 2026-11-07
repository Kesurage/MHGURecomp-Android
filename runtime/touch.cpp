// Touch controls for phones and tablets: runtime/touch.h. SDL's finger events move buttons and two
// floating stick zones; touch_update merges what they hold into the Switch controller the game reads
// (host_input_update). The layout is drawn on the ImGui overlay's foreground draw list, edited with a
// finger in the edit mode (drag to move, the toolbar for the rest), kept per profile in touch.txt next
// to mhgurecomp.ini, and haptic-ticked with the device's vibrator when it has one.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "config.h"
#include "hle/input.h"
#include "overlay.h"
#include "platform.h"
#include "touch.h"

// (runtime/ui.c: the game window, for a finger's pixel space. C linkage, like the rest of the runtime.)
extern "C" SDL_Window *ui_window(void);

#define B(x) (1ull << (x))
#define LOG(...) fprintf(stderr, __VA_ARGS__)

// ------------------------------------------------------------------ layout

enum { CTL_BUTTON, CTL_STICK, CTL_MENU, CTL_PROFILE };
typedef struct {
    int kind;          // CTL_*
    int btn;           // CTL_BUTTON: the Switch button (BTN_*); CTL_STICK: 0 left, 1 right
    char label[16];    // drawn on the control (custom buttons name themselves)
    float cx, cy;      // centre, 0..1 of the window
    float r;           // radius, a fraction of the window's height
} Ctl;

static constexpr int MAX_PROFILES = 8;

typedef struct {
    char name[32];
    std::vector<Ctl> ctls;
} Profile;

// The button a CTL_BUTTON presses, and how it is labelled by default. L3 / R3 are the sticks' clicks.
typedef struct { const char *name; int btn; } BtnDef;
static const BtnDef BTN_DEFS[] = {
    {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R},
    {"ZL", BTN_ZL}, {"ZR", BTN_ZR}, {"+", BTN_PLUS}, {"-", BTN_MINUS},
    {"\xE2\x97\x80", BTN_LEFT}, {"\xE2\x96\xB2", BTN_UP}, {"\xE2\x96\xB6", BTN_RIGHT}, {"\xE2\x96\xBC", BTN_DOWN},
    {"L3", BTN_LSTICK}, {"R3", BTN_RSTICK},
};
static const char *const STICK_NAMES[] = {"Move", "Camera"};

static Profile *cur(void);
static std::mutex g_lock;
static std::vector<Profile> g_profiles;
static int g_profile;             // the one in use
static bool g_editing;
static int g_selected = -1;       // the control the edit mode has (index into the profile)
static bool g_adding;             // the edit mode's add list is open

static float cfg_opacity() { return config_live_touch_opacity(); }
static float cfg_scale() { return config_live_touch_scale(); }
static float cfg_sensitivity() { return config_live_touch_sensitivity(); }
static bool g_edit_request;  // the settings menu's Edit touch layout, until overlay_frame takes it

// The defaults: a layout for a phone held landscape (Infinix Hot 50 4G and friends). Positions are
// guesses at a thumb's reach; the edit mode moves everything anyway.
static void default_layout(std::vector<Ctl> &c) {
    auto btn = [&](int b, const char *label, float x, float y, float r) {
        c.push_back({CTL_BUTTON, b, "", x, y, r});
        snprintf(c.back().label, sizeof c.back().label, "%s", label);
    };
    c.push_back({CTL_STICK, 0, "", 0.115f, 0.730f, 0.105f});       // move
    c.push_back({CTL_STICK, 1, "", 0.660f, 0.745f, 0.085f});       // camera
    btn(BTN_A, "A", 0.935f, 0.575f, 0.050f);
    btn(BTN_B, "B", 0.855f, 0.665f, 0.050f);
    btn(BTN_X, "X", 0.855f, 0.485f, 0.050f);
    btn(BTN_Y, "Y", 0.775f, 0.575f, 0.050f);
    btn(BTN_ZL, "ZL", 0.060f, 0.070f, 0.048f);
    btn(BTN_ZR, "ZR", 0.940f, 0.070f, 0.048f);
    btn(BTN_L, "L", 0.175f, 0.055f, 0.040f);
    btn(BTN_R, "R", 0.825f, 0.055f, 0.040f);
    btn(BTN_PLUS, "+", 0.585f, 0.050f, 0.034f);
    btn(BTN_MINUS, "-", 0.415f, 0.050f, 0.034f);
    btn(BTN_UP, "\xE2\x96\xB2", 0.500f, 0.845f, 0.037f);
    btn(BTN_DOWN, "\xE2\x96\xBC", 0.500f, 0.950f, 0.037f);
    btn(BTN_LEFT, "\xE2\x97\x80", 0.430f, 0.898f, 0.037f);
    btn(BTN_RIGHT, "\xE2\x96\xB6", 0.570f, 0.898f, 0.037f);
    c.push_back({CTL_MENU, 0, "", 0.500f, 0.050f, 0.030f});
    c.push_back({CTL_PROFILE, 0, "", 0.665f, 0.050f, 0.030f});
}

static void make_default_profiles(void) {
    g_profiles.clear();
    Profile p{};
    snprintf(p.name, sizeof p.name, "Default");
    default_layout(p.ctls);
    g_profiles.push_back(std::move(p));
    g_profile = 0;
}

// ------------------------------------------------------------------ persistence (touch.txt, beside the ini)

static void touch_path(char *out, size_t size) {
    char ini[1100];
    config_path(ini, sizeof ini);
    char *slash = strrchr(ini, '/');
#ifdef _WIN32
    char *bs = strrchr(ini, '\\');
    if (bs > slash) slash = bs;
#endif
    if (slash) *slash = 0;
    snprintf(out, size, "%s/touch.txt", slash ? ini : ".");
}

static void save_locked(void) {
    char path[1100];
    touch_path(path, sizeof path);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG("[touch] cannot write %s\n", path); return; }
    fprintf(f, "; MHGU Recomp touch controls (runtime/touch.cpp). A profile per weapon, say; the app's\n"
               "; edit mode writes this file. button: <BtnDef name> <x> <y> <r>; stick: L|R; menu; profile.\n");
    for (const Profile &p : g_profiles) {
        fprintf(f, "\nprofile %s\n", p.name);
        for (const Ctl &c : p.ctls) {
            if (c.kind == CTL_BUTTON) fprintf(f, "button %s %.4f %.4f %.4f\n", c.label, (double)c.cx, (double)c.cy, (double)c.r);
            else if (c.kind == CTL_STICK) fprintf(f, "stick %s %.4f %.4f %.4f\n", STICK_NAMES[c.btn], (double)c.cx, (double)c.cy, (double)c.r);
            else if (c.kind == CTL_MENU) fprintf(f, "menu %.4f %.4f %.4f\n", (double)c.cx, (double)c.cy, (double)c.r);
            else fprintf(f, "profiles %.4f %.4f %.4f\n", (double)c.cx, (double)c.cy, (double)c.r);
        }
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) remove(tmp);
}

static bool parse_two(const char *s, float *x, float *y, float *r) {
    return sscanf(s, "%f %f %f", x, y, r) == 3;
}

static void load_locked(void) {
    char path[1100];
    touch_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;  // (the defaults stay)
    Profile cur_p{};
    bool have = false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, "\r\n")] = 0;
        if (*s == ';' || *s == '#' || !*s) continue;
        char arg[160] = "";
        if (sscanf(s, "profile %31[^\n]", cur_p.name) == 1) {
            if (have && cur_p.ctls.size()) g_profiles.push_back(cur_p);
            cur_p = Profile{};
            have = true;
            continue;
        }
        if (!have) { snprintf(cur_p.name, sizeof cur_p.name, "Default"); have = true; }
        float x, y, r;
        if (sscanf(s, "button %15s", arg) == 1 && parse_two(strchr(s, arg - s) + strlen(arg) + 1, &x, &y, &r)) {
            int btn = -1;
            const char *label = arg;
            for (const BtnDef &d : BTN_DEFS)
                if (!strcmp(arg, d.name)) btn = d.btn;
            if (btn < 0)  // (a custom label: find the button it was added as, or a button by its Switch name)
                for (const BtnDef &d : BTN_DEFS)
                    if (!strcasecmp(arg, d.name)) { btn = d.btn; label = d.name; }
            if (btn >= 0 && x >= 0 && x <= 1 && y >= 0 && y <= 1 && r > 0.005f && r < 0.5f) {
                Ctl c{CTL_BUTTON, btn, "", x, y, r};
                snprintf(c.label, sizeof c.label, "%s", label);
                cur_p.ctls.push_back(c);
            }
        } else if (sscanf(s, "stick %15s", arg) == 1 && parse_two(strchr(s, arg - s) + strlen(arg) + 1, &x, &y, &r)) {
            int st = !strcasecmp(arg, "camera") || !strcasecmp(arg, "R");
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1 && r > 0.02f && r < 0.5f) cur_p.ctls.push_back({CTL_STICK, st, "", x, y, r});
        } else if (sscanf(s, "menu %f %f %f", &x, &y, &r) == 3) {
            cur_p.ctls.push_back({CTL_MENU, 0, "", x, y, r});
        } else if (sscanf(s, "profiles %f %f %f", &x, &y, &r) == 3) {
            cur_p.ctls.push_back({CTL_PROFILE, 0, "", x, y, r});
        }
    }
    fclose(f);
    if (have && cur_p.ctls.size()) g_profiles.push_back(cur_p);
    if (g_profiles.empty()) make_default_profiles();
    if (g_profile >= (int)g_profiles.size()) g_profile = 0;
}

static bool loaded;
static void ensure_loaded(void) {
    if (loaded) return;
    loaded = true;
    make_default_profiles();
    load_locked();
}

// ------------------------------------------------------------------ the fingers

struct Touch { SDL_FingerID id; float x, y; };  // x, y in window pixels
static std::vector<Touch> g_down;               // what each held finger started on (-1: none)
static std::vector<int> g_hit;                  // index into the profile's controls, per held finger
static uint64_t g_buttons;                      // what the touch controls hold right now
static float g_stick[2][2];                     // left, right: -1..1, y up
static int g_menu_pending;                      // a menu/profile press seen by touch_update's thread

bool touch_enabled(void) { return config_live_touch_enabled(); }

bool touch_draws(void) {
    if (!touch_enabled()) {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_editing && !g_edit_request) return false;
    }
    if (overlay_menu_open() && !g_editing) return false;
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    return g_editing || g_edit_request || !cur()->ctls.empty();
}

static void haptic_tick(void) {
    static SDL_Haptic *h;
    static bool tried, ok;
    if (!config_live_touch_haptics()) return;
    if (tried) { if (ok) SDL_PlayHapticRumble(h, 0.18f, 45); return; }
    tried = true;
    if (!(SDL_WasInit(0) & SDL_INIT_HAPTIC) && !SDL_InitSubSystem(SDL_INIT_HAPTIC)) return;
    int n = 0;
    SDL_HapticID *ids = SDL_GetHaptics(&n);
    if (ids && n > 0) h = SDL_OpenHaptic(ids[0]);
    SDL_free(ids);
    if (h && SDL_InitHapticRumble(h)) ok = true;
    if (ok) SDL_PlayHapticRumble(h, 0.18f, 45);
}

// The control at (x, y) window pixels, or -1. Window size from SDL's own (the finger coords are in it).
static int hit(const std::vector<Ctl> &ctls, float x, float y, int w, int h) {
    float scale = cfg_scale();
    for (int i = (int)ctls.size() - 1; i >= 0; i--) {
        const Ctl &c = ctls[i];
        float rad = c.r * (float)h * scale;
        float dx = x - c.cx * (float)w, dy = y - c.cy * (float)h;
        float slack = c.kind == CTL_STICK ? 1.6f : 1.25f;  // (sticks: a finger's pad is wide; buttons: a little)
        if (dx * dx + dy * dy <= rad * rad * slack * slack) return i;
    }
    return -1;
}

static void ctl_label(const Ctl &c, char *out, size_t size) {
    if (c.kind == CTL_BUTTON) snprintf(out, size, "%s", c.label);
    else if (c.kind == CTL_STICK) snprintf(out, size, "%s", STICK_NAMES[c.btn]);
    else if (c.kind == CTL_MENU) snprintf(out, size, "\xE2\x89\xA1");
    else snprintf(out, size, "\xE2\x87\x84");
}

static bool edit_event(const SDL_Event *e, size_t slot, float fx, float fy, int w, int h);

bool touch_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP)
        return false;
    SDL_Window *win = ui_window();
    int w = 0, h = 0;
    if (!win || !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return false;
    SDL_GetWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return false;
    if (!touch_enabled() && !g_editing) return false;
    float fx = e->tfinger.x * (float)w, fy = e->tfinger.y * (float)h;
    SDL_FingerID id = e->tfinger.fingerID;

    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();

    // find (or make) this finger's slot
    size_t slot = 0;
    while (slot < g_down.size() && g_down[slot].id != id) slot++;
    if (e->type == SDL_EVENT_FINGER_UP && slot >= g_down.size()) return false;

    if (g_editing) return edit_event(e, slot, fx, fy, w, h);  // (declared below; consumes everything)

    if (e->type == SDL_EVENT_FINGER_UP) {
        if (g_hit[slot] >= 0) {
            Ctl &c = cur()->ctls[g_hit[slot]];
            if (c.kind == CTL_BUTTON) g_buttons &= ~B(c.btn);
            else if (c.kind == CTL_STICK) g_stick[c.btn][0] = g_stick[c.btn][1] = 0;
        }
        g_down.erase(g_down.begin() + slot);
        g_hit.erase(g_hit.begin() + slot);
        return true;
    }
    if (slot >= g_down.size()) {  // a new finger
        if (g_down.size() >= 10) return false;
        g_down.push_back({id, fx, fy});
        g_hit.push_back(-1);
        slot = g_down.size() - 1;
    } else {
        g_down[slot].x = fx, g_down[slot].y = fy;
    }
    int i = hit(cur()->ctls, fx, fy, w, h);
    if (i >= 0 && g_hit[slot] < 0) {  // a finger that was nowhere takes the control it found
        g_hit[slot] = i;
        const Ctl &c = cur()->ctls[i];
        if (c.kind == CTL_BUTTON) {
            g_buttons |= B(c.btn);
            haptic_tick();
        } else if (c.kind == CTL_STICK) {
            g_stick[c.btn][0] = g_stick[c.btn][1] = 0;
            haptic_tick();
        } else if (c.kind == CTL_MENU) {
            g_menu_pending = 1;
            haptic_tick();
        } else if (c.kind == CTL_PROFILE) {
            g_menu_pending = 2;
            haptic_tick();
        }
        g_down[slot].x = fx, g_down[slot].y = fy;  // (a stick's anchor: where the finger came down)
    } else if (g_hit[slot] >= 0) {
        const Ctl &c = cur()->ctls[g_hit[slot]];
        if (c.kind == CTL_STICK) {  // (the finger moved: retarget the stick from its anchor)
            float rad = c.r * (float)h * cfg_scale();
            float dx = (fx - g_down[slot].x) / rad, dy = (fy - g_down[slot].y) / rad;
            float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) dx /= m, dy /= m;
            float dz = 0.14f;
            float out = m < dz ? 0 : (m - dz) / (1 - dz);
            float n = m > 0 ? out / m : 0;
            g_stick[c.btn][0] = dx * n * cfg_sensitivity();
            g_stick[c.btn][1] = -dy * n * cfg_sensitivity();
        }
    }
    return i >= 0;
}

// ------------------------------------------------------------------ the controller merge

void touch_update(uint64_t *buttons, int *lx, int *ly, int *rx, int *ry) {
    if (!touch_enabled() || overlay_menu_open()) return;
    std::lock_guard<std::mutex> l(g_lock);
    *buttons |= g_buttons;
    *lx += (int)std::clamp(g_stick[0][0], -1.0f, 1.0f) * 32767;
    *ly += (int)std::clamp(g_stick[0][1], -1.0f, 1.0f) * 32767;
    *rx += (int)std::clamp(g_stick[1][0], -1.0f, 1.0f) * 32767;
    *ry += (int)std::clamp(g_stick[1][1], -1.0f, 1.0f) * 32767;
    if (g_menu_pending) {  // (the caller's thread, where the menu may be opened: runtime/overlay.cpp)
        if (g_menu_pending == 1) overlay_toggle_settings();
        else touch_profile_select(-1);
        g_menu_pending = 0;
        g_buttons = 0;
        for (float &v : g_stick[0]) v = 0;
        for (float &v : g_stick[1]) v = 0;
    }
}

// ------------------------------------------------------------------ profiles

static Profile *cur(void) { return &g_profiles[g_profile]; }

int touch_profile_count(void) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    return (int)g_profiles.size();
}
const char *touch_profile_name(int index) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    return index >= 0 && index < (int)g_profiles.size() ? g_profiles[index].name : "?";
}
int touch_profile_current(void) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    return g_profile;
}
void touch_profile_select(int index) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    int n = (int)g_profiles.size();
    if (index < 0) g_profile = (g_profile + 1) % n;
    else if (index < n) g_profile = index;
    g_buttons = 0;
    g_stick[0][0] = g_stick[0][1] = g_stick[1][0] = g_stick[1][1] = 0;
    LOG("[touch] profile %s\n", g_profiles[g_profile].name);
}
void touch_profile_add(const char *name) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    if ((int)g_profiles.size() >= MAX_PROFILES) return;
    Profile p = *cur();
    snprintf(p.name, sizeof p.name, "%s", name && *name ? name : "New");
    g_profiles.push_back(std::move(p));
    g_profile = (int)g_profiles.size() - 1;
    save_locked();
}
void touch_profile_delete(int index) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    if ((int)g_profiles.size() <= 1 || index < 0 || index >= (int)g_profiles.size()) return;
    g_profiles.erase(g_profiles.begin() + index);
    if (g_profile >= (int)g_profiles.size()) g_profile = 0;
    g_selected = -1;
    save_locked();
}
void touch_reset_layout(void) {
    std::lock_guard<std::mutex> l(g_lock);
    make_default_profiles();
    g_selected = -1;
    save_locked();
}
void touch_set_editing(bool on) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    g_editing = on;
    g_selected = -1;
    g_adding = false;
    g_buttons = 0;
    g_stick[0][0] = g_stick[0][1] = g_stick[1][0] = g_stick[1][1] = 0;
    if (!on) save_locked();
    LOG("[touch] edit mode %s\n", on ? "on: drag the controls, the toolbar has the rest" : "off");
}
bool touch_editing(void) {
    std::lock_guard<std::mutex> l(g_lock);
    return g_editing;
}

// ------------------------------------------------------------------ the edit mode (a finger moves a control)

static bool g_dragging;      // the selected control follows this finger
static float g_grab_dx, g_grab_dy;  // where in the control the finger took it

static bool edit_event(const SDL_Event *e, size_t slot, float fx, float fy, int w, int h) {
    // (g_lock held)
    if (slot >= g_down.size()) {
        if (e->type != SDL_EVENT_FINGER_DOWN || g_down.size() >= 10) return true;
        g_down.push_back({e->tfinger.fingerID, fx, fy});
        g_hit.push_back(-1);
        slot = g_down.size() - 1;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        g_selected = hit(cur()->ctls, fx, fy, w, h);
        if (g_selected >= 0) {
            const Ctl &c = cur()->ctls[g_selected];
            g_dragging = true;
            g_grab_dx = fx - c.cx * (float)w;
            g_grab_dy = fy - c.cy * (float)h;
            haptic_tick();
        }
    } else if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (g_dragging && g_selected >= 0) {
            Ctl &c = cur()->ctls[g_selected];
            c.cx = std::clamp((fx - g_grab_dx) / (float)w, 0.02f, 0.98f);
            c.cy = std::clamp((fy - g_grab_dy) / (float)h, 0.02f, 0.98f);
        }
    } else if (e->type == SDL_EVENT_FINGER_UP) {
        g_dragging = false;
        g_down.erase(g_down.begin() + slot);
        g_hit.erase(g_hit.begin() + slot);
    }
    return true;  // (the edit mode consumes every touch)
}

// ------------------------------------------------------------------ drawing (the ImGui overlay's frame)

#include "imgui.h"

static ImU32 alpha(ImU32 rgb, float a) { return (rgb & 0x00FFFFFFu) | (ImU32)(a * 255.0f) << 24; }

static void draw_ctl(ImDrawList *dl, ImFont *font, const Ctl &c, float px, float py, float rad, float op,
                     bool held, bool selected, float stickx, float sticky) {
    float fill = held ? 84.0f : 34.0f;
    ImU32 ink = alpha(0xFFFFFF, op * (held ? 0.95f : 0.62f));
    dl->AddCircleFilled(ImVec2(px, py), rad, alpha(0xFFFFFF, op * fill / 255.0f), 40);
    dl->AddCircle(ImVec2(px, py), rad, ink, 40, std::max(1.5f, rad * 0.045f));
    if (c.kind == CTL_STICK) {
        float kx = px + stickx * rad * 0.55f, ky = py - sticky * rad * 0.55f;
        dl->AddCircleFilled(ImVec2(kx, ky), rad * 0.38f, alpha(0xFFFFFF, op * (held ? 150 : 90) / 255.0f), 32);
        dl->AddCircle(ImVec2(kx, ky), rad * 0.38f, ink, 32, std::max(1.2f, rad * 0.04f));
    }
    char label[16];
    ctl_label(c, label, sizeof label);
    float size = std::clamp(rad * 0.9f, 9.0f, 72.0f);
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, label);
    dl->AddText(font, size, ImVec2(px - ts.x * 0.5f, py - ts.y * 0.5f), ink, label);
    if (selected) {
        dl->AddCircle(ImVec2(px, py), rad + 6.0f, IM_COL32(255, 178, 64, 255), 48, 3.0f);
        dl->AddCircle(ImVec2(px, py), rad + 11.0f, IM_COL32(255, 178, 64, 130), 48, 1.5f);
    }
}

void touch_draw(float ui_scale) {
    bool edit, adding;
    int selected;
    float op, scale;
    Profile p;
    float sticks[2][2];
    uint64_t buttons;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ensure_loaded();
        edit = g_editing;
        adding = g_adding;
        selected = g_selected;
        if (!edit && !touch_enabled()) return;
        if (!edit && overlay_menu_open()) return;
        op = edit ? 0.95f : cfg_opacity();
        scale = cfg_scale();
        p = *cur();
        memcpy(sticks, g_stick, sizeof sticks);
        buttons = g_buttons;
    }
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float W = vp->Size.x, H = vp->Size.y;
    if (W <= 0 || H <= 0) return;
    ImFont *font = ImGui::GetFont();
    ImDrawList *dl = ImGui::GetForegroundDrawList();

    for (int i = 0; i < (int)p.ctls.size(); i++) {
        const Ctl &c = p.ctls[i];
        float rad = c.r * H * scale, px = c.cx * W, py = c.cy * H;
        bool held = false;
        if (c.kind == CTL_BUTTON && !edit) held = (buttons & B(c.btn)) != 0;
        if (c.kind == CTL_STICK && !edit) held = sticks[c.btn][0] != 0 || sticks[c.btn][1] != 0;
        float sx = c.kind == CTL_STICK ? sticks[c.btn][0] : 0, sy = c.kind == CTL_STICK ? sticks[c.btn][1] : 0;
        draw_ctl(dl, font, c, px, py, rad, op, held, edit && i == selected, sx, sy);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (edit) {
        char title[64];
        snprintf(title, sizeof title, "Touch layout: %s", p.name);
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, 12 * ui_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.82f);
        bool done = false;
        ImGui::Begin("##touchlayout", nullptr, flags);
        ImGui::TextUnformatted(title);
        ImGui::SameLine();
        done |= ImGui::Button("Done");
        ImGui::SameLine();
        if (ImGui::Button(adding ? "Cancel add" : "Add")) {
            std::lock_guard<std::mutex> l(g_lock);
            g_adding = !g_adding;
            g_selected = -1;
            adding = g_adding;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0);
        if (ImGui::Button("- size")) {
            std::lock_guard<std::mutex> l(g_lock);
            if (selected >= 0) cur()->ctls[selected].r = std::clamp(cur()->ctls[selected].r * 0.88f, 0.012f, 0.30f);
        }
        ImGui::SameLine();
        if (ImGui::Button("+ size")) {
            std::lock_guard<std::mutex> l(g_lock);
            if (selected >= 0) cur()->ctls[selected].r = std::clamp(cur()->ctls[selected].r * 1.14f, 0.012f, 0.30f);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0 || p.ctls.size() <= 1);
        if (ImGui::Button("Delete")) {
            std::lock_guard<std::mutex> l(g_lock);
            if (selected >= 0) {
                cur()->ctls.erase(cur()->ctls.begin() + selected);
                g_selected = -1;
                g_dragging = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset all")) {
            std::lock_guard<std::mutex> l(g_lock);
            make_default_profiles();
            g_selected = -1;
            save_locked();
        }
        ImGui::End();
        if (done) {
            touch_set_editing(false);
            return;
        }

        if (adding) {  // (what a finger can add: the Switch's buttons, two sticks, the two actions)
            ImGui::SetNextWindowPos(ImVec2(W * 0.5f, 74 * ui_scale), ImGuiCond_Always, ImVec2(0.5f, 0));
            ImGui::SetNextWindowBgAlpha(0.9f);
            ImGui::Begin("##touchadd", nullptr, flags);
            ImGui::TextUnformatted("Add (it lands mid-screen; drag it from there):");
            for (const BtnDef &d : BTN_DEFS) {
                ImGui::SameLine();
                char id[24];
                snprintf(id, sizeof id, "%s##add", d.name);
                if (ImGui::Button(id)) {
                    std::lock_guard<std::mutex> l(g_lock);
                    Ctl c{CTL_BUTTON, d.btn, "", 0.5f, 0.42f, 0.045f};
                    snprintf(c.label, sizeof c.label, "%s", d.name);
                    cur()->ctls.push_back(c);
                    g_selected = (int)cur()->ctls.size() - 1;
                    g_adding = false;
                }
            }
            for (int s = 0; s < 2; s++) {
                ImGui::SameLine();
                char id[24];
                snprintf(id, sizeof id, "%s##add", STICK_NAMES[s]);
                if (ImGui::Button(id)) {
                    std::lock_guard<std::mutex> l(g_lock);
                    cur()->ctls.push_back({CTL_STICK, s, "", 0.5f, 0.5f, 0.10f});
                    g_selected = (int)cur()->ctls.size() - 1;
                    g_adding = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("\xE2\x89\xA1##add")) {
                std::lock_guard<std::mutex> l(g_lock);
                cur()->ctls.push_back({CTL_MENU, 0, "", 0.5f, 0.30f, 0.030f});
                g_selected = (int)cur()->ctls.size() - 1;
                g_adding = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("\xE2\x87\x84##add")) {
                std::lock_guard<std::mutex> l(g_lock);
                cur()->ctls.push_back({CTL_PROFILE, 0, "", 0.56f, 0.30f, 0.030f});
                g_selected = (int)cur()->ctls.size() - 1;
                g_adding = false;
            }
            ImGui::End();
        }
        // how to use it, for the first while
        static uint64_t t0 = SDL_GetTicksNS();
        if (SDL_GetTicksNS() - t0 < 8000000000ull) {
            const char *tip = "Drag a control to move it; the toolbar adds, sizes and deletes; Done saves.";
            float size = ImGui::GetFontSize();
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, tip);
            dl->AddRectFilled(ImVec2(W * 0.5f - ts.x * 0.5f - 10, H - 46 * ui_scale),
                              ImVec2(W * 0.5f + ts.x * 0.5f + 10, H - 12 * ui_scale), IM_COL32(8, 12, 20, 200), 6);
            dl->AddText(font, size, ImVec2(W * 0.5f - ts.x * 0.5f, H - 36 * ui_scale), IM_COL32(240, 244, 250, 220), tip);
        }
    }
}

// The settings menu's "Edit touch layout": it asks from its row, and overlay_frame closes the menu and
// starts the edit mode once the menu is down (a request seen while it draws would fight its windows).
void touch_request_edit(void) {
    std::lock_guard<std::mutex> l(g_lock);
    ensure_loaded();
    g_edit_request = true;
}
bool touch_take_edit_request(void) {
    std::lock_guard<std::mutex> l(g_lock);
    bool want = g_edit_request;
    g_edit_request = false;
    return want;
}
