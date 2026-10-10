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

#include <obs-module.h>

#define EIRL_SWITCHER_SERVICE_ID "eirl_switcher_service"
#define EIRL_RTMP_OUTPUT_ID "eirl_rtmp_output"

extern struct obs_service_info eirl_switcher_service_info;

/* Platform slug ("twitch", "kick", "custom"...) of a switcher service, or NULL
 * if the service is not ours. */
const char *switcher_service_platform(obs_service_t *service);

/* The platform's name as people know it ("Twitch", "YouTube - RTMPS"...), or NULL
 * for a custom server of an unknown platform and for services that are not ours. */
const char *switcher_service_display_name(obs_service_t *service);

/* Settings of another service rewritten for the switcher's service: OBS's own
 * platform list ("rtmp_common") as it is, anything else as a custom server.
 * `platform_hint` (may be NULL) names the platform behind a custom server.
 * The key only moves from one settings object to another. Release the result
 * with obs_data_release(). */
obs_data_t *switcher_service_settings_from(obs_service_t *current, const char *platform_hint);
