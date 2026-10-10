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

#include "obs-bridge.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <mbedtls/md.h>

#include <cctype>
#include <cstring>

extern "C" {
#include "../switcher-service.h"
#include "../rtmp-output/eirl-rtmp.h"
}

namespace eirl {

std::string url_host(const std::string &url)
{
	size_t start = url.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t end = url.find_first_of(":/?#", start);
	std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	for (char &c : host)
		c = (char)std::tolower((unsigned char)c);
	return host;
}

std::string twitch_broadcaster_id(const std::string &key)
{
	if (key.rfind("live_", 0) != 0)
		return {};
	size_t end = key.find('_', 5);
	if (end == std::string::npos || end == 5)
		return {};
	std::string id = key.substr(5, end - 5);
	for (char c : id)
		if (!std::isdigit((unsigned char)c))
			return {};
	return id;
}

std::string hmac_sha256_hex(const std::string &salt, const std::string &data)
{
	unsigned char mac[32];
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md || mbedtls_md_hmac(md, (const unsigned char *)salt.data(), salt.size(),
				   (const unsigned char *)data.data(), data.size(), mac) != 0)
		return {};
	static const char *hex = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for (unsigned char b : mac) {
		out += hex[b >> 4];
		out += hex[b & 0xf];
	}
	return out;
}

static std::optional<EncoderInfo> read_encoder(obs_output_t *out)
{
	if (!out)
		return std::nullopt;
	obs_encoder_t *enc = obs_output_get_video_encoder(out);
	if (!enc)
		return std::nullopt;
	EncoderInfo info;
	const char *codec = obs_encoder_get_codec(enc);
	info.codec = codec ? codec : "";
	obs_data_t *settings = obs_encoder_get_settings(enc);
	if (settings) {
		info.bitrateKbps = (int)obs_data_get_int(settings, "bitrate");
		obs_data_release(settings);
	}
	if (info.codec.empty() || info.bitrateKbps <= 0)
		return std::nullopt;
	return info;
}

ObsState read_obs_state(const std::string &salt)
{
	ObsState s;
	s.streaming = obs_frontend_streaming_active();

	std::string effective_url;
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (out) {
		s.reconnecting = obs_output_reconnecting(out);
		s.encoder = read_encoder(out);
		/* The server really in use (YouTube: the ingest the switch chose). */
		if (const char *eff = eirl_rtmp_effective_url(out))
			effective_url = eff;
		obs_output_release(out);
	}

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_den > 0) {
		s.video = VideoInfo{(int)ovi.output_width, (int)ovi.output_height,
				    (int)((ovi.fps_num + ovi.fps_den / 2) / ovi.fps_den)};
	}

	obs_service_t *service = obs_frontend_get_streaming_service();
	if (service) {
		const char *platform = switcher_service_platform(service);
		if (platform) {
			s.platform = platform;
			s.serviceReady = obs_service_can_try_to_connect(service);
			const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
			const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_ID);
			if (!effective_url.empty())
				s.serverHost = url_host(effective_url);
			else if (url && *url)
				s.serverHost = url_host(url);
			if (key && *key) {
				if (!salt.empty())
					s.keyFingerprint = hmac_sha256_hex(salt, key);
				if (s.platform == "twitch")
					s.twitchBroadcasterId = twitch_broadcaster_id(key);
			}
		}
		/* obs_frontend_get_streaming_service does not add a reference. */
	}
	return s;
}

void start_streaming()
{
	if (!obs_frontend_streaming_active())
		obs_frontend_streaming_start();
}

bool cut_streaming()
{
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return false;
	bool ok = false;
	if (strcmp(obs_output_get_id(out), EIRL_RTMP_OUTPUT_ID) == 0 && obs_output_active(out)) {
		calldata_t cd = {0};
		ok = proc_handler_call(obs_output_get_proc_handler(out), "eirl_cut", &cd);
		calldata_free(&cd);
	}
	obs_output_release(out);
	return ok;
}

} // namespace eirl
