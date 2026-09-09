// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "cpu.h"
#include "dos.h"
#include "memory.h"

#include "capture/capture.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <set>
#include <string>
#include <thread>

#include "libs/http/http.h"
#include "libs/json/json.h"

#include "config/config.h"
#include "dosbox.h"
#include "misc/cross.h"
#include "misc/logging.h"
#include "misc/support.h"
#include "hardware/input/keyboard.h"
#include "hardware/input/mouse.h"

using json = nlohmann::json;

namespace Webserver {

void send_json(httplib::Response& res, const nlohmann::json& j)
{
	res.set_content(j.dump(2), "application/json");
}

static void error_handler(const httplib::Request&, httplib::Response& res,
                          std::exception_ptr ep)
{
	json j;
	std::string msg;

	try {
		if (ep) {
			std::rethrow_exception(ep);
		}
	} catch (const std::exception& e) {
		msg = e.what();
	} catch (...) {
		msg = "Unknown error";
	}

	j["error"] = msg;
	res.status = httplib::StatusCode::InternalServerError_500;

	send_json(res, j);
}

static httplib::Server server;


// DARKLANDS_INPUT_API BEGIN
enum class DarklandsInputAction {
	Press,
	Down,
	Up,
};

static std::string darklands_lower(std::string value)
{
	for (auto& c : value) {
		c = static_cast<char>(
		        std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
}

static DarklandsInputAction darklands_parse_action(std::string value)
{
	value = darklands_lower(std::move(value));

	if (value == "press" || value == "click") {
		return DarklandsInputAction::Press;
	}
	if (value == "down") {
		return DarklandsInputAction::Down;
	}
	if (value == "up") {
		return DarklandsInputAction::Up;
	}

	throw std::invalid_argument(
	        "action must be press/click, down, or up");
}

static KBD_KEYS darklands_key_from_name(std::string name)
{
	name = darklands_lower(std::move(name));

	static const std::unordered_map<std::string, KBD_KEYS> keys = {
	        {"1", KBD_1}, {"2", KBD_2}, {"3", KBD_3},
	        {"4", KBD_4}, {"5", KBD_5}, {"6", KBD_6},
	        {"7", KBD_7}, {"8", KBD_8}, {"9", KBD_9},
	        {"0", KBD_0},

	        {"q", KBD_q}, {"w", KBD_w}, {"e", KBD_e},
	        {"r", KBD_r}, {"t", KBD_t}, {"y", KBD_y},
	        {"u", KBD_u}, {"i", KBD_i}, {"o", KBD_o},
	        {"p", KBD_p},

	        {"a", KBD_a}, {"s", KBD_s}, {"d", KBD_d},
	        {"f", KBD_f}, {"g", KBD_g}, {"h", KBD_h},
	        {"j", KBD_j}, {"k", KBD_k}, {"l", KBD_l},

	        {"z", KBD_z}, {"x", KBD_x}, {"c", KBD_c},
	        {"v", KBD_v}, {"b", KBD_b}, {"n", KBD_n},
	        {"m", KBD_m},

	        {"f1", KBD_f1},   {"f2", KBD_f2},
	        {"f3", KBD_f3},   {"f4", KBD_f4},
	        {"f5", KBD_f5},   {"f6", KBD_f6},
	        {"f7", KBD_f7},   {"f8", KBD_f8},
	        {"f9", KBD_f9},   {"f10", KBD_f10},
	        {"f11", KBD_f11}, {"f12", KBD_f12},

	        {"esc", KBD_esc},
	        {"escape", KBD_esc},
	        {"tab", KBD_tab},
	        {"backspace", KBD_backspace},
	        {"enter", KBD_enter},
	        {"return", KBD_enter},
	        {"space", KBD_space},

	        {"left", KBD_left},
	        {"right", KBD_right},
	        {"up", KBD_up},
	        {"down", KBD_down},

	        {"home", KBD_home},
	        {"end", KBD_end},
	        {"pageup", KBD_pageup},
	        {"pgup", KBD_pageup},
	        {"pagedown", KBD_pagedown},
	        {"pgdn", KBD_pagedown},
	        {"insert", KBD_insert},
	        {"delete", KBD_delete},

	        {"shift", KBD_leftshift},
	        {"leftshift", KBD_leftshift},
	        {"rightshift", KBD_rightshift},

	        {"ctrl", KBD_leftctrl},
	        {"control", KBD_leftctrl},
	        {"leftctrl", KBD_leftctrl},
	        {"rightctrl", KBD_rightctrl},

	        {"alt", KBD_leftalt},
	        {"leftalt", KBD_leftalt},
	        {"rightalt", KBD_rightalt},

	        {"minus", KBD_minus},
	        {"equals", KBD_equals},
	        {"grave", KBD_grave},
	        {"backslash", KBD_backslash},
	        {"leftbracket", KBD_leftbracket},
	        {"rightbracket", KBD_rightbracket},
	        {"semicolon", KBD_semicolon},
	        {"quote", KBD_quote},
	        {"comma", KBD_comma},
	        {"period", KBD_period},
	        {"slash", KBD_slash},

	        {"kp0", KBD_kp0},
	        {"kp1", KBD_kp1},
	        {"kp2", KBD_kp2},
	        {"kp3", KBD_kp3},
	        {"kp4", KBD_kp4},
	        {"kp5", KBD_kp5},
	        {"kp6", KBD_kp6},
	        {"kp7", KBD_kp7},
	        {"kp8", KBD_kp8},
	        {"kp9", KBD_kp9},
	        {"kpenter", KBD_kpenter},
	        {"kpplus", KBD_kpplus},
	        {"kpminus", KBD_kpminus},
	        {"kpmultiply", KBD_kpmultiply},
	        {"kpdivide", KBD_kpdivide},
	};

	const auto it = keys.find(name);
	if (it == keys.end()) {
		throw std::invalid_argument("unknown key: " + name);
	}

	return it->second;
}

static MouseButtonId darklands_mouse_button(std::string name)
{
	name = darklands_lower(std::move(name));

	if (name == "left" || name == "1") {
		return MouseButtonId::Left;
	}
	if (name == "right" || name == "2") {
		return MouseButtonId::Right;
	}
	if (name == "middle" || name == "3") {
		return MouseButtonId::Middle;
	}

	throw std::invalid_argument(
	        "button must be left, right, or middle");
}


class DarklandsKeyCommand final : public Command {
public:
	DarklandsKeyCommand(const KBD_KEYS key,
	                    const DarklandsInputAction action)
	        : key(key),
	          action(action)
	{}

	void Execute() override
	{
		switch (action) {
		case DarklandsInputAction::Press:
			KEYBOARD_AddKey(key, true);
			KEYBOARD_AddKey(key, false);
			break;

		case DarklandsInputAction::Down:
			KEYBOARD_AddKey(key, true);
			break;

		case DarklandsInputAction::Up:
			KEYBOARD_AddKey(key, false);
			break;
		}
	}

	static void Post(const httplib::Request& req,
	                 httplib::Response& res)
	{
		const auto j = json::parse(req.body);

		const auto key_name =
		        j.at("key").get<std::string>();

		const auto action_name =
		        j.value("action", std::string("press"));

		DarklandsKeyCommand cmd(
		        darklands_key_from_name(key_name),
		        darklands_parse_action(action_name));

		cmd.WaitForCompletion();

		if (!cmd.error.empty()) {
			throw std::runtime_error(cmd.error);
		}

		json out;
		out["ok"] = true;
		out["key"] = key_name;
		out["action"] = action_name;
		send_json(res, out);
	}

private:
	KBD_KEYS key;
	DarklandsInputAction action;
};


class DarklandsMouseMoveCommand final : public Command {
public:
	DarklandsMouseMoveCommand(const float dx,
	                          const float dy)
	        : dx(dx),
	          dy(dy)
	{}

	void Execute() override
	{
		if (!MOUSE_InjectRemoteMove(dx, dy)) {
			error = "DOS mouse interface is not active";
		}
	}

	static void Post(const httplib::Request& req,
	                 httplib::Response& res)
	{
		const auto j = json::parse(req.body);

		const auto dx = j.value("dx", 0.0f);
		const auto dy = j.value("dy", 0.0f);

		DarklandsMouseMoveCommand cmd(dx, dy);
		cmd.WaitForCompletion();

		if (!cmd.error.empty()) {
			throw std::runtime_error(cmd.error);
		}

		json out;
		out["ok"] = true;
		out["dx"] = dx;
		out["dy"] = dy;
		send_json(res, out);
	}

private:
	float dx = 0.0f;
	float dy = 0.0f;
};


class DarklandsMouseButtonCommand final : public Command {
public:
	DarklandsMouseButtonCommand(
	        const MouseButtonId button,
	        const DarklandsInputAction action)
	        : button(button),
	          action(action)
	{}

	void Execute() override
	{
		switch (action) {
		case DarklandsInputAction::Press:
			if (!MOUSE_InjectRemoteButton(button, true) ||
			    !MOUSE_InjectRemoteButton(button, false)) {
				error = "DOS mouse interface is not active";
			}
			break;

		case DarklandsInputAction::Down:
			if (!MOUSE_InjectRemoteButton(button, true)) {
				error = "DOS mouse interface is not active";
			}
			break;

		case DarklandsInputAction::Up:
			if (!MOUSE_InjectRemoteButton(button, false)) {
				error = "DOS mouse interface is not active";
			}
			break;
		}
	}

	static void Post(const httplib::Request& req,
	                 httplib::Response& res)
	{
		const auto j = json::parse(req.body);

		const auto button_name =
		        j.at("button").get<std::string>();

		const auto action_name =
		        j.value("action", std::string("click"));

		DarklandsMouseButtonCommand cmd(
		        darklands_mouse_button(button_name),
		        darklands_parse_action(action_name));

		cmd.WaitForCompletion();

		if (!cmd.error.empty()) {
			throw std::runtime_error(cmd.error);
		}

		json out;
		out["ok"] = true;
		out["button"] = button_name;
		out["action"] = action_name;
		send_json(res, out);
	}

private:
	MouseButtonId button;
	DarklandsInputAction action;
};
// DARKLANDS_INPUT_API END


static void setup_api_handlers()
{
	server.Get("/api/v1/cpu/state", CpuStateCommand::Get);

	server.Get("/api/v1/dos/internals", DosInternalsCommand::Get);

	server.Post("/api/v1/memory/allocate", AllocMemoryCommand::Post);
	server.Post("/api/v1/memory/free", FreeMemoryCommand::Post);
	server.Get("/api/v1/memory/:offset/:len", ReadMemoryCommand::Get);
	server.Get("/api/v1/memory/:segment/:offset/:len", ReadMemoryCommand::Get);
	server.Put("/api/v1/memory/:offset", WriteMemoryCommand::Put);
	server.Put("/api/v1/memory/:segment/:offset", WriteMemoryCommand::Put);

	server.Post("/api/v1/input/key",
	            DarklandsKeyCommand::Post);

	server.Post("/api/v1/input/mouse/move",
	            DarklandsMouseMoveCommand::Post);

	server.Post("/api/v1/input/mouse/button",
	            DarklandsMouseButtonCommand::Post);


	// DARKTEXT_LIVE_FRAME: native emulated frame, independent of host window.
	server.Get("/api/v1/video/frame",
	           [](const httplib::Request&, httplib::Response& res) {
		           const auto ppm = CAPTURE_GetLiveFramePpm();
		           if (ppm.empty()) {
			           res.status = 503;
			           res.set_content("No indexed video frame is available yet",
			                           "text/plain");
			           return;
		           }
		           res.set_header("Cache-Control", "no-store");
		           res.set_content(ppm, "image/x-portable-pixmap");
	           });
}

static std::string strip_port(const std::string& host)
{
	// IPv6 literal: [::1]:8080
	if (host.size() > 1 && host[0] == '[') {
		const auto bracket = host.rfind(']');
		if (bracket != std::string::npos) {
			return host.substr(0, bracket + 1);
		}
		return host;
	}

	// IPv4 or hostname: 127.0.0.1:8080
	const auto colon = host.rfind(':');
	if (colon != std::string::npos) {
		return host.substr(0, colon);
	}
	return host;
}

static void setup_host_validation(const std::string& addr, int port)
{
	// Build the set of allowed Host header values to prevent DNS
	// rebinding attacks. A rebound domain would not match any of these.
	std::set<std::string> allowed;

	const auto port_str = ":" + std::to_string(port);

	auto add = [&](const std::string& hostname) {
		allowed.emplace(hostname);
		allowed.emplace(hostname + port_str);
	};

	add(addr);

	if (addr == "127.0.0.1" || addr == "0.0.0.0") {
		add("localhost");
	}
	if (addr == "::1" || addr == "::") {
		add("localhost");
		add("[::1]");
	}

	server.set_pre_routing_handler(
	        [allowed = std::move(allowed)](const httplib::Request& req,
	                                       httplib::Response& res) {
		        const auto host = strip_port(req.get_header_value("Host"));

		        if (allowed.find(host) == allowed.end()) {
			        LOG_WARNING("WEBSERVER: Rejected request with Host header '%s'",
			                    req.get_header_value("Host").c_str());

			        res.status = httplib::StatusCode::Forbidden_403;
			        res.set_content("Forbidden", "text/plain");

			        return httplib::Server::HandlerResponse::Handled;
		        }
		        return httplib::Server::HandlerResponse::Unhandled;
	        });
}

static void run(const std::string addr, const int port, const std::string resource_home)
{
	const auto config_home = (get_config_dir() / DefaultWebserverDir).string();

	server.set_mount_point("/", config_home);
	server.set_mount_point("/", resource_home);

	setup_api_handlers();
	setup_host_validation(addr, port);

	server.set_exception_handler(error_handler);

	server.Get("/api/v1/dosbox/info", [=](auto, auto& res) {
		json j;
		j["configHome"]      = get_config_dir();
		j["configWebserver"] = config_home;
		j["version"]         = DOSBOX_GetDetailedVersion();

		send_json(res, j);
	});

	LOG_INFO("WEBSERVER: Starting HTTP REST API on http://%s:%d",
	         addr.c_str(),
	         port);

	LOG_INFO("WEBSERVER: Using document root directory '%s'",
	         config_home.c_str());

	auto ok = server.listen(addr, port);
	if (!ok) {
		LOG_WARNING("WEBSERVER: Failed to bind to %s:%d", addr.c_str(), port);
	}
}

static void init_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto enabled = section.AddBool("webserver_enabled", OnlyAtStart, false);
	enabled->SetHelp(
	        "Enable the HTTP REST API that exposes internal state and memory (disabled by\n"
	        "default). Open http://localhost:8086 in a browser (or use the configured port)\n"
	        "to view the API documentation.");
	auto bind_ip = section.AddString("webserver_bind_address",
	                                 OnlyAtStart,
	                                 "127.0.0.1");
	bind_ip->SetHelp(
	        "Bind to the given IP address. This API gives full control over DOSBox, do not\n"
	        "ever expose this to untrusted hosts.\n"
	        "\n"
	        "By default only local connections are allowed.");

	auto bind_port = section.AddInt("webserver_port", OnlyAtStart, 8086);
	bind_port->SetMinMax(1, 0xFFFF);
	bind_port->SetHelp("TCP port to bind to.");
}

} // namespace Webserver

static bool is_webserver_enabled = false;

void WEBSERVER_Init()
{
	auto section = get_section("webserver");

	if (section->GetBool("webserver_enabled")) {
		is_webserver_enabled = true;
		CAPTURE_SetLiveFrameEnabled(true);

		const auto addr = section->GetString("webserver_bind_address");
		const auto port = section->GetInt("webserver_port");
		const auto resource_home = get_resource_path("webserver").string();

		std::thread thread(Webserver::run, addr, port, resource_home);

		thread.detach();
	}
}

void WEBSERVER_Destroy()
{
	CAPTURE_SetLiveFrameEnabled(false);
	Webserver::server.stop();
}

void WEBSERVER_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	auto section = conf->AddSection("webserver");

	Webserver::init_config_settings(*section);
}

bool WEBSERVER_IsEnabled()
{
	return is_webserver_enabled;
}
