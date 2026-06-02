// Mumble Voice Overlay -- native Mumble plugin.
//
// This plugin does not draw anything. Mumble's built-in overlay injects into
// the game's rendering pipeline (OpenGL/D3D hooking), which anti-cheat systems
// such as Star Citizen's EAC flag and ban. To stay completely out of the game
// process, this plugin only *observes* Mumble (who is talking, who is in your
// channel, what Mumble knows about them) and streams that state as tiny
// newline-delimited JSON datagrams over localhost UDP. A separate overlay
// process (see ../overlay) listens on that port and renders an always-on-top
// transparent window the desktop compositor draws over the game -- no injection,
// nothing EAC objects to.
//
// The wire protocol is documented in ../../docs/PROTOCOL.md.

#include "MumblePlugin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#	include <winsock2.h>
#	include <ws2tcpip.h>
#	pragma comment(lib, "Ws2_32.lib")
using socket_t = SOCKET;
static const socket_t INVALID_SOCK = INVALID_SOCKET;
#else
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <unistd.h>
using socket_t = int;
static const socket_t INVALID_SOCK = -1;
#endif

namespace {

// ---- Plugin identity ----------------------------------------------------

constexpr const char *PLUGIN_NAME    = "Mumble Voice Overlay (Star Citizen)";
constexpr const char *PLUGIN_AUTHOR  = "Mumble Voice Overlay";
constexpr const char *PLUGIN_DESC    = "Streams Mumble talking state to an EAC-safe out-of-process voice overlay over localhost UDP.";
constexpr const char *DEFAULT_HOST   = "127.0.0.1";
constexpr uint16_t DEFAULT_PORT      = 27812;
constexpr int PROTOCOL_VERSION       = 1;

// ---- Global state -------------------------------------------------------

struct PluginState {
	std::mutex mutex;
	mumble_plugin_id_t id = 0;
	mumble_api_t api{};
	bool apiValid = false;

	socket_t sock = INVALID_SOCK;
	struct sockaddr_in dest{};

	// Cached per-connection context. Guarded by `mutex` because the
	// onServerConnected/onServerDisconnected callbacks run on a different
	// thread than the talking-state / channel callbacks.
	mumble_connection_t connection = -1;
	mumble_userid_t localUser = 0;
	bool synchronized = false;

	std::vector<mumble_userid_t> known;  // users we've already described to the overlay
};

PluginState g;

// ---- Small helpers ------------------------------------------------------

uint16_t configuredPort() {
	if (const char *raw = std::getenv("MUMBLE_VOICE_OVERLAY_PORT")) {
		long value = std::strtol(raw, nullptr, 10);
		if (value > 0 && value < 65536) {
			return static_cast< uint16_t >(value);
		}
	}
	return DEFAULT_PORT;
}

std::string configuredHost() {
	if (const char *raw = std::getenv("MUMBLE_VOICE_OVERLAY_HOST")) {
		if (raw[0] != '\0') {
			return std::string(raw);
		}
	}
	return std::string(DEFAULT_HOST);
}

// Append `value` to `out` as a JSON string literal (quotes + escaping).
void appendJsonString(std::string &out, const char *value) {
	out.push_back('"');
	if (value) {
		for (const char *p = value; *p; ++p) {
			unsigned char c = static_cast< unsigned char >(*p);
			switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20) {
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						// UTF-8 bytes (>=0x20) pass through unchanged.
						out.push_back(static_cast< char >(c));
					}
			}
		}
	}
	out.push_back('"');
}

void appendKeyString(std::string &out, const char *key, const char *value) {
	appendJsonString(out, key);
	out.push_back(':');
	appendJsonString(out, value);
}

void appendKeyInt(std::string &out, const char *key, long long value) {
	appendJsonString(out, key);
	out.push_back(':');
	out += std::to_string(value);
}

void appendKeyBool(std::string &out, const char *key, bool value) {
	appendJsonString(out, key);
	out += value ? ":true" : ":false";
}

// Fire-and-forget a single JSON object as one UDP datagram. Never blocks
// Mumble: the socket is non-blocking and we ignore send errors entirely (the
// overlay simply may not be running, which is fine).
void emit(const std::string &json) {
	std::lock_guard< std::mutex > lock(g.mutex);
	if (g.sock == INVALID_SOCK) {
		return;
	}
	std::string line = json;
	line.push_back('\n');
#if defined(_WIN32)
	sendto(g.sock, line.c_str(), static_cast< int >(line.size()), 0,
		   reinterpret_cast< struct sockaddr * >(&g.dest), sizeof(g.dest));
#else
	sendto(g.sock, line.c_str(), line.size(), 0, reinterpret_cast< struct sockaddr * >(&g.dest),
		   sizeof(g.dest));
#endif
}

