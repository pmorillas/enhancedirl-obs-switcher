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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "switcher-service.h"
#include "agent/agent.hpp"
#include "ui/switcher-dialog.hpp"
#include "managed.h"

#include <stdlib.h>

/* Test hooks over obs-websocket (vendor "eirl-switcher") are only compiled into
 * test builds (CMake option ENABLE_TEST_HOOKS, off by default). Release builds
 * do not register the vendor at all. */
#ifdef EIRL_TEST_HOOKS
#include "obs-websocket-api.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_output_info eirl_rtmp_output_info;

#ifdef EIRL_TEST_HOOKS
static obs_websocket_vendor vendor;

/* Cuts the main stream without saying goodbye to the platform. It only acts if
 * the active output is ours; with any other output it does nothing. */
static bool switcher_cut(void)
{
	obs_output_t *out = obs_frontend_get_streaming_output();
	bool ok = false;

	if (!out)
		return false;
	if (strcmp(obs_output_get_id(out), EIRL_RTMP_OUTPUT_ID) == 0 && obs_output_active(out)) {
		calldata_t cd = {0};
		ok = proc_handler_call(obs_output_get_proc_handler(out), "eirl_cut", &cd);
		calldata_free(&cd);
	}
	obs_output_release(out);
	return ok;
}
#endif

static void tools_open(void *unused)
{
	UNUSED_PARAMETER(unused);
	eirl_ui_show_dialog();
}

#if EIRL_MANAGED_BUILD
/* Cloud OBS: the image writes the destination as OBS's custom server, the same
 * file for the whole fleet whether this plugin loads or not (an image without
 * the plugin keeps working). Here it moves into our service, so the stream goes
 * out through the output that can cut. EIRL_SWITCHER_PLATFORM says which
 * platform is behind that server. Never while streaming: it waits for the stop. */
static void managed_adopt_service(void)
{
	if (obs_frontend_streaming_active())
		return;
	obs_service_t *current = obs_frontend_get_streaming_service();
	if (!current || switcher_service_platform(current))
		return;

	obs_data_t *settings = switcher_service_settings_from(current, getenv("EIRL_SWITCHER_PLATFORM"));
	obs_service_t *ours = obs_service_create(EIRL_SWITCHER_SERVICE_ID, "default_service", settings, NULL);
	obs_data_release(settings);
	if (!ours) {
		obs_log(LOG_WARNING, "managed: could not create the switcher service");
		return;
	}
	obs_frontend_set_streaming_service(ours);
	obs_frontend_save_streaming_service();
	obs_service_release(ours);
	obs_log(LOG_INFO, "managed: stream service moved to EnhancedIRL Switcher");
}
#endif

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		eirl_agent_on_streaming_started();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		eirl_agent_on_obs_exit();
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		eirl_agent_on_streaming_stopped();
#if EIRL_MANAGED_BUILD
		managed_adopt_service();
#endif
	}
#if EIRL_MANAGED_BUILD
	else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		managed_adopt_service();
	}
#endif
}

#ifdef EIRL_TEST_HOOKS
static void vendor_cut(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	bool ok = switcher_cut();
	obs_log(LOG_INFO, "test cut from obs-websocket: %s", ok ? "done" : "not applicable");
	obs_data_set_bool(response, "ok", ok);
}

/* Test hooks over obs-websocket (vendor "eirl-switcher"): the same actions as the
 * Tools menu, so automated tests do not need to click. */
static void vendor_link(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	eirl_agent_begin_pairing();
	obs_data_set_bool(response, "ok", true);
}

static void vendor_unlink(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	eirl_agent_unlink();
	obs_data_set_bool(response, "ok", true);
}

static void show_dialog_task(void *unused)
{
	UNUSED_PARAMETER(unused);
	eirl_ui_show_dialog();
}

static void vendor_ui(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	obs_queue_task(OBS_TASK_UI, show_dialog_task, NULL, false);
	obs_data_set_bool(response, "ok", true);
}

/* Turns remote control on or off, as the switch in the window does:
 * { "enabled": true|false }. */
static void vendor_control(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	eirl_agent_set_control_enabled(obs_data_get_bool(request, "enabled"));
	struct eirl_agent_state s;
	eirl_agent_get_state(&s);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_bool(response, "controlEnabled", s.control_enabled);
	obs_data_set_bool(response, "controlSupported", s.control_supported);
}

static void vendor_status(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	char *text = eirl_agent_describe();
	obs_data_set_string(response, "status", text);
	bfree(text);
}
#endif

bool obs_module_load(void)
{
#if EIRL_MANAGED_BUILD
	/* Cloud OBS image: one image for the whole fleet, so the plugin only loads on
	 * the servers where EnhancedIRL enabled it (same rule as the other plugins). */
	const char *enabled = getenv("EIRL_SWITCHER_ENABLED");
	if (!enabled || strcmp(enabled, "1") != 0) {
		obs_log(LOG_INFO, "switcher not enabled on this server; not loading");
		return false;
	}
#endif
	obs_register_output(&eirl_rtmp_output_info);
	obs_register_service(&eirl_switcher_service_info);
	obs_frontend_add_tools_menu_item(obs_module_text("Menu.Switcher"), tools_open, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	eirl_agent_start();
#ifdef EIRL_TEST_HOOKS
	vendor = obs_websocket_register_vendor("eirl-switcher");
	if (vendor) {
		obs_websocket_vendor_register_request(vendor, "cut", vendor_cut, NULL);
		obs_websocket_vendor_register_request(vendor, "link", vendor_link, NULL);
		obs_websocket_vendor_register_request(vendor, "unlink", vendor_unlink, NULL);
		obs_websocket_vendor_register_request(vendor, "status", vendor_status, NULL);
		obs_websocket_vendor_register_request(vendor, "ui", vendor_ui, NULL);
		obs_websocket_vendor_register_request(vendor, "control", vendor_control, NULL);
		obs_log(LOG_INFO, "test build: obs-websocket test hooks registered");
	} else
		obs_log(LOG_INFO, "obs-websocket not available: test cut only from the Tools menu");
#endif
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	eirl_agent_stop();
	obs_log(LOG_INFO, "plugin unloaded");
}
