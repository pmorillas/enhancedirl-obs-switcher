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
 * The agent: links this OBS to an EnhancedIRL account and keeps one outgoing
 * WebSocket to the backend. Protocol: docs/PROTOCOL.md.
 *
 * What it can do is deliberately narrow: report the stream state and obey five
 * commands about the main stream (prepare, cut, start, commit, rollback). There
 * is no command to change the server, the key or any other OBS setting, and the
 * stream key never leaves OBS: only an HMAC fingerprint of it.
 */

#include "agent.hpp"
#include "obs-bridge.hpp"
#include "../managed.h"
#include "../rtmp-output/eirl-rtmp.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

extern "C" {
#include <plugin-support.h>
}

using Clock = std::chrono::steady_clock;

namespace {

constexpr int PROTOCOL = 1;
constexpr const char *DEFAULT_API = "https://api.enhancedirl.com";
constexpr const char *SUBPROTOCOL = "eirl-switcher.v1";
constexpr auto STATUS_EVERY = std::chrono::seconds(30);
constexpr auto LOCAL_START_TIMEOUT = std::chrono::seconds(40);

/* Close codes after which the plugin must not retry (docs/PROTOCOL.md §2). */
constexpr int CLOSE_REVOKED = 4401;
constexpr int CLOSE_NOT_ALLOWED = 4403;
constexpr int CLOSE_REPLACED = 4409;
constexpr int CLOSE_UPGRADE_REQUIRED = 4426;

#define LOG(level, fmt, ...) blog(level, "[enhancedirl-obs-switcher] " fmt, ##__VA_ARGS__)

std::string api_base()
{
	/* Only for development builds and tests: production uses the default. */
	const char *env = std::getenv("EIRL_SWITCHER_API");
	std::string base = env && *env ? env : DEFAULT_API;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	return base;
}

std::string ws_url(const std::string &base)
{
	if (base.rfind("https://", 0) == 0)
		return "wss://" + base.substr(8) + "/obs-agent";
	if (base.rfind("http://", 0) == 0)
		return "ws://" + base.substr(7) + "/obs-agent";
	return base + "/obs-agent";
}

/* Trusted roots for TLS: the system store on Windows, the system bundle elsewhere. */
std::string ca_file()
{
	/* Windows: the system store (mbedTLS backend). Linux: OpenSSL's default trust
	 * store. macOS: the system bundle that ships with macOS (mbedTLS backend). */
#if defined(__APPLE__)
	return "/etc/ssl/cert.pem";
#else
	return "SYSTEM";
#endif
}

const char *os_name()
{
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#else
	return "linux";
#endif
}

std::string machine_name()
{
	char buf[256] = {0};
#ifdef _WIN32
	DWORD size = sizeof(buf);
	if (!GetComputerNameA(buf, &size))
		return "OBS";
#else
	if (gethostname(buf, sizeof(buf) - 1) != 0)
		return "OBS";
#endif
	std::string name = buf;
	if (name.size() > 6 && name.substr(name.size() - 6) == ".local")
		name.resize(name.size() - 6);
	return name.empty() ? "OBS" : name.substr(0, 64);
}

/* Only https URLs (http for a development API) with plain characters reach the
 * browser: the URL comes from the backend, but we do not trust it blindly. */
bool safe_url(const std::string &url)
{
	bool dev = api_base().rfind("http://", 0) == 0;
	if (url.rfind("https://", 0) != 0 && !(dev && url.rfind("http://", 0) == 0))
		return false;
	for (char c : url)
		if (!(std::isalnum((unsigned char)c) || std::strchr(":/?=&.-_%#", c)))
			return false;
	return url.size() < 512;
}

void open_in_browser(const std::string &url)
{
	/* Automated tests link through the API and must not pop up a browser. */
	const char *no_browser = std::getenv("EIRL_SWITCHER_NO_BROWSER");
	if (no_browser && *no_browser)
		return;
	if (!safe_url(url)) {
		LOG(LOG_WARNING, "refusing to open an unexpected URL");
		return;
	}
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
	const char *tool = "open";
#else
	const char *tool = "xdg-open";
#endif
	pid_t pid;
	char *argv[] = {(char *)tool, (char *)url.c_str(), nullptr};
	if (posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ) == 0) {
		std::thread([pid] {
			int status;
			waitpid(pid, &status, 0);
		}).detach();
	}
#endif
}

