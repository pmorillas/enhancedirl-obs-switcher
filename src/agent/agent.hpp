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

/* C interface of the agent, used by plugin-main.c. The protocol it speaks is in
 * docs/PROTOCOL.md. */

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the saved link (if any) and connects. Call from obs_module_post_load. */
void eirl_agent_start(void);
/* Disconnects and stops all threads. Call from obs_module_unload. */
void eirl_agent_stop(void);

/* Starts linking this OBS: asks for a code and opens the confirmation page in
 * the browser. Does nothing if a link is already in progress. */
void eirl_agent_begin_pairing(void);
/* Stops the link in progress (the code is dropped; nothing is linked). */
void eirl_agent_cancel_pairing(void);
/* Forgets the saved link and disconnects. */
void eirl_agent_unlink(void);

/* Human-readable state for logs and tests ("Linked to x (connected)"...).
 * Returns a newly allocated string: free it with bfree(). */
char *eirl_agent_describe(void);

/* Structured state for the UI (which translates it). */
struct eirl_agent_state {
	bool pairing;          /* a link is in progress */
	bool linked;           /* this OBS has a saved link */
	bool connected;        /* the connection with EnhancedIRL is up */
	char code[16];         /* pairing code while linking ("ABCD-EFGH") */
	int code_seconds_left; /* while a code is shown: seconds until it expires */
	char account[128];     /* account it is linked to, once known */
	char agent_name[128];  /* name of this OBS in My OBS, once known */
	char panel_url[512];   /* My OBS page (from the backend, or derived from the API) */
	/* Last problem, as a stable key: "", "revoked", "not_allowed", "replaced",
	 * "upgrade", "limit_reached", "email_not_verified", "expired", "network",
	 * "too_many", "unexpected". */
	char error[32];
};
void eirl_agent_get_state(struct eirl_agent_state *out);

/* Opens the confirmation page again while linking. */
void eirl_agent_reopen_browser(void);
/* Opens My OBS in the browser. */
void eirl_agent_open_panel(void);

/* Frontend events the agent needs (streaming started/stopped). */
void eirl_agent_on_streaming_started(void);
void eirl_agent_on_streaming_stopped(void);

#ifdef __cplusplus
}
#endif
