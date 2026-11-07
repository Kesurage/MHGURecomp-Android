// Touch controls for phones and tablets (runtime/touch.cpp): a layer of on-screen controls between
// SDL's touch events and the Switch controller the game reads (runtime/hle/input.c, through
// runtime/host_input.c). Buttons, two floating stick zones, a menu button and a profile switcher;
// every one of them movable, sizeable and deletable, in profiles of their own (one per weapon, say),
// saved to touch.txt beside the settings. Edited from the settings menu's Android tab ("Edit touch
// layout": drag to move, the toolbar to add, size and delete) or with a finger alone.
//
// The drawn overlay needs the ImGui overlay's frame (runtime/overlay.cpp, touch_draw); its state is
// filled by touch_event (the window's thread) and merged into the controller by touch_update
// (host_input_update). touch_enabled() gates it: [Android] TouchControls in the settings.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Touch events: true when the touch layer took the event (a control was touched, or the edit mode is on)
bool touch_event(const SDL_Event *e);

// The held buttons and stick tilts into the game's controller state (up is positive y, as host_input's)
void touch_update(uint64_t *buttons, int *lx, int *ly, int *rx, int *ry);

// Draw the controls (and the edit mode's toolbar) on the ImGui overlay's foreground; scale is the
// settings menu's UI scale. No-ops when the layer is off or hidden.
void touch_draw(float scale);

// Anything to draw this frame (the overlay's "nothing to draw" fast path asks first)
bool touch_draws(void);

// The settings: [Android] TouchControls. Profiles live in touch.txt; the menu edits the settings,
// the overlay's edit mode edits the layout.
bool touch_enabled(void);
void touch_set_editing(bool on);
bool touch_editing(void);

// Profiles (the Android tab's rows and the edit mode's toolbar)
int touch_profile_count(void);
const char *touch_profile_name(int index);
int touch_profile_current(void);
void touch_profile_select(int index);   // -1: next one
void touch_profile_add(const char *name);  // copy of the current one, "name"
void touch_profile_delete(int index);
void touch_reset_layout(void);          // back to the default layout, every profile gone but Default

// The settings menu's "Edit touch layout": it asks, and the overlay's frame closes the menu and starts
// the edit mode when it is next built (they must not draw over each other)
void touch_request_edit(void);
bool touch_take_edit_request(void);

#ifdef __cplusplus
}
#endif
