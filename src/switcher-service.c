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

/*
 * Stream service of the switcher. OBS picks the stream output from the service
 * (see GetStreamOutputType in the OBS frontend), so this service asks for our
 * own RTMP output, the one that can cut without saying goodbye.
 *
 * Everything else is delegated to OBS's own service ("rtmp_common"): the same
 * list of platforms and servers the user already knows, Twitch's "Auto" server,
 * and each platform's limits. A "Custom server" switch covers anything that is
 * not in that list (Restream, your own server...).
 */

#include <obs-module.h>
#include <util/dstr.h>
#include <string.h>
#include <ctype.h>

#include "switcher-service.h"

#define INNER_SERVICE "rtmp_common"
#define S_CUSTOM "eirl_custom"
#define S_CUSTOM_SERVER "eirl_custom_server"
#define S_CUSTOM_KEY "eirl_custom_key"
/* Custom mode only: the platform behind the custom server, when the one who
 * configures it knows (EnhancedIRL's Cloud OBS sets it from the user's channel). */
#define S_CUSTOM_PLATFORM "eirl_platform"

struct switcher_service {
	obs_service_t *inner;
	bool custom;
	char *custom_server;
	char *custom_key;
	char *platform;
};

static const char *switcher_service_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "EnhancedIRL Switcher";
}

/* "Twitch", "YouTube - RTMPS", "Kick"... → the slug the backend expects. */
static const char *platform_slug(const char *service_name)
{
	static const struct {
		const char *needle;
		const char *slug;
	} known[] = {
		{"twitch", "twitch"}, {"kick", "kick"},   {"youtube", "youtube"}, {"facebook", "facebook"},
		{"tiktok", "tiktok"}, {"trovo", "trovo"}, {"rumble", "rumble"},   {"x - ", "x"},
	};
	if (!service_name || !*service_name)
		return "other";

	struct dstr lower = {0};
	dstr_copy(&lower, service_name);
	dstr_to_lower(&lower);
	const char *slug = "other";
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (strstr(lower.array, known[i].needle)) {
			slug = known[i].slug;
			break;
		}
	}
	dstr_free(&lower);
	return slug;
}

/* Custom server → platform, for the ones whose ingest host says it. OBS's own
 * list has no Kick, so Kick users always come through here. */
static const char *platform_from_server(const char *url)
{
	if (!url || !*url)
		return NULL;
	const char *host = strstr(url, "://");
	host = host ? host + 3 : url;
	size_t len = strcspn(host, ":/?");
	char buf[256];
	if (len == 0 || len >= sizeof(buf))
		return NULL;
	for (size_t i = 0; i < len; i++)
		buf[i] = (char)tolower((unsigned char)host[i]);
	buf[len] = '\0';

#define ENDS_WITH(s) (len >= sizeof(s) - 1 && strcmp(buf + len - (sizeof(s) - 1), s) == 0)
	/* Kick's Amazon IVS ingest. Before Twitch: it is also *.contribute.live-video.net. */
	if (strcmp(buf, "fa723fc1b171.global-contribute.live-video.net") == 0)
		return "kick";
	if (ENDS_WITH(".contribute.live-video.net") || strcmp(buf, "live.twitch.tv") == 0 || ENDS_WITH(".twitch.tv"))
		return "twitch";
	if (ENDS_WITH(".youtube.com"))
		return "youtube";
	if (ENDS_WITH(".facebook.com"))
		return "facebook";
#undef ENDS_WITH
	return NULL;
}

static void switcher_service_update(void *data, obs_data_t *settings)
{
	struct switcher_service *s = data;

	s->custom = obs_data_get_bool(settings, S_CUSTOM);
	bfree(s->custom_server);
	bfree(s->custom_key);
	bfree(s->platform);
	s->custom_server = bstrdup(obs_data_get_string(settings, S_CUSTOM_SERVER));
	s->custom_key = bstrdup(obs_data_get_string(settings, S_CUSTOM_KEY));
	if (s->custom) {
		/* The one who configured it knows best (the Cloud OBS sets it); else the host. */
		const char *hint = obs_data_get_string(settings, S_CUSTOM_PLATFORM);
		const char *slug = hint && *hint ? platform_slug(hint) : NULL;
		if (!slug || strcmp(slug, "other") == 0)
			slug = platform_from_server(s->custom_server);
		s->platform = bstrdup(slug ? slug : "custom");
	} else {
		s->platform = bstrdup(platform_slug(obs_data_get_string(settings, "service")));
	}

	if (s->inner)
		obs_service_update(s->inner, settings);
}

