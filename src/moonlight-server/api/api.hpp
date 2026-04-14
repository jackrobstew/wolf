#pragma once

#include <api/http_server.hpp>
#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <moonlight/control.hpp>
#include <state/data-structures.hpp>

namespace wolf::api {

using namespace wolf::core;

void start_server(std::string_view runtime_dir, immer::box<state::AppState> app_state);

struct PendingPairClient {
  std::string pair_secret;
  rfl::Description<"The IP of the remote Moonlight client", std::string> client_ip;
};

struct PairRequest {
  std::string pair_secret;
  rfl::Description<"The PIN created by the remote Moonlight client", std::string> pin;
};

struct UnpairClientRequest {
  rfl::Description<"The client ID to unpair", std::string> client_id;
};

struct GenericSuccessResponse {
  bool success = true;
};

struct GenericErrorResponse {
  bool success = false;
  std::string error;
};

struct PendingPairRequestsResponse {
  bool success = true;
  std::vector<PendingPairClient> requests;
};

struct PairedClient {
  std::string client_id;
  std::string app_state_folder;
  config::ClientSettings settings = {};
};

struct PairedClientsResponse {
  bool success = true;
  std::vector<PairedClient> clients;
};

struct PartialClientSettings {
  std::optional<uint> run_uid;
  std::optional<uint> run_gid;
  std::optional<std::vector<wolf::config::ControllerType>> controllers_override;
  std::optional<float> mouse_acceleration;
  std::optional<float> v_scroll_acceleration;
  std::optional<float> h_scroll_acceleration;
};

struct UpdateClientSettingsRequest {
  rfl::Description<"The client ID to identify the client (derived from certificate)", std::string> client_id;
  rfl::Description<"New app state folder path (optional)", std::optional<std::string>> app_state_folder;
  rfl::Description<"Client settings to update (only specified fields will be updated)",
                   std::optional<PartialClientSettings>>
      settings;
};

struct AppListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::App>::ReflType> apps;
};

struct AppDeleteRequest {
  std::string id;
};

struct ProfileListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::Profile>::ReflType> profiles;
};

struct ProfileRemoveRequest {
  std::string id;
};

struct StreamSessionCreated {
  bool success = true;
  std::string session_id;
};

struct StreamSessionListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::StreamSession>::ReflType> sessions;
};

struct StreamSessionStartRequest {
  std::string session_id;

  events::VideoSession video_session;
  events::AudioSession audio_session;
};

struct StreamSessionPauseRequest {
  std::string session_id;
};

struct StreamSessionStopRequest {
  std::string session_id;
};

struct StreamSessionHandleInputRequest {
  std::string session_id;
  rfl::Description<"A HEX encoded Moonlight input packet, for the full format see: "
                   "games-on-whales.github.io/wolf/stable/protocols/input-data.html",
                   std::string>
      input_packet_hex;
};

struct CreateLobbyRequest {
  rfl::Description<"The profile that originally created the lobby", std::string> profile_id;
  std::string name;
  std::optional<std::string> icon_png_path;
  bool multi_user = true;
  rfl::Description<"If present, the pin that is required to join the lobby."
                   "If this is not set, then the lobby is open to everyone",
                   std::optional<std::vector<short>>>
      pin;
  bool stop_when_everyone_leaves = true;

  events::VideoSettings video_settings;
  events::AudioSettings audio_settings;

  rfl::Description<"Client settings to update (only specified fields will be updated)",
                   std::optional<PartialClientSettings>>
      client_settings;

  std::string runner_state_folder;
  events::RunnerTypes runner;
};

struct LobbiesResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::Lobby>::ReflType> lobbies;
};

struct LobbyCreateResponse {
  bool success = true;
  std::string lobby_id;
};

struct RunnerStartRequest {
  bool stop_stream_when_over;
  events::RunnerTypes runner;
  std::string session_id;
};

struct GetIconRequest {
  std::string icon_png_path;
};

struct GetIconResponse {
  bool success = true;
  std::string icon_base64;
};

struct DockerPullImageRequest {
  std::string image_name;
};

struct DockerPullImageResponse {
  bool success = true;
};

/**
 * Controller Hub API types
 */
struct HubCreateRequest {
  int num_slots = 4;
  std::string session_id; // The Moonlight session that owns the hub
};

struct HubCreateResponse {
  bool success = true;
  int num_slots;
};