// ---- Mumble API convenience wrappers -----------------------------------
//
// API getters that return strings allocate memory we must hand back via
// api.freeMemory. These helpers copy into a std::string and free immediately.

bool apiReady() {
	return g.apiValid && g.id != 0;
}

std::string userName(mumble_connection_t connection, mumble_userid_t user) {
	std::string result;
	if (!g.api.getUserName) {
		return result;
	}
	const char *raw = nullptr;
	if (g.api.getUserName(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
		result = raw;
		g.api.freeMemory(g.id, raw);
	}
	return result;
}

std::string userComment(mumble_connection_t connection, mumble_userid_t user) {
	std::string result;
	if (!g.api.getUserComment) {
		return result;
	}
	const char *raw = nullptr;
	if (g.api.getUserComment(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
		result = raw;
		g.api.freeMemory(g.id, raw);
	}
	return result;
}

std::string userHash(mumble_connection_t connection, mumble_userid_t user) {
	std::string result;
	if (!g.api.getUserHash) {
		return result;
	}
	const char *raw = nullptr;
	if (g.api.getUserHash(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
		result = raw;
		g.api.freeMemory(g.id, raw);
	}
	return result;
}

std::string channelNameOfUser(mumble_connection_t connection, mumble_userid_t user, mumble_channelid_t *outId) {
	std::string result;
	if (!g.api.getChannelOfUser || !g.api.getChannelName) {
		return result;
	}
	mumble_channelid_t channel = -1;
	if (g.api.getChannelOfUser(g.id, connection, user, &channel) != MUMBLE_EC_OK) {
		return result;
	}
	if (outId) {
		*outId = channel;
	}
	const char *raw = nullptr;
	if (g.api.getChannelName(g.id, connection, channel, &raw) == MUMBLE_EC_OK && raw) {
		result = raw;
		g.api.freeMemory(g.id, raw);
	}
	return result;
}

bool userLocallyMuted(mumble_connection_t connection, mumble_userid_t user) {
	if (!g.api.isUserLocallyMuted) {
		return false;
	}
	bool muted = false;
	g.api.isUserLocallyMuted(g.id, connection, user, &muted);
	return muted;
}

const char *talkingStateName(mumble_talking_state_t state) {
	switch (state) {
		case MUMBLE_TS_PASSIVE: return "passive";
		case MUMBLE_TS_TALKING: return "talking";
		case MUMBLE_TS_WHISPERING: return "whispering";
		case MUMBLE_TS_SHOUTING: return "shouting";
		case MUMBLE_TS_TALKING_MUTED: return "muted";
		default: return "invalid";
	}
}

const char *transmissionModeName(mumble_transmission_mode_t mode) {
	switch (mode) {
		case MUMBLE_TM_CONTINOUS: return "continuous";
		case MUMBLE_TM_VOICE_ACTIVATION: return "voice-activation";
		case MUMBLE_TM_PUSH_TO_TALK: return "push-to-talk";
		default: return "unknown";
	}
}

// ---- Message builders ---------------------------------------------------

// Emit a full description of a user (everything Mumble exposes about them).
void emitUser(mumble_connection_t connection, mumble_userid_t user, bool markKnown) {
	mumble_channelid_t channelId = -1;
	std::string name    = userName(connection, user);
	std::string channel = channelNameOfUser(connection, user, &channelId);
	std::string comment = userComment(connection, user);
	std::string hash    = userHash(connection, user);

	std::string json = "{";
	appendKeyString(json, "t", "user");
	json.push_back(',');
	appendKeyInt(json, "id", user);
	json.push_back(',');
	appendKeyString(json, "name", name.c_str());
	json.push_back(',');
	appendKeyString(json, "channel", channel.c_str());
	json.push_back(',');
	appendKeyInt(json, "channelId", channelId);
	json.push_back(',');
	appendKeyString(json, "comment", comment.c_str());
	json.push_back(',');
	appendKeyString(json, "hash", hash.c_str());
	json.push_back(',');
	appendKeyBool(json, "locallyMuted", userLocallyMuted(connection, user));
	json.push_back(',');
	appendKeyBool(json, "self", user == g.localUser);
	json.push_back('}');
	emit(json);

	if (markKnown) {
		std::lock_guard< std::mutex > lock(g.mutex);
		g.known.push_back(user);
	}
}

bool isKnown(mumble_userid_t user) {
	std::lock_guard< std::mutex > lock(g.mutex);
	for (mumble_userid_t u : g.known) {
		if (u == user) {
			return true;
		}
	}
	return false;
}

void emitTalk(mumble_userid_t user, mumble_talking_state_t state) {
	std::string json = "{";
	appendKeyString(json, "t", "talk");
	json.push_back(',');
	appendKeyInt(json, "id", user);
	json.push_back(',');
	appendKeyString(json, "state", talkingStateName(state));
	json.push_back('}');
	emit(json);
}

void emitSelf(mumble_connection_t connection) {
	bool muted = false, deafened = false;
	mumble_transmission_mode_t mode = MUMBLE_TM_CONTINOUS;
	if (g.api.isLocalUserMuted) {
		g.api.isLocalUserMuted(g.id, &muted);
	}
	if (g.api.isLocalUserDeafened) {
		g.api.isLocalUserDeafened(g.id, &deafened);
	}
	if (g.api.getLocalUserTransmissionMode) {
		g.api.getLocalUserTransmissionMode(g.id, &mode);
	}

	std::string json = "{";
	appendKeyString(json, "t", "self");
	json.push_back(',');
	appendKeyInt(json, "id", g.localUser);
	json.push_back(',');
	appendKeyBool(json, "muted", muted);
	json.push_back(',');
	appendKeyBool(json, "deafened", deafened);
	json.push_back(',');
	appendKeyString(json, "mode", transmissionModeName(mode));
	(void) connection;
	json.push_back('}');
	emit(json);
}

void emitServer(const char *event) {
	std::string json = "{";
	appendKeyString(json, "t", "server");
	json.push_back(',');
	appendKeyString(json, "event", event);
	json.push_back('}');
	emit(json);
}

void emitMeta(const char *event) {
	std::string json = "{";
	appendKeyString(json, "t", "meta");
	json.push_back(',');
	appendKeyString(json, "event", event);
	json.push_back(',');
	appendKeyInt(json, "protocol", PROTOCOL_VERSION);
	json.push_back(',');
	appendKeyString(json, "plugin", PLUGIN_NAME);
	json.push_back('}');
	emit(json);
}

// Describe every user currently in the local user's channel. Used right after
// synchronizing and whenever the local user changes channel, so the overlay
// always knows the roster of people it might hear.
void emitChannelRoster(mumble_connection_t connection) {
	if (!g.api.getChannelOfUser || !g.api.getUsersInChannel) {
		return;
	}
	mumble_channelid_t channel = -1;
	if (g.api.getChannelOfUser(g.id, connection, g.localUser, &channel) != MUMBLE_EC_OK) {
		return;
	}
	mumble_userid_t *users = nullptr;
	size_t count          = 0;
	if (g.api.getUsersInChannel(g.id, connection, channel, &users, &count) != MUMBLE_EC_OK) {
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		emitUser(connection, users[i], true);
	}
	if (users) {
		g.api.freeMemory(g.id, users);
	}
}

// ---- Socket lifecycle ---------------------------------------------------

void openSocket() {
#if defined(_WIN32)
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return;
	}
#endif
	socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCK) {
		return;
	}

#if defined(_WIN32)
	u_long nonblocking = 1;
	ioctlsocket(s, FIONBIO, &nonblocking);
#else
	// Best-effort non-blocking; not strictly required for localhost UDP.
	int flags = 0;
	(void) flags;
#endif

	struct sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port   = htons(configuredPort());
	std::string host = configuredHost();
	if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
		inet_pton(AF_INET, DEFAULT_HOST, &dest.sin_addr);
	}

	std::lock_guard< std::mutex > lock(g.mutex);
	g.sock = s;
	g.dest = dest;
}

