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

#include "remote-control.hpp"
#include "json-text.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <vector>

/* obs-websocket's plugin API (header-only: it talks to obs-websocket through
 * libobs' proc handlers, so the plugin does not link against it). */
#include "../obs-websocket-api.h"

#define LOG(level, fmt, ...) blog(level, "[enhancedirl-obs-switcher] " fmt, ##__VA_ARGS__)

namespace eirl {

namespace {

/* obs-websocket's RequestStatus codes we answer with ourselves. */
constexpr unsigned SUCCESS = 100;
constexpr unsigned UNKNOWN_REQUEST_TYPE = 204;
constexpr unsigned GENERIC_ERROR = 205;
constexpr unsigned NOT_READY = 207;
constexpr unsigned INVALID_REQUEST_FIELD = 400;
constexpr unsigned INVALID_REQUEST_FIELD_TYPE = 401;
constexpr unsigned REQUEST_FIELD_OUT_OF_RANGE = 402;
constexpr unsigned REQUEST_PROCESSING_FAILED = 702;

constexpr const char *NOT_ALLOWED = "not allowed by EnhancedIRL Switcher";

/* The event callback API arrived in obs-websocket 5.5 (plugin API v3). */
constexpr unsigned MIN_API_VERSION = 3;

/* Limits (docs/PROTOCOL.md §3.4). */
constexpr size_t MAX_QUEUE = 64;
constexpr long long REQUEST_TTL_MS = 10000; /* the backend gives up after 10 s */
constexpr size_t MAX_RESPONSE_BYTES = 3000000;
constexpr size_t MAX_EVENT_BYTES = 60 * 1024; /* other frames stay under 64 KiB */
constexpr size_t MAX_STRING_FIELD = 512;
constexpr size_t MAX_SUBSCRIBED = 64;
constexpr long long MAX_SCREENSHOT_WIDTH = 1280;
/* Sizes beyond this are nonsense (obs-websocket itself stops at 4096). */
constexpr long long MAX_SCREENSHOT_SIDE = 16384;
constexpr double EVENTS_PER_SECOND = 50.0;
constexpr double EVENT_BURST = 200.0;
/* InputVolumeChanged only gets the bucket above this level: when events pile
 * up, volume changes go first and scene/stream/record changes still get through. */
constexpr double VOLUME_RESERVE = 50.0;

long long now_ms()
{
	return (long long)(os_gettime_ns() / 1000000);
}

enum class Kind { String, Bool, Number, Integer };

struct Field {
	const char *name;
	Kind kind;
};

/* Fields each request may carry (obs-websocket's names). Anything else in
 * requestData is dropped before the request runs. Lists end with a null name. */
const Field NO_FIELDS[] = {{nullptr, Kind::String}};
const Field SCENE_FIELDS[] = {{"sceneName", Kind::String}, {"sceneUuid", Kind::String}, {nullptr, Kind::String}};
const Field STUDIO_FIELDS[] = {{"studioModeEnabled", Kind::Bool}, {nullptr, Kind::String}};
const Field INPUT_LIST_FIELDS[] = {{"inputKind", Kind::String}, {nullptr, Kind::String}};
const Field INPUT_FIELDS[] = {{"inputName", Kind::String}, {"inputUuid", Kind::String}, {nullptr, Kind::String}};
const Field VOLUME_FIELDS[] = {{"inputName", Kind::String},
			       {"inputUuid", Kind::String},
			       {"inputVolumeMul", Kind::Number},
			       {"inputVolumeDb", Kind::Number},
			       {nullptr, Kind::String}};
const Field MUTE_FIELDS[] = {{"inputName", Kind::String},
			     {"inputUuid", Kind::String},
			     {"inputMuted", Kind::Bool},
			     {nullptr, Kind::String}};
const Field SCREENSHOT_FIELDS[] = {{"sourceName", Kind::String},   {"sourceUuid", Kind::String},
				   {"imageFormat", Kind::String},  {"imageWidth", Kind::Integer},
				   {"imageHeight", Kind::Integer}, {"imageCompressionQuality", Kind::Integer},
				   {nullptr, Kind::String}};

struct Allowed {
	const char *type;
	const Field *fields;
};

/* THE closed list of requests (docs/PROTOCOL.md §3.5). Nothing else runs. */
const Allowed REQUESTS[] = {
	{"GetVersion", NO_FIELDS},
	{"GetStats", NO_FIELDS},
	{"GetSceneList", NO_FIELDS},
	{"GetCurrentProgramScene", NO_FIELDS},
	{"SetCurrentProgramScene", SCENE_FIELDS},
	{"GetCurrentPreviewScene", NO_FIELDS},
	{"SetCurrentPreviewScene", SCENE_FIELDS},
	{"GetStudioModeEnabled", NO_FIELDS},
	{"SetStudioModeEnabled", STUDIO_FIELDS},
	{"TriggerStudioModeTransition", NO_FIELDS},
	{"GetCurrentSceneTransition", NO_FIELDS},
	{"GetCurrentSceneTransitionCursor", NO_FIELDS},
	{"GetStreamStatus", NO_FIELDS},
	{"StartStream", NO_FIELDS},
	{"StopStream", NO_FIELDS},
	{"GetRecordStatus", NO_FIELDS},
	{"StartRecord", NO_FIELDS},
	{"StopRecord", NO_FIELDS},
	{"PauseRecord", NO_FIELDS},
	{"ResumeRecord", NO_FIELDS},
	{"GetInputList", INPUT_LIST_FIELDS},
	{"GetInputVolume", INPUT_FIELDS},
	{"SetInputVolume", VOLUME_FIELDS},
	{"GetInputMute", INPUT_FIELDS},
	{"SetInputMute", MUTE_FIELDS},
	{"ToggleInputMute", INPUT_FIELDS},
	{"GetSourceScreenshot", SCREENSHOT_FIELDS},
};

/* THE closed list of events (docs/PROTOCOL.md §3.5). */
const char *const EVENTS[] = {
	"CurrentProgramSceneChanged", "CurrentPreviewSceneChanged", "SceneListChanged",   "StudioModeStateChanged",
	"StreamStateChanged",         "RecordStateChanged",         "InputVolumeChanged", "InputMuteStateChanged",
};

const Allowed *find_request(const std::string &type)
{
	for (const Allowed &a : REQUESTS)
		if (type == a.type)
			return &a;
	return nullptr;
}

bool allowed_event(const char *type)
{
	for (const char *e : EVENTS)
		if (std::strcmp(type, e) == 0)
			return true;
	return false;
}

const char *kind_name(Kind k)
{
	switch (k) {
	case Kind::String:
		return "a string";
	case Kind::Bool:
		return "a boolean";
	case Kind::Number:
		return "a number";
	default:
		return "an integer";
	}
}

/* Copies the fields `rule` allows from `raw`, checking their types. Returns the
 * clean requestData (null if it has no fields), or sets `code`/`comment`. */
obs_data_t *checked_fields(const Allowed &rule, obs_data_t *raw, unsigned &code, std::string &comment)
{
	code = SUCCESS;
	if (!raw)
		return nullptr;
	obs_data_t *out = nullptr;
	for (const Field *f = rule.fields; f->name; f++) {
		obs_data_item_t *item = obs_data_item_byname(raw, f->name);
		if (!item)
			continue;
		const enum obs_data_type type = obs_data_item_gettype(item);
		bool ok = false;
		switch (f->kind) {
		case Kind::String:
			ok = type == OBS_DATA_STRING;
			break;
		case Kind::Bool:
			ok = type == OBS_DATA_BOOLEAN;
			break;
		case Kind::Number:
			ok = type == OBS_DATA_NUMBER;
			break;
		case Kind::Integer:
			ok = type == OBS_DATA_NUMBER && obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
			break;
		}
		if (!ok) {
			obs_data_item_release(&item);
			code = INVALID_REQUEST_FIELD_TYPE;
			comment = std::string("The field `") + f->name + "` must be " + kind_name(f->kind) + ".";
			break;
		}
		if (!out)
			out = obs_data_create();
		switch (f->kind) {
		case Kind::String: {
			const char *s = obs_data_item_get_string(item);
			if (std::strlen(s) > MAX_STRING_FIELD) {
				code = INVALID_REQUEST_FIELD;
				comment = std::string("The field `") + f->name + "` is too long.";
			} else {
				obs_data_set_string(out, f->name, s);
			}
			break;
		}
		case Kind::Bool:
			obs_data_set_bool(out, f->name, obs_data_item_get_bool(item));
			break;
		case Kind::Number:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				obs_data_set_int(out, f->name, obs_data_item_get_int(item));
			else
				obs_data_set_double(out, f->name, obs_data_item_get_double(item));
			break;
		case Kind::Integer:
			obs_data_set_int(out, f->name, obs_data_item_get_int(item));
			break;
		}
		obs_data_item_release(&item);
		if (code != SUCCESS)
			break;
	}
	/* Screenshots: only JPG or PNG, sizes in a sane range (the width is capped
	 * when it runs). */
	if (code == SUCCESS && out && std::strcmp(rule.type, "GetSourceScreenshot") == 0) {
		const char *format = obs_data_get_string(out, "imageFormat");
		if (obs_data_has_user_value(out, "imageFormat") && std::strcmp(format, "jpg") != 0 &&
		    std::strcmp(format, "jpeg") != 0 && std::strcmp(format, "png") != 0) {
			code = INVALID_REQUEST_FIELD;
			comment = "The field `imageFormat` must be jpg or png.";
		}
		for (const char *side : {"imageWidth", "imageHeight"}) {
			const long long v = obs_data_get_int(out, side);
			if (code == SUCCESS && obs_data_has_user_value(out, side) &&
			    (v < 1 || v > MAX_SCREENSHOT_SIDE)) {
				code = REQUEST_FIELD_OUT_OF_RANGE;
				comment = std::string("The field `") + side + "` is out of range.";
			}
		}
	}
	if (code != SUCCESS && out) {
		obs_data_release(out);
		out = nullptr;
	}
	return out;
}

/* At most MAX_SCREENSHOT_WIDTH pixels wide, whatever was asked for: with no
 * width, obs-websocket uses the source's own size (4K canvases exist), and with
 * only a height it keeps the aspect ratio, so the source size is needed too. */
void cap_screenshot(obs_data_t *data)
{
	if (!data)
		return;
	const long long w = obs_data_has_user_value(data, "imageWidth") ? obs_data_get_int(data, "imageWidth") : 0;
	const long long h = obs_data_has_user_value(data, "imageHeight") ? obs_data_get_int(data, "imageHeight") : 0;
	long long base_w = 0, base_h = 0;
	/* The same lookup order as obs-websocket: the name first, then the UUID. */
	const char *name = obs_data_get_string(data, "sourceName");
	const char *uuid = obs_data_get_string(data, "sourceUuid");
	obs_source_t *source = *name ? obs_get_source_by_name(name) : (*uuid ? obs_get_source_by_uuid(uuid) : nullptr);
	if (source) {
		base_w = obs_source_get_base_width(source);
		base_h = obs_source_get_base_height(source);
		obs_source_release(source);
	}
	const long long width = w > 0 ? w : (h > 0 && base_w > 0 && base_h > 0 ? h * base_w / base_h : base_w);
	if (width <= MAX_SCREENSHOT_WIDTH)
		return;
	if (w > 0 && h > 0)
		obs_data_set_int(data, "imageHeight", std::max(8LL, h * MAX_SCREENSHOT_WIDTH / w));
	else
		obs_data_erase(data, "imageHeight");
	obs_data_set_int(data, "imageWidth", MAX_SCREENSHOT_WIDTH);
}

/* Replaces the array of strings under `key` in obs-websocket's JSON with
 * `items`. Only for arrays of plain names (no brackets inside), as in GetVersion. */
std::string replace_name_array(const std::string &json_text, const char *key, const std::vector<std::string> &items)
{
	const std::string needle = std::string("\"") + key + "\":[";
	const size_t start = json_text.find(needle);
	if (start == std::string::npos)
		return json_text;
	const size_t open = start + needle.size();
	const size_t close = json_text.find(']', open);
	if (close == std::string::npos)
		return json_text;
	std::string list;
	for (const std::string &item : items) {
		if (!list.empty())
			list += ',';
		list += json::quote(item);
	}
	return json_text.substr(0, open) + list + json_text.substr(close);
}

/* GetVersion lists what this OBS can do through the plugin, not everything
 * obs-websocket knows: the closed list, and only JPG and PNG screenshots. */
std::string restrict_version_info(const char *data)
{
	std::vector<std::string> requests;
	for (const Allowed &a : REQUESTS)
		requests.push_back(a.type);
	std::string out = replace_name_array(data, "availableRequests", requests);
	return replace_name_array(out, "supportedImageFormats", {"jpeg", "jpg", "png"});
}

std::string response_frame(const std::string &id, unsigned code, const std::string &comment, const char *data)
{
	std::string out;
	const size_t data_len = data ? std::strlen(data) : 0;
	out.reserve(160 + id.size() + comment.size() + data_len);
	out += "{\"type\":\"obs_response\",\"id\":";
	out += json::quote(id);
	out += code == SUCCESS ? ",\"ok\":true" : ",\"ok\":false";
	out += ",\"code\":";
	out += std::to_string(code);
	out += ",\"comment\":";
	out += comment.empty() ? std::string("null") : json::quote(comment);
	out += ",\"responseData\":";
	/* obs-websocket's own serialization of an object, spliced as is. */
	const char *p = data;
	while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
		p++;
	if (p && *p == '{')
		out += p;
	else
		out += "null";
	out += '}';
	return out;
}

} // namespace

RemoteControl::RemoteControl(Sender send) : send_(std::move(send)), tokens_(EVENT_BURST) {}

RemoteControl::~RemoteControl()
{
	if (listening_)
		LOG(LOG_WARNING, "remote control: OBS closed without the exit event");
	std::unique_lock<std::mutex> l(m_);
	stopping_ = true;
	for (Job &job : queue_)
		if (job.data)
			obs_data_release(job.data);
	queue_.clear();
	cv_.notify_all();
	/* Never hang OBS on exit: a request blocked on OBS's UI thread may never
	 * finish once the UI is gone. */
	const bool done = cv_.wait_for(l, std::chrono::seconds(3), [this] { return worker_done_; });
	l.unlock();
	if (worker_.joinable()) {
		if (done)
			worker_.join();
		else
			worker_.detach();
	}
}

bool RemoteControl::supported()
{
	return obs_websocket_get_api_version() >= MIN_API_VERSION;
}

std::string RemoteControl::refusal_frame(const std::string &id)
{
	return response_frame(id, UNKNOWN_REQUEST_TYPE, NOT_ALLOWED, nullptr);
}

void RemoteControl::start()
{
	listening_ = obs_websocket_register_event_callback(event_callback, this);
	if (!listening_)
		LOG(LOG_WARNING, "remote control: could not listen to OBS events");
	std::lock_guard<std::mutex> g(m_);
	worker_done_ = false;
	worker_ = std::thread([this] { worker_loop(); });
}

void RemoteControl::shutdown()
{
	/* obs-websocket is still loaded during OBS's exit event: the last moment to
	 * unregister. Once this returns, no callback runs. */
	if (listening_) {
		obs_websocket_unregister_event_callback(event_callback, this);
		listening_ = false;
	}
	std::lock_guard<std::mutex> g(m_);
	stopping_ = true;
	for (Job &job : queue_)
		if (job.data)
			obs_data_release(job.data);
	queue_.clear();
	cv_.notify_all();
}

void RemoteControl::reset_session()
{
	{
		std::lock_guard<std::mutex> g(events_m_);
		subscribed_.clear();
	}
	std::lock_guard<std::mutex> g(m_);
	for (Job &job : queue_)
		if (job.data)
			obs_data_release(job.data);
	queue_.clear();
}

void RemoteControl::reply(const std::string &id, unsigned code, const std::string &comment, const char *response_data)
{
	std::string frame = response_frame(id, code, comment, response_data);
	if (frame.size() > MAX_RESPONSE_BYTES)
		frame = response_frame(id, REQUEST_PROCESSING_FAILED,
				       "The response is larger than 3 MB. Ask for a smaller or a JPG image.", nullptr);
	send_(frame);
}

void RemoteControl::on_request(obs_data_t *message)
{
	const std::string id = obs_data_get_string(message, "id");
	if (id.empty() || id.size() > 64)
		return; /* nothing to answer to */
	const std::string type = obs_data_get_string(message, "requestType");
	const Allowed *rule = type.size() <= 64 ? find_request(type) : nullptr;
	if (!enabled_ || !rule) {
		reply(id, UNKNOWN_REQUEST_TYPE, NOT_ALLOWED);
		return;
	}

	obs_data_t *raw = nullptr;
	obs_data_item_t *item = obs_data_item_byname(message, "requestData");
	if (item) {
		const bool object = obs_data_item_gettype(item) == OBS_DATA_OBJECT;
		if (object)
			raw = obs_data_item_get_obj(item);
		obs_data_item_release(&item);
		if (!object) {
			reply(id, INVALID_REQUEST_FIELD_TYPE, "The field `requestData` must be an object.");
			return;
		}
	}
	unsigned code;
	std::string comment;
	obs_data_t *data = checked_fields(*rule, raw, code, comment);
	if (raw)
		obs_data_release(raw);
	if (code != SUCCESS) {
		reply(id, code, comment);
		return;
	}

	const char *refused = nullptr;
	{
		std::lock_guard<std::mutex> g(m_);
		if (stopping_)
			refused = "OBS is closing.";
		else if (queue_.size() >= MAX_QUEUE)
			refused = "Too many requests are waiting. Try again in a moment.";
		else
			queue_.push_back(Job{id, type, data, now_ms()});
		if (!refused)
			cv_.notify_one();
	}
	if (refused) {
		if (data)
			obs_data_release(data);
		reply(id, GENERIC_ERROR, refused);
	}
}

void RemoteControl::on_subscribe(const std::string &text)
{
	std::vector<std::string> names;
	if (!json::string_array_field(text, "events", names, MAX_SUBSCRIBED)) {
		LOG(LOG_WARNING, "remote control: ignoring an unexpected event subscription");
		return;
	}
	std::set<std::string> events;
	for (const std::string &n : names)
		if (allowed_event(n.c_str()))
			events.insert(n);
	std::lock_guard<std::mutex> g(events_m_);
	subscribed_ = std::move(events);
}

void RemoteControl::worker_loop()
{
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> l(m_);
			cv_.wait(l, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_)
				break;
			job = std::move(queue_.front());
			queue_.pop_front();
		}
		run(job);
		if (job.data)
			obs_data_release(job.data);
	}
	std::lock_guard<std::mutex> g(m_);
	worker_done_ = true;
	cv_.notify_all();
}

