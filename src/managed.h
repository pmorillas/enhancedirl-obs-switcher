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

/*
 * Managed build (EnhancedIRL's Cloud OBS image sets this to 1 with a patch).
 *
 * In a managed build the plugin only loads when the server has the switcher
 * enabled (EIRL_SWITCHER_ENABLED=1), and its link comes from the environment
 * (EIRL_SWITCHER_AGENT_TOKEN, EIRL_SWITCHER_FINGERPRINT_SALT, EIRL_SWITCHER_API)
 * instead of the pairing flow: the user never links a Cloud OBS by hand.
 */
#define EIRL_MANAGED_BUILD 0
