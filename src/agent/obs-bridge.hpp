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

#include <optional>
#include <string>

/* Everything the agent reads from or does to OBS. Thread-safe: OBS's frontend API
 * queues the calls to the UI thread. */
namespace eirl {

struct VideoInfo {
	int width = 0;
	int height = 0;
	int fps = 0;
};

struct EncoderInfo {
	std::string codec;
	int bitrateKbps = 0;
};

struct ObsState {
	bool streaming = false;
	bool reconnecting = false;
	/* The stream service is the switcher's and has a server and a key. */
	bool serviceReady = false;
	std::string platform;     /* "" if the service is not ours */
	std::string serverHost;   /* host only, never the path */
	std::string keyFingerprint; /* hex HMAC-SHA256(salt, key), "" without key */
	std::string twitchBroadcasterId;
	std::optional<VideoInfo> video;
	std::optional<EncoderInfo> encoder;
};

/* Reads the current state. `salt` keys the fingerprint of the stream key. */
ObsState read_obs_state(const std::string &salt);

/* Starts the main stream with the current settings (returns immediately). */
void start_streaming();

/* Cuts the main stream without the RTMP goodbye. False if it is not streaming
 * through the switcher's output. */
bool cut_streaming();

/* Host of a stream URL: "rtmps://a.b.c:443/app/x" → "a.b.c". */
std::string url_host(const std::string &url);

/* Public broadcaster id at the start of every Twitch key ("live_<id>_..."). */
std::string twitch_broadcaster_id(const std::string &key);

/* hex(HMAC-SHA256(key = salt bytes, message = data bytes)). */
std::string hmac_sha256_hex(const std::string &salt, const std::string &data);

} // namespace eirl
