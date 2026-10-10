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

#ifdef __cplusplus
extern "C" {
#endif

/* The next start of our output keeps its previous takeover priority instead of
 * taking a new one (see `ivs_priority` in rtmp-stream.h). The agent calls it
 * before the dead-man restart: an OBS that was replaced must not take the
 * stream back from the one that replaced it. */
void eirl_rtmp_keep_priority_once(void);

/* YouTube's two ingests. The next fresh start of our output goes to this one
 * (the switch puts the new OBS on the ingest the live one is NOT using: YouTube
 * then moves over without a gap when the live one stops). Kept on reconnects. */
enum eirl_ingest { EIRL_INGEST_ASIS = 0, EIRL_INGEST_PRIMARY = 1, EIRL_INGEST_BACKUP = 2 };
void eirl_rtmp_set_next_ingest(enum eirl_ingest ingest);

/* The server URL our streaming output is really using (after the rewrite
 * above), or NULL if `output` is not ours or is not active. */
const char *eirl_rtmp_effective_url(obs_output_t *output);

#ifdef __cplusplus
}
#endif