struct HubPairRequest {
  std::string session_id;
  int controller_number; // Moonlight controller index
  int slot_number;       // 1-based slot
};

struct HubUnpairRequest {
  std::string session_id;
  int slot_number;
};

struct HubRouteRequest {
  std::string session_id;
  int slot_number;
  std::string target_session_id;
};

struct HubSwapRequest {
  std::string session_id;
  int slot_a;
  int slot_b;
};

struct HubSlotInfo {
  int slot_number;
  std::optional<int> paired_controller;
  std::string target_session_id;
  bool physical_connected;
};

struct HubStatusResponse {
  bool success = true;
  std::vector<HubSlotInfo> slots;
  std::vector<int> unpaired_controllers;
  std::vector<int> active_controllers; // controllers with input in last 500ms
};

/**
 * Party Mode API types
 */
struct PartyStartRequest {
  std::string session_id;
  std::vector<std::string> lobby_ids;
  std::string layout; // "2p", "3p", "4p"
};

struct PartyStatusResponse {
  bool success = true;
  bool active = false;
  std::string layout;
  std::string tile_aspect;
  std::vector<std::string> lobby_ids;
  int focused_slot = 1;
};

/**
 * Party Spawn API types — one-click party mode
 */
struct PartyPlayerConfig {
  std::string profile_id;
  std::string app_title;
};

struct PartySpawnRequest {
  std::string session_id;
  std::string layout;
  std::vector<PartyPlayerConfig> players;
  bool adaptive = false; // true: lobbies render at tile resolution (fills screen). false: 1080p + letterbox.
  std::string tile_aspect = "16:9"; // "16:9" (default) or "4:3" (retro — tiles constrained to 4:3)
  int controllers_per_player = 1; // 1 = one controller per lobby (default). 2+ = local splitscreen per lobby.
};

struct PartySpawnResponse {
  bool success = true;
  std::vector<std::string> lobby_ids;
};

struct PartyFocusRequest {
  int slot_number = 1;                     // 1-based slot index in active party layout
  std::optional<std::string> session_id;   // optional stream session to retarget; defaults to active session
};

struct PartyFocusResponse {
  bool success = true;
  std::string session_id;
  int focused_slot = 1;
  std::string lobby_id;
};

struct UnixSocket {
  boost::asio::local::stream_protocol::socket socket;
  bool is_alive = true;
};

class UnixSocketServer {
public:
  UnixSocketServer(boost::asio::io_context &io_context,
                   const std::string &socket_path,
                   immer::box<state::AppState> app_state);

  UnixSocketServer(const UnixSocketServer &) = default;

  void broadcast_event(const std::string &event_type, const std::string &event_json);

private:
  void endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Lobbies(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyCreate(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyJoin(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyLeave(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_RunnerStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_HubCreate(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_HubPair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_HubUnpair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_HubRoute(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_HubSwap(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_HubStatus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_PartyStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PartyStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PartyStatus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PartySpawn(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PartyFocus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void sse_broadcast(const std::string &payload);
  void sse_keepalive(const boost::system::error_code &e);

  void send_http(std::shared_ptr<UnixSocket> socket, int status_code, std::string_view body);
  void send_http(std::shared_ptr<UnixSocket> socket,
                 int status_code,
                 const std::vector<std::string_view> &http_headers,
                 std::string_view body);
  void send_data(std::shared_ptr<UnixSocket> socket, std::string_view data);

  void handle_request(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void start_connection(std::shared_ptr<UnixSocket> socket);
  void start_accept();

  void cleanup_sockets();
  void close(UnixSocket &socket);

  struct UnixSocketState {
    UnixSocketState(boost::asio::io_context &io_context, immer::box<state::AppState> app_state, std::string socket_path)
        : io_context(io_context), app_state(app_state),
          acceptor(io_context, boost::asio::local::stream_protocol::endpoint(socket_path)),
          http(HTTPServer<std::shared_ptr<UnixSocket>>{}), sse_keepalive_timer(boost::asio::steady_timer{io_context}) {}

    boost::asio::io_context &io_context;
    immer::box<state::AppState> app_state;
    boost::asio::local::stream_protocol::acceptor acceptor;
    std::vector<std::shared_ptr<UnixSocket>> sockets;
    HTTPServer<std::shared_ptr<UnixSocket>> http;
    boost::asio::steady_timer sse_keepalive_timer;
  };

  std::shared_ptr<UnixSocketState> state_;
};

} // namespace wolf::api