/* On the worker thread, like obs-websocket runs requests off the UI thread:
 * OBS's frontend calls hop to the UI thread by themselves. */
void RemoteControl::run(Job &job)
{
	/* Turned off after it was queued: it does not run. */
	if (!enabled_) {
		reply(job.id, UNKNOWN_REQUEST_TYPE, NOT_ALLOWED);
		return;
	}
	/* The backend already answered "timeout" for it: running it now would
	 * surprise whoever asked. */
	if (now_ms() - job.queued_ms > REQUEST_TTL_MS) {
		reply(job.id, GENERIC_ERROR, "The request expired before OBS could run it.");
		return;
	}
	last_request_ms_ = now_ms();
	if (job.type == "GetSourceScreenshot")
		cap_screenshot(job.data);
	struct obs_websocket_request_response *res = obs_websocket_call_request(job.type.c_str(), job.data);
	if (!res) {
		/* obs-websocket answers nothing until OBS has finished loading. */
		reply(job.id, NOT_READY, "OBS is not ready yet.");
		return;
	}
	if (job.type == "GetVersion" && res->response_data) {
		const std::string restricted = restrict_version_info(res->response_data);
		reply(job.id, res->status_code, res->comment ? res->comment : "", restricted.c_str());
	} else {
		reply(job.id, res->status_code, res->comment ? res->comment : "", res->response_data);
	}
	obs_websocket_request_response_free(res);
}

