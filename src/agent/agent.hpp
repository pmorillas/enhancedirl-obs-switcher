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

#ifdef EIRL_TEST_HOOKS
/* Human-readable state for tests, with the pairing code and the account
 * ("Linked to x (connected)"...). Test builds only: never log it.
 * Returns a newly allocated string: free it with bfree(). */
char *eirl_agent_describe(void);
#endif
/* Human-readable state safe for the OBS log: no pairing code, no account.
 * Returns a newly allocated string: free it with bfree(). */
char *eirl_agent_describe_for_log(void);

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
	/* Remote control: this OBS supports it (not a Cloud OBS, OBS WebSocket 5.5+),
	 * the user allowed it, and a request ran in the last few seconds. */
	bool control_supported;
	bool control_enabled;
	bool control_active;
};
void eirl_agent_get_state(struct eirl_agent_state *out);

/* Opens the confirmation page again while linking. */
void eirl_agent_reopen_browser(void);
/* Opens My OBS in the browser. */
void eirl_agent_open_panel(void);

/* Frontend events the agent needs (streaming started/stopped, OBS closing). */
void eirl_agent_on_streaming_started(void);
void eirl_agent_on_streaming_stopped(void);
void eirl_agent_on_obs_exit(void);

/* Allows or forbids remote control on this OBS (saved with the link; off for a
 * new link). Does nothing while not linked or in a Cloud OBS. */
void eirl_agent_set_control_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