/* --- tiny JSON helpers on top of obs_data (libobs' jansson wrapper) --- */

struct Json {
	obs_data_t *d;
	explicit Json(obs_data_t *data = obs_data_create()) : d(data) {}
	~Json()
	{
		if (d)
			obs_data_release(d);
	}
	Json(const Json &) = delete;
	Json &operator=(const Json &) = delete;
	static Json parse(const std::string &text) { return Json(obs_data_create_from_json(text.c_str())); }
	bool ok() const { return d != nullptr; }
	std::string str(const char *k) const { return d ? obs_data_get_string(d, k) : ""; }
	long long num(const char *k) const { return d ? obs_data_get_int(d, k) : 0; }
	bool has(const char *k) const { return d && obs_data_has_user_value(d, k); }
	Json &set(const char *k, const std::string &v)
	{
		obs_data_set_string(d, k, v.c_str());
		return *this;
	}
	Json &set(const char *k, long long v)
	{
		obs_data_set_int(d, k, v);
		return *this;
	}
	Json &set_bool(const char *k, bool v)
	{
		obs_data_set_bool(d, k, v);
		return *this;
	}
	Json &set_obj(const char *k, Json &v)
	{
		obs_data_set_obj(d, k, v.d);
		return *this;
	}
	std::string dump() const
	{
		const char *j = obs_data_get_json(d);
		return j ? j : "{}";
	}
};

struct Link {
	std::string apiBase;
	std::string agentId;
	std::string token;
	std::string salt;
	bool valid() const { return !agentId.empty() && !token.empty(); }
};