bool RemoteControl::take_event_token(bool low_priority)
{
	const long long now = now_ms();
	if (tokens_at_ms_ != 0)
		tokens_ = std::min(EVENT_BURST, tokens_ + (double)(now - tokens_at_ms_) * EVENTS_PER_SECOND / 1000.0);
	tokens_at_ms_ = now;
	if (tokens_ < (low_priority ? 1.0 + VOLUME_RESERVE : 1.0))
		return false;
	tokens_ -= 1.0;
	return true;
}

/* Called by obs-websocket on whatever thread the event happens (UI, audio...):
 * filter and send, nothing slow. */
void RemoteControl::on_event(uint64_t intent, const char *type, const char *data)
{
	if (!enabled_ || !type || !allowed_event(type))
		return;
	{
		std::lock_guard<std::mutex> g(events_m_);
		if (!subscribed_.count(type) || !take_event_token(std::strcmp(type, "InputVolumeChanged") == 0))
			return;
	}
	const char *p = data;
	while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
		p++;
	std::string frame = "{\"type\":\"obs_event\",\"eventType\":";
	frame += json::quote(type);
	frame += ",\"eventIntent\":";
	frame += std::to_string((unsigned long long)intent);
	frame += ",\"eventData\":";
	frame += p && *p == '{' ? p : "{}";
	frame += '}';
	if (frame.size() > MAX_EVENT_BYTES)
		return;
	send_(frame);
}

void RemoteControl::event_callback(uint64_t intent, const char *type, const char *data, void *priv)
{
	static_cast<RemoteControl *>(priv)->on_event(intent, type, data);
}

} // namespace eirl
