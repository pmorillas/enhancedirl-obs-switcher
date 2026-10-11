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

/*
 * Remote control (docs/PROTOCOL.md §3): runs a closed list of obs-websocket
 * requests inside OBS through obs-websocket's plugin API, and forwards a closed
 * list of its events. No WebSocket server and no password are involved, and
 * nothing outside the list can run, whatever the backend sends.
 *
 * Off unless the user turns it on in the plugin window.
 */

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace eirl {

class RemoteControl {
public:
	/* Sends one text frame to the backend (dropped while disconnected). */
	using Sender = std::function<void(const std::string &)>;

	explicit RemoteControl(Sender send);
	~RemoteControl();
	RemoteControl(const RemoteControl &) = delete;
	RemoteControl &operator=(const RemoteControl &) = delete;

	/* True if this OBS has obs-websocket with the plugin API we need (event
	 * callbacks: obs-websocket 5.5, bundled since OBS 30.2). Call it from
	 * obs_module_post_load or later. */
	static bool supported();
	/* The answer to a request when there is no remote control at all. */
	static std::string refusal_frame(const std::string &id);

	/* Starts the worker and listens to obs-websocket's events. */
	void start();
	/* OBS is closing: stop listening and stop running requests, while
	 * obs-websocket is still loaded. Call on the UI thread (OBS_FRONTEND_EVENT_EXIT). */
	void shutdown();

	/* The user's switch. Off refuses every request, also the ones already queued. */
	void set_enabled(bool enabled) { enabled_ = enabled; }
	bool enabled() const { return enabled_; }

	/* A new connection to the backend, or the end of one: forget the event
	 * subscription and the queued requests (their answers could not arrive). */
	void reset_session();

	/* `obs_request` from the backend (`message` is the parsed frame). Never blocks:
	 * the request runs on the worker thread and the answer is sent from there. */
	void on_request(obs_data_t *message);
	/* `obs_subscribe` from the backend (`text` is the raw frame). */
	void on_subscribe(const std::string &text);

	/* When a request last ran (steady clock, ms; 0 = never). For the UI. */
	long long last_request_ms() const { return last_request_ms_; }

private:
	struct Job {
		std::string id;
		std::string type;
		obs_data_t *data = nullptr; /* checked fields only, or null */
		long long queued_ms = 0;
	};

	void worker_loop();
	void run(Job &job);
	void reply(const std::string &id, unsigned code, const std::string &comment,
		   const char *response_data = nullptr);
	void on_event(uint64_t intent, const char *type, const char *data);
	bool take_event_token(bool low_priority);
	static void event_callback(uint64_t intent, const char *type, const char *data, void *priv);

	Sender send_;
	std::atomic<bool> enabled_{false};
	std::atomic<long long> last_request_ms_{0};

	std::mutex m_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	bool stopping_ = false;
	bool worker_done_ = true;
	std::thread worker_;
	bool listening_ = false;

	std::mutex events_m_;
	std::set<std::string> subscribed_;
	double tokens_;
	long long tokens_at_ms_ = 0;
};

} // namespace eirl