static void switcher_service_destroy(void *data)
{
	struct switcher_service *s = data;

	obs_service_release(s->inner);
	bfree(s->custom_server);
	bfree(s->custom_key);
	bfree(s->platform);
	bfree(s);
}

static void *switcher_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);
	struct switcher_service *s = bzalloc(sizeof(struct switcher_service));

	s->inner = obs_service_create_private(INNER_SERVICE, "eirl-switcher-inner", settings);
	if (!s->inner)
		blog(LOG_WARNING, "[enhancedirl-obs-switcher] could not create the inner '%s' service", INNER_SERVICE);
	switcher_service_update(s, settings);
	return s;
}

static bool custom_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool custom = obs_data_get_bool(settings, S_CUSTOM);
	const char *platform_props[] = {"show_all", "service", "server", "key", "bwtest"};

	for (size_t i = 0; i < sizeof(platform_props) / sizeof(platform_props[0]); i++) {
		obs_property_t *pp = obs_properties_get(props, platform_props[i]);
		if (pp)
			obs_property_set_visible(pp, !custom);
	}
	obs_property_set_visible(obs_properties_get(props, S_CUSTOM_SERVER), custom);
	obs_property_set_visible(obs_properties_get(props, S_CUSTOM_KEY), custom);
	return true;
}

static obs_properties_t *switcher_service_properties(void *data)
{
	UNUSED_PARAMETER(data);

	/* OBS's own platform list, servers and key field, with their callbacks. */
	obs_properties_t *props = obs_get_service_properties(INNER_SERVICE);
	if (!props)
		props = obs_properties_create();

	obs_property_t *p = obs_properties_add_bool(props, S_CUSTOM, obs_module_text("CustomServer"));
	obs_property_set_modified_callback(p, custom_modified);
	obs_properties_add_text(props, S_CUSTOM_SERVER, obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, S_CUSTOM_KEY, obs_module_text("StreamKey"), OBS_TEXT_PASSWORD);
	return props;
}

static void switcher_service_defaults(obs_data_t *settings)
{
	/* Same defaults as OBS's service (e.g. Twitch selected). */
	obs_data_t *inner = obs_service_defaults(INNER_SERVICE);
	if (inner) {
		obs_data_apply(settings, inner);
		obs_data_release(inner);
	}
	obs_data_set_default_bool(settings, S_CUSTOM, false);
}

static const char *switcher_service_connect_info(void *data, uint32_t type)
{
	struct switcher_service *s = data;

	if (s->custom) {
		switch ((enum obs_service_connect_info)type) {
		case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
			return s->custom_server;
		case OBS_SERVICE_CONNECT_INFO_STREAM_ID:
			return s->custom_key;
		default:
			return NULL;
		}
	}
	return s->inner ? obs_service_get_connect_info(s->inner, type) : NULL;
}

static const char *switcher_service_url(void *data)
{
	return switcher_service_connect_info(data, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
}

static const char *switcher_service_key(void *data)
{
	return switcher_service_connect_info(data, OBS_SERVICE_CONNECT_INFO_STREAM_ID);
}

static const char *switcher_service_protocol(void *data)
{
	struct switcher_service *s = data;

	if (!s->custom)
		return s->inner ? obs_service_get_protocol(s->inner) : "RTMP";
	if (s->custom_server && strncmp(s->custom_server, "rtmps://", 8) == 0)
		return "RTMPS";
	return "RTMP";
}

static const char *switcher_service_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return EIRL_RTMP_OUTPUT_ID;
}