std::string link_path()
{
	char *path = obs_module_config_path("agent.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

Link load_link()
{
	Link l;
	obs_data_t *d = obs_data_create_from_json_file(link_path().c_str());
	if (!d)
		return l;
	l.apiBase = obs_data_get_string(d, "apiBase");
	l.agentId = obs_data_get_string(d, "agentId");
	l.token = obs_data_get_string(d, "agentToken");
	l.salt = obs_data_get_string(d, "fingerprintSalt");
	obs_data_release(d);
	return l;
}

void save_link(const Link &l)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	Json d;
	d.set("apiBase", l.apiBase).set("agentId", l.agentId).set("agentToken", l.token).set("fingerprintSalt", l.salt);
	std::string path = link_path();
	obs_data_save_json_safe(d.d, path.c_str(), "tmp", "bak");
#ifndef _WIN32
	chmod(path.c_str(), 0600); /* the token is a credential: user-only */
	std::string bak = path + ".bak";
	chmod(bak.c_str(), 0600);
#endif
}

/* Managed build (Cloud OBS): the link comes from the server's environment. */
Link managed_link()
{
	Link l;
	const char *token = std::getenv("EIRL_SWITCHER_AGENT_TOKEN");
	const char *salt = std::getenv("EIRL_SWITCHER_FINGERPRINT_SALT");
	if (token && *token) {
		l.apiBase = api_base();
		l.agentId = "managed";
		l.token = token;
		l.salt = salt ? salt : "";
	}
	return l;
}

void forget_link()
{
	std::string path = link_path();
	os_unlink(path.c_str());
	os_unlink((path + ".bak").c_str());
}

/* ------------------------------------------------------------------------ */

class Agent {
public:
	void start()
	{
		ix::initNetSystem();
		running_ = true;
		ticker_ = std::thread([this] { tick_loop(); });
		Link l = EIRL_MANAGED_BUILD ? managed_link() : load_link();
		if (l.valid()) {
			std::lock_guard<std::mutex> g(m_);
			link_ = l;
			connect_locked();
		}
	}

	void stop()
	{
		running_ = false;
		pairing_cancel_ = true;
		if (ticker_.joinable())
			ticker_.join();
		if (pairing_.joinable())
			pairing_.join();
		std::unique_ptr<ix::WebSocket> ws;
		{
			std::lock_guard<std::mutex> g(m_);
			ws = std::move(ws_);
		}
		if (ws)
			ws->stop();
		ix::uninitNetSystem();
	}

	void begin_pairing()
	{
		if (EIRL_MANAGED_BUILD)
			return; /* a Cloud OBS is linked by EnhancedIRL, never by hand */
		if (pairing_active_.exchange(true))
			return;
		if (pairing_.joinable())
			pairing_.join();
		pairing_cancel_ = false;
		pairing_ = std::thread([this] {
			pair_flow();
			pairing_active_ = false;
		});
	}

	void unlink()
	{
		if (EIRL_MANAGED_BUILD)
			return;
		std::unique_ptr<ix::WebSocket> ws;
		{
			std::lock_guard<std::mutex> g(m_);
			ws = std::move(ws_);
			link_ = Link{};
			account_.clear();
			connected_ = false;
			deadman_armed_ = false;
		}
		if (ws)
			ws->stop();
		forget_link();
		LOG(LOG_INFO, "unlinked from the EnhancedIRL account");
	}

	void state(eirl_agent_state *out)
	{
		std::lock_guard<std::mutex> g(m_);
		*out = {};
		out->pairing = pairing_active_;
		out->linked = link_.valid();
		out->connected = connected_;
		snprintf(out->code, sizeof(out->code), "%s", pairing_code_.c_str());
		snprintf(out->account, sizeof(out->account), "%s", account_.c_str());
		snprintf(out->error, sizeof(out->error), "%s", last_error_.c_str());
	}

	void reopen_browser()
	{
		std::string url;
		{
			std::lock_guard<std::mutex> g(m_);
			url = verify_url_;
		}
		if (!url.empty())
			open_in_browser(url);
	}

	std::string describe()
	{
		std::lock_guard<std::mutex> g(m_);
		if (pairing_active_)
			return pairing_code_.empty() ? "Linking..." : "Linking: confirm code " + pairing_code_ + " in your browser";
		if (!link_.valid())
			return last_error_.empty() ? "Not linked" : "Not linked (" + last_error_ + ")";
		std::string who = account_.empty() ? "your EnhancedIRL account" : account_;
		return "Linked to " + who + (connected_ ? " (connected)" : " (connecting...)");
	}

	void on_streaming_started()
	{
		std::string id;
		{
			std::lock_guard<std::mutex> g(m_);
			id = pending_start_id_;
			pending_start_id_.clear();
		}
		if (!id.empty())
			send_result(id, true);
		send_event("streaming_started");
		force_status_ = true;
	}

	void on_streaming_stopped()
	{
		std::string id;
		{
			std::lock_guard<std::mutex> g(m_);
			id = pending_start_id_;
			pending_start_id_.clear();
		}
		if (!id.empty())
			send_result(id, false, "start_failed");
		send_event("streaming_stopped");
		force_status_ = true;
	}

private:
	/* Caller holds m_. */
	void connect_locked()
	{
		if (ws_ || !link_.valid())
			return;
		auto ws = std::make_unique<ix::WebSocket>();
		ws->setUrl(ws_url(link_.apiBase.empty() ? api_base() : link_.apiBase));
		ix::WebSocketHttpHeaders headers;
		headers["Authorization"] = "Bearer " + link_.token;
		ws->setExtraHeaders(headers);
		ws->addSubProtocol(SUBPROTOCOL);
		ix::SocketTLSOptions tls;
		tls.caFile = ca_file();
		ws->setTLSOptions(tls);
		ws->setPingInterval(30);
		ws->enableAutomaticReconnection();
		ws->setMinWaitBetweenReconnectionRetries(1000);
		ws->setMaxWaitBetweenReconnectionRetries(30000);
		ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) { on_ws(msg); });
		ws->start();
		ws_ = std::move(ws);
	}

	void send(const std::string &text)
	{
		std::lock_guard<std::mutex> g(m_);
		if (ws_ && connected_)
			ws_->sendText(text);
	}

	void send_result(const std::string &id, bool ok, const std::string &error = "", const std::string &detail = "")
	{
		Json r;
		r.set("type", "result").set("id", id).set_bool("ok", ok);
		if (!error.empty())
			r.set("error", error);
		if (!detail.empty())
			r.set("detail", detail);
		send(r.dump());
	}

	void send_event(const char *name)
	{
		Json e;
		e.set("type", "event").set("name", name).set("at", (long long)(os_gettime_ns() / 1000000));
		send(e.dump());
	}

	std::string status_json()
	{
		std::string salt;
		{
			std::lock_guard<std::mutex> g(m_);
			salt = link_.salt;
		}
		eirl::ObsState s = eirl::read_obs_state(salt);
		Json st;
		st.set("type", "status")
			.set_bool("streaming", s.streaming)
			.set_bool("reconnecting", s.reconnecting)
			.set_bool("serviceReady", s.serviceReady);
		if (!s.platform.empty())
			st.set("platform", s.platform);
		if (!s.serverHost.empty())
			st.set("serverHost", s.serverHost);
		if (!s.keyFingerprint.empty())
			st.set("keyFingerprint", s.keyFingerprint);
		if (!s.platform.empty()) {
			Json hint;
			hint.set("platform", s.platform);
			if (!s.twitchBroadcasterId.empty())
				hint.set("broadcasterId", s.twitchBroadcasterId);
			st.set_obj("channelHint", hint);
		}
		if (s.video) {
			Json v;
			v.set("width", s.video->width).set("height", s.video->height).set("fps", s.video->fps);
			st.set_obj("video", v);
		}
		if (s.encoder) {
			Json e;
			e.set("codec", s.encoder->codec).set("bitrateKbps", s.encoder->bitrateKbps);
			st.set_obj("encoder", e);
		}
		return st.dump();
	}

	void send_hello()
	{
		Json h;
		h.set("type", "hello")
			.set("protocol", PROTOCOL)
			.set("pluginVersion", PLUGIN_VERSION)
			.set("obsVersion", obs_get_version_string())
			.set("os", os_name());
		/* obs_data has no arrays of plain strings: append that field by hand. */
		std::string json = h.dump();
		json = json.substr(0, json.rfind('}')) + ",\"capabilities\":[\"cut\",\"start\"]}";
		send(json);
	}

	void on_ws(const ix::WebSocketMessagePtr &msg)
	{
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			{
				std::lock_guard<std::mutex> g(m_);
				connected_ = true;
				last_error_.clear();
			}
			LOG(LOG_INFO, "connected to EnhancedIRL");
			send_hello();
			force_status_ = true;
			break;
		}
		case ix::WebSocketMessageType::Close: {
			int code = msg->closeInfo.code;
			{
				std::lock_guard<std::mutex> g(m_);
				connected_ = false;
			}
			LOG(LOG_INFO, "disconnected from EnhancedIRL (%d)", code);
			if (code == CLOSE_REVOKED || code == CLOSE_NOT_ALLOWED || code == CLOSE_REPLACED ||
			    code == CLOSE_UPGRADE_REQUIRED)
				final_close(code);
			break;
		}
		case ix::WebSocketMessageType::Error:
			LOG(LOG_WARNING, "connection error: %s", msg->errorInfo.reason.c_str());
			break;
		case ix::WebSocketMessageType::Message:
			if (!msg->binary)
				on_message(msg->str);
			break;
		default:
			break;
		}
	}

	void final_close(int code)
	{
		std::lock_guard<std::mutex> g(m_);
		if (ws_)
			ws_->disableAutomaticReconnection();
		switch (code) {
		case CLOSE_REVOKED:
			last_error_ = "revoked";
			if (!EIRL_MANAGED_BUILD) {
				link_ = Link{};
				account_.clear();
				forget_link();
			}
			break;
		case CLOSE_NOT_ALLOWED:
			last_error_ = "not_allowed";
			break;
		case CLOSE_REPLACED:
			last_error_ = "replaced";
			break;
		default:
			last_error_ = "upgrade";
			break;
		}
	}

	void on_message(const std::string &text)
	{
		Json m = Json::parse(text);
		if (!m.ok())
			return;
		const std::string type = m.str("type");
		const std::string id = m.str("id");

		if (type == "welcome") {
			std::lock_guard<std::mutex> g(m_);
			account_ = m.str("accountName").substr(0, 128);
			LOG(LOG_INFO, "linked to %s", account_.c_str());
			return;
		}
		if (id.empty() || id.size() > 64)
			return; /* every command carries an id */

		if (type == "prepare") {
			eirl::ObsState s = eirl::read_obs_state("");
			if (s.streaming)
				send_result(id, false, "already_streaming");
			else if (!s.serviceReady)
				send_result(id, false, "not_ready");
			else
				send_result(id, true);
		} else if (type == "cut") {
			handle_cut(id, m.has("deadManMs") ? m.num("deadManMs") : 0);
		} else if (type == "start" || type == "rollback") {
			if (type == "rollback") {
				std::lock_guard<std::mutex> g(m_);
				deadman_armed_ = false;
			}
			/* YouTube: which of its two ingests to use (the free one, see eirl-rtmp.h). */
			const std::string ingest = m.str("ingest");
			eirl_rtmp_set_next_ingest(ingest == "backup"	 ? EIRL_INGEST_BACKUP
						  : ingest == "primary" ? EIRL_INGEST_PRIMARY
									: EIRL_INGEST_ASIS);
			handle_start(id);
		} else if (type == "commit") {
			{
				std::lock_guard<std::mutex> g(m_);
				deadman_armed_ = false;
			}
			send_result(id, true);
		}
		/* Anything else is ignored: the backend has no other commands. */
	}

	void handle_cut(const std::string &id, long long dead_man_ms)
	{
		std::string cancelled_start;
		{
			std::lock_guard<std::mutex> g(m_);
			cancelled_start = pending_start_id_;
			pending_start_id_.clear();
		}
		bool was_starting = !cancelled_start.empty();
		bool cut = eirl::cut_streaming();
		if (was_starting)
			send_result(cancelled_start, false, "start_failed", "cancelled");
		if (!cut && !was_starting) {
			send_result(id, false, eirl::read_obs_state("").streaming ? "not_switcher_output" : "not_streaming");
			return;
		}
		if (dead_man_ms > 0) {
			std::lock_guard<std::mutex> g(m_);
			/* Never wait longer than the platforms' shortest window allows. */
			long long ms = dead_man_ms > 60000 ? 60000 : dead_man_ms;
			deadman_at_ = Clock::now() + std::chrono::milliseconds(ms);
			deadman_armed_ = true;
		}
		LOG(LOG_INFO, "switch: cut without goodbye%s", dead_man_ms > 0 ? " (dead-man armed)" : "");
		send_result(id, true);
	}

	void handle_start(const std::string &id)
	{
		eirl::ObsState s = eirl::read_obs_state("");
		if (s.streaming) {
			send_result(id, true);
			return;
		}
		if (!s.serviceReady) {
			send_result(id, false, "start_failed", "service not ready");
			return;
		}
		{
			std::lock_guard<std::mutex> g(m_);
			pending_start_id_ = id;
			pending_start_at_ = Clock::now();
		}
		eirl::start_streaming();
	}

	void tick_loop()
	{
		auto last_status_sent = Clock::now() - STATUS_EVERY;
		std::string last_status;
		while (running_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			bool restart = false;
			std::string timed_out_start;
			bool connected;
			{
				std::lock_guard<std::mutex> g(m_);
				connected = connected_;
				if (deadman_armed_ && Clock::now() >= deadman_at_) {
					deadman_armed_ = false;
					restart = true;
				}
				if (!pending_start_id_.empty() && Clock::now() - pending_start_at_ > LOCAL_START_TIMEOUT) {
					timed_out_start = pending_start_id_;
					pending_start_id_.clear();
				}
			}
			if (restart) {
				LOG(LOG_WARNING, "switch: no commit received, going back on air (dead-man)");
				/* With the old takeover priority: if the other OBS did go on air, the
				 * platform keeps it and this one does not take the stream back. */
				eirl_rtmp_keep_priority_once();
				eirl::start_streaming();
				send_event("deadman_restart");
			}
			if (!timed_out_start.empty())
				send_result(timed_out_start, false, "start_failed", "timeout");
			if (!connected)
				continue;
			std::string status = status_json();
			if (force_status_.exchange(false) || status != last_status ||
			    Clock::now() - last_status_sent >= STATUS_EVERY) {
				send(status);
				last_status = status;
				last_status_sent = Clock::now();
			}
		}
	}

	void pair_flow()
	{
		std::string base = api_base();
		ix::HttpClient http(false);
		ix::SocketTLSOptions tls;
		tls.caFile = ca_file();
		http.setTLSOptions(tls);
		auto args = http.createRequest();
		args->extraHeaders["Content-Type"] = "application/json";
		args->connectTimeout = 10;
		args->transferTimeout = 20;

		Json body;
		body.set("name", machine_name())
			.set("os", os_name())
			.set("obsVersion", obs_get_version_string())
			.set("pluginVersion", PLUGIN_VERSION);
		auto res = http.post(base + "/obs-agent/pairing", body.dump(), args);
		if (!res || res->statusCode != 201) {
			set_error(res && res->statusCode == 429 ? "too_many"
								 : "network");
			LOG(LOG_WARNING, "pairing request failed (%d)", res ? res->statusCode : -1);
			return;
		}
		Json p = Json::parse(res->body);
		const std::string pairing_id = p.str("pairingId");
		const std::string code = p.str("code");
		const std::string poll_secret = p.str("pollSecret");
		const std::string verify_url = p.str("verifyUrl");
		long long interval = p.num("interval");
		long long expires = p.num("expiresIn");
		if (pairing_id.empty() || poll_secret.empty()) {
			set_error("unexpected");
			return;
		}
		{
			std::lock_guard<std::mutex> g(m_);
			pairing_code_ = code;
			verify_url_ = verify_url;
			last_error_.clear();
		}
		LOG(LOG_INFO, "linking: confirm code %s in your browser", code.c_str());
		open_in_browser(verify_url);

		if (interval < 1 || interval > 30)
			interval = 3;
		auto deadline = Clock::now() + std::chrono::seconds(expires > 0 && expires < 3600 ? expires : 600);
		Json poll;
		poll.set("pollSecret", poll_secret);
		const std::string poll_body = poll.dump();
		while (!pairing_cancel_ && Clock::now() < deadline) {
			for (long long i = 0; i < interval * 4 && !pairing_cancel_; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			auto r = http.post(base + "/obs-agent/pairing/" + pairing_id + "/token", poll_body, args);
			if (!r || r->statusCode == 202 || r->statusCode == 0)
				continue;
			if (r->statusCode == 200) {
				Json c = Json::parse(r->body);
				Link l;
				l.apiBase = base;
				l.agentId = c.str("agentId");
				l.token = c.str("agentToken");
				l.salt = c.str("fingerprintSalt");
				if (!l.valid()) {
					set_error("unexpected");
					break;
				}
				save_link(l);
				std::lock_guard<std::mutex> g(m_);
				link_ = l;
				pairing_code_.clear();
				last_error_.clear();
				connect_locked();
				LOG(LOG_INFO, "linked");
				return;
			}
			Json e = Json::parse(r->body);
			const std::string err = e.str("error");
			set_error(err == "limit_reached"        ? "limit_reached"
				  : err == "email_not_verified" ? "email_not_verified"
								: "expired");
			break;
		}
		std::lock_guard<std::mutex> g(m_);
		pairing_code_.clear();
		if (last_error_.empty())
			last_error_ = "expired";
	}

	void set_error(const std::string &e)
	{
		std::lock_guard<std::mutex> g(m_);
		last_error_ = e;
		pairing_code_.clear();
	}

	std::mutex m_;
	Link link_;
	std::unique_ptr<ix::WebSocket> ws_;
	bool connected_ = false;
	std::string account_;
	std::string last_error_;
	std::string pairing_code_;
	std::string verify_url_;
	std::string pending_start_id_;
	Clock::time_point pending_start_at_;
	bool deadman_armed_ = false;
	Clock::time_point deadman_at_;
	std::atomic<bool> force_status_{false};
	std::atomic<bool> running_{false};
	std::atomic<bool> pairing_active_{false};
	std::atomic<bool> pairing_cancel_{false};
	std::thread ticker_;
	std::thread pairing_;
};

Agent *g_agent = nullptr;

} // namespace

extern "C" {

void eirl_agent_start(void)
{
	if (g_agent)
		return;
	g_agent = new Agent();
	g_agent->start();
}

void eirl_agent_stop(void)
{
	if (!g_agent)
		return;
	g_agent->stop();
	delete g_agent;
	g_agent = nullptr;
}

void eirl_agent_begin_pairing(void)
{
	if (g_agent)
		g_agent->begin_pairing();
}

void eirl_agent_unlink(void)
{
	if (g_agent)
		g_agent->unlink();
}

void eirl_agent_get_state(struct eirl_agent_state *out)
{
	if (g_agent)
		g_agent->state(out);
	else
		*out = {};
}

void eirl_agent_reopen_browser(void)
{
	if (g_agent)
		g_agent->reopen_browser();
}

char *eirl_agent_describe(void)
{
	return bstrdup(g_agent ? g_agent->describe().c_str() : "Not running");
}

void eirl_agent_on_streaming_started(void)
{
	if (g_agent)
		g_agent->on_streaming_started();
}

void eirl_agent_on_streaming_stopped(void)
{
	if (g_agent)
		g_agent->on_streaming_stopped();
}
}