void closeSocket() {
	std::lock_guard< std::mutex > lock(g.mutex);
	if (g.sock != INVALID_SOCK) {
#if defined(_WIN32)
		closesocket(g.sock);
		WSACleanup();
#else
		close(g.sock);
#endif
		g.sock = INVALID_SOCK;
	}
}

}  // namespace

// =========================================================================
//  Mandatory plugin functions
// =========================================================================

mumble_error_t mumble_init(mumble_plugin_id_t id) {
	g.id = id;
	openSocket();
	emitMeta("hello");
	return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
	emitMeta("bye");
	closeSocket();
}

struct MumbleStringWrapper mumble_getName() {
	MumbleStringWrapper wrapper;
	wrapper.data           = PLUGIN_NAME;
	wrapper.size           = std::strlen(PLUGIN_NAME);
	wrapper.needsReleasing = false;
	return wrapper;
}

mumble_version_t mumble_getAPIVersion() {
	// Target the original (1.0.0) plugin API. Every getter this plugin uses is
	// part of that revision, and because the API struct is append-only, casting
	// a newer struct to it and reading only the 1.0.x prefix is safe. This keeps
	// the plugin loadable on the widest range of Mumble releases (1.4.0+).
	mumble_version_t version;
	version.major = 1;
	version.minor = 0;
	version.patch = 0;
	return version;
}

