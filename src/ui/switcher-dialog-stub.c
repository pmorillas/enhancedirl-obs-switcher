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

/* Builds without Qt (ENABLE_QT=OFF): no window; the state goes to the log. */

#include <obs-module.h>
#include <plugin-support.h>

#include "switcher-dialog.hpp"
#include "../agent/agent.hpp"

void eirl_ui_show_dialog(void)
{
	/* Only test builds put the pairing code and the account in the log. */
#ifdef EIRL_TEST_HOOKS
	char *text = eirl_agent_describe();
#else
	char *text = eirl_agent_describe_for_log();
#endif
	obs_log(LOG_INFO, "status: %s", text);
	bfree(text);
}

bool eirl_ui_has_window(void)
{
	return false;
}

void eirl_ui_show_handoff(const char *to_name, long long at_ms)
{
	UNUSED_PARAMETER(at_ms);
	obs_log(LOG_INFO, "the stream moved to \"%s\" (asked for from My OBS)", to_name ? to_name : "");
}
