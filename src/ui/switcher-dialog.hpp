/*
EnhancedIRL OBS Switcher
Copyright (C) 2026 EnhancedIRL

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shows the EnhancedIRL Switcher window (Tools menu). Call on the UI thread. */
void eirl_ui_show_dialog(void);

/* False in builds without a window (ENABLE_QT=OFF): nobody could see or change
 * the remote control switch there, so it stays off. */
bool eirl_ui_has_window(void);

/* Tells the user that the stream moved from this OBS to `to_name` (a switch
 * asked for from My OBS at `at_ms`, milliseconds since the epoch). A newer
 * notice replaces an older one. Callable from any thread. */
void eirl_ui_show_handoff(const char *to_name, long long at_ms);

#ifdef __cplusplus
}
#endif