void mumble_registerAPIFunctions(void *apiStruct) {
	if (apiStruct) {
		g.api      = *reinterpret_cast< mumble_api_t * >(apiStruct);
		g.apiValid = true;
	}
}

void mumble_releaseResource(const void *) {
	// This plugin only ever returns static strings (needsReleasing == false),
	// so there is never anything for Mumble to ask us to free.
}

// =========================================================================
//  General information functions
// =========================================================================

void mumble_setMumbleInfo(mumble_version_t, mumble_version_t, mumble_version_t) {}

mumble_version_t mumble_getVersion() {
	mumble_version_t version;
	version.major = 1;
	version.minor = 0;
	version.patch = 0;
	return version;
}

struct MumbleStringWrapper mumble_getAuthor() {
	MumbleStringWrapper wrapper;
	wrapper.data           = PLUGIN_AUTHOR;
	wrapper.size           = std::strlen(PLUGIN_AUTHOR);
	wrapper.needsReleasing = false;
	return wrapper;
}

struct MumbleStringWrapper mumble_getDescription() {
	MumbleStringWrapper wrapper;
	wrapper.data           = PLUGIN_DESC;
	wrapper.size           = std::strlen(PLUGIN_DESC);
	wrapper.needsReleasing = false;
	return wrapper;
}

uint32_t mumble_getFeatures() {
	return MUMBLE_FEATURE_NONE;
}

// =========================================================================
//  Event callbacks
// =========================================================================

void mumble_onServerConnected(mumble_connection_t connection) {
	// Runs on a different thread; just record the connection. Real work waits
	// for synchronization, after which API getters are valid.
	std::lock_guard< std::mutex > lock(g.mutex);
	g.connection   = connection;
	g.synchronized = false;
	g.known.clear();
}

void mumble_onServerDisconnected(mumble_connection_t) {
	{
		std::lock_guard< std::mutex > lock(g.mutex);
		g.connection   = -1;
		g.synchronized = false;
		g.known.clear();
	}
	emitServer("disconnected");
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
	if (!apiReady()) {
		return;
	}
	{
		std::lock_guard< std::mutex > lock(g.mutex);
		g.connection   = connection;
		g.synchronized = true;
		g.known.clear();
	}
	if (g.api.getLocalUserID) {
		mumble_userid_t local = 0;
		if (g.api.getLocalUserID(g.id, connection, &local) == MUMBLE_EC_OK) {
			std::lock_guard< std::mutex > lock(g.mutex);
			g.localUser = local;
		}
	}
	emitServer("synchronized");
	emitSelf(connection);
	emitChannelRoster(connection);
}

void mumble_onChannelEntered(mumble_connection_t connection, mumble_userid_t userID,
							  mumble_channelid_t /*previousChannelID*/, mumble_channelid_t /*newChannelID*/) {
	if (!apiReady()) {
		return;
	}
	// When the local user moves, the whole roster of who we can hear changes,
	// so re-describe everyone in the new channel. Otherwise just (re)describe
	// the single user who entered our awareness.
	if (userID == g.localUser) {
		emitChannelRoster(connection);
	} else {
		emitUser(connection, userID, true);
	}
}

void mumble_onChannelExited(mumble_connection_t /*connection*/, mumble_userid_t userID,
							 mumble_channelid_t /*channelID*/) {
	std::string json = "{";
	appendKeyString(json, "t", "leave");
	json.push_back(',');
	appendKeyInt(json, "id", userID);
	json.push_back('}');
	emit(json);

	std::lock_guard< std::mutex > lock(g.mutex);
	for (auto it = g.known.begin(); it != g.known.end(); ++it) {
		if (*it == userID) {
			g.known.erase(it);
			break;
		}
	}
}

void mumble_onUserTalkingStateChanged(mumble_connection_t connection, mumble_userid_t userID,
									  mumble_talking_state_t talkingState) {
	if (!apiReady()) {
		return;
	}
	// Make sure the overlay has this user's details before (or alongside) the
	// first talk event, in case we never saw them enter the channel.
	if (!isKnown(userID)) {
		emitUser(connection, userID, true);
	}
	emitTalk(userID, talkingState);
}