static bool switcher_service_can_try_to_connect(void *data)
{
	struct switcher_service *s = data;

	if (s->custom)
		return s->custom_server && *s->custom_server && s->custom_key && *s->custom_key;
	return s->inner && obs_service_can_try_to_connect(s->inner);
}

static void switcher_service_apply_encoder_settings(void *data, obs_data_t *video, obs_data_t *audio)
{
	struct switcher_service *s = data;

	if (!s->custom && s->inner)
		obs_service_apply_encoder_settings(s->inner, video, audio);
}

static void switcher_service_supported_resolutions(void *data, struct obs_service_resolution **resolutions,
						   size_t *count)
{
	struct switcher_service *s = data;

	*resolutions = NULL;
	*count = 0;
	if (!s->custom && s->inner)
		obs_service_get_supported_resolutions(s->inner, resolutions, count);
}

static void switcher_service_max_fps(void *data, int *fps)
{
	struct switcher_service *s = data;

	*fps = 0;
	if (!s->custom && s->inner)
		obs_service_get_max_fps(s->inner, fps);
}

static void switcher_service_max_bitrate(void *data, int *video_bitrate, int *audio_bitrate)
{
	struct switcher_service *s = data;

	*video_bitrate = 0;
	*audio_bitrate = 0;
	if (!s->custom && s->inner)
		obs_service_get_max_bitrate(s->inner, video_bitrate, audio_bitrate);
}

static const char **switcher_service_video_codecs(void *data)
{
	struct switcher_service *s = data;

	return (!s->custom && s->inner) ? obs_service_get_supported_video_codecs(s->inner) : NULL;
}

static const char **switcher_service_audio_codecs(void *data)
{
	struct switcher_service *s = data;

	return (!s->custom && s->inner) ? obs_service_get_supported_audio_codecs(s->inner) : NULL;
}

const char *switcher_service_platform(obs_service_t *service)
{
	if (!service || strcmp(obs_service_get_id(service), EIRL_SWITCHER_SERVICE_ID) != 0)
		return NULL;
	struct switcher_service *s = obs_obj_get_data(service);
	return s ? s->platform : NULL;
}

obs_data_t *switcher_service_settings_from(obs_service_t *current, const char *platform_hint)
{
	obs_data_t *from = obs_service_get_settings(current);
	obs_data_t *to = obs_data_create();

	if (strcmp(obs_service_get_id(current), INNER_SERVICE) == 0) {
		obs_data_apply(to, from); /* service, server, key... as they are */
		obs_data_set_bool(to, S_CUSTOM, false);
	} else {
		/* "Custom..." or anything else: our custom server mode. */
		obs_data_set_bool(to, S_CUSTOM, true);
		obs_data_set_string(to, S_CUSTOM_SERVER, obs_data_get_string(from, "server"));
		obs_data_set_string(to, S_CUSTOM_KEY, obs_data_get_string(from, "key"));
		if (platform_hint && *platform_hint)
			obs_data_set_string(to, S_CUSTOM_PLATFORM, platform_hint);
	}
	obs_data_release(from);
	return to;
}

struct obs_service_info eirl_switcher_service_info = {
	.id = EIRL_SWITCHER_SERVICE_ID,
	.get_name = switcher_service_name,
	.create = switcher_service_create,
	.destroy = switcher_service_destroy,
	.update = switcher_service_update,
	.get_defaults = switcher_service_defaults,
	.get_properties = switcher_service_properties,
	.get_protocol = switcher_service_protocol,
	.get_url = switcher_service_url,
	.get_key = switcher_service_key,
	.get_connect_info = switcher_service_connect_info,
	.get_output_type = switcher_service_output_type,
	.can_try_to_connect = switcher_service_can_try_to_connect,
	.apply_encoder_settings = switcher_service_apply_encoder_settings,
	.get_supported_resolutions = switcher_service_supported_resolutions,
	.get_max_fps = switcher_service_max_fps,
	.get_max_bitrate = switcher_service_max_bitrate,
	.get_supported_video_codecs = switcher_service_video_codecs,
	.get_supported_audio_codecs = switcher_service_audio_codecs,
};
