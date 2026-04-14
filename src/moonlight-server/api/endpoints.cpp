#include <algorithm>
#include <api/api.hpp>
#include <control/controller_hub.hpp>
#include <streaming/party_compositor.hpp>
#include <streaming/session_overlay.hpp>
#include <thread>
#include <control/input_handler.hpp>
#include <core/docker.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

void UnixSocketServer::endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // curl -N --unix-socket /tmp/wolf.sock http://localhost/api/v1/events
  state_->sockets.push_back(socket);
  send_http(socket,
            200,
            {{"Content-Type: text/event-stream"}, {"Connection: keep-alive"}, {"Cache-Control: no-cache"}},
            ""); // Inform clients this is going to be SS
}

void UnixSocketServer::endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto requests = std::vector<PendingPairClient>();
  for (auto [secret, pair_request] : *(state_->app_state)->pairing_atom->load()) {
    requests.push_back({.pair_secret = secret, .client_ip = pair_request->client_ip});
  }
  send_http(socket, 200, rfl::json::write(PendingPairRequestsResponse{.requests = requests}));
}

void UnixSocketServer::endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<PairRequest>(req.body);
  if (event) {
    if (auto pair_request = state_->app_state->pairing_atom->load()->find(event.value().pair_secret)) {
      pair_request->get().user_pin->set_value(event.value().pin.value()); // Resolve the promise
      state_->app_state->pairing_atom->update(
          [pair_secret = event.value().pair_secret](auto pairing_map) { return pairing_map.erase(pair_secret); });
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid pair secret: {}", event.value().pair_secret);
      auto res = GenericErrorResponse{.error = "Invalid pair secret"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = PairedClientsResponse{.success = true};
  auto clients = state_->app_state->config->paired_clients->load();
  for (const config::PairedClient &client : clients.get()) {
    res.clients.push_back(PairedClient{.client_id = std::to_string(state::get_client_id(client)),
                                       .app_state_folder = client.app_state_folder,
                                       .settings = client.settings});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  try {
    auto payload_result = rfl::json::read<UnpairClientRequest>(req.body);
    if (!payload_result) {
      auto res = GenericErrorResponse{.error = "Invalid request format"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    const auto &payload = payload_result.value(); // Unwrap the Result
    auto client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
    if (!client) {
      auto res = GenericErrorResponse{.error = "Client not found"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    state::unpair(this->state_->app_state->config, *client);

    auto res = GenericSuccessResponse{.success = true};
    send_http(socket, 200, rfl::json::write(res));
  } catch (const std::exception &e) {
    auto res = GenericErrorResponse{.error = e.what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps.push_back(rfl::Reflector<events::App>::to(app, this->state_->app_state->event_bus));
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps | //
                         ranges::views::filter(
                             [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                         ranges::to<immer::vector<immer::box<events::App>>>();
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles.push_back(rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus)));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::remove_if([&p](const immer::box<events::Profile> &profile) {
                                 return profile.get().id == p.id;
                               }) | //
                               ranges::to<state::ProfilesList>());
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    res.sessions.push_back(rfl::Reflector<events::StreamSession>::from(session));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    immer::box<events::App> choosen_app;
    auto ss = session.value();
    if (auto app_id = ss.app_id) {
      auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, *app_id);
      if (!app) {
        logs::log(logs::warning, "[API] Invalid app_id: {}", *app_id);
        auto res = GenericErrorResponse{.error = "Invalid app_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_app = *app;
    } else {
      auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
      if (!moonlight_profile) {
        logs::log(logs::warning, "[API] No moonlight profile found, unable to automatically create an app.");
        auto res = GenericErrorResponse{.error = "No moonlight profile found"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
      immer::box<events::App> sample_app = apps.front();
      choosen_app = events::App{
          .base = {.title = "dummy", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

          .video_producer_buffer_caps = sample_app->video_producer_buffer_caps,

          .h264_gst_pipeline = sample_app->h264_gst_pipeline,
          .hevc_gst_pipeline = sample_app->hevc_gst_pipeline,
          .av1_gst_pipeline = sample_app->av1_gst_pipeline,

          .render_node = sample_app->render_node,
          .opus_gst_pipeline = sample_app->opus_gst_pipeline,
          .start_virtual_compositor = true,
          .start_audio_server = true,

          .runner = std::make_shared<process::RunProcess>(state_->app_state->event_bus,
                                                          "sh -c \"while :; do echo 'running...'; sleep 10; done\"")};
    }

    config::PairedClient choosen_client;
    if (auto client_id = ss.client_id) {
      auto client = state::get_client_by_id(this->state_->app_state->config, *client_id);
      if (!client) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        auto res = GenericErrorResponse{.error = "Invalid client_id"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      choosen_client = *client;
    } else {
      // Create a dummy client
      choosen_client = {.client_cert = "", .app_state_folder = state::gen_uuid(), .settings = {}};
    }

    choosen_client.settings = ss.client_settings.value_or(config::ClientSettings{});

    auto new_session = state::create_stream_session( //
        state_->app_state,
        choosen_app,
        choosen_client,
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv);
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;

    state_->app_state->running_sessions->update(
        [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = std::to_string(new_session->session_id)};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(start_req.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      video_session.session_id = session_id; // Can't be JSON encoded
      if (video_session.render_node.empty()) {
        video_session.render_node = session->app->render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id; // Can't be JSON encoded
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionPauseRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(input_request.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto pkt_parsed = crypto::hex_to_str(hex_pkt);
      control::INPUT_PKT *input_pkt = reinterpret_cast<control::INPUT_PKT *>(pkt_parsed.data());
      control::handle_input(session.value(), {}, input_pkt);

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();
    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get(),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = event.value().video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus)};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(20));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session = state::get_session_by_id(this->state_->app_state->running_sessions->load(),
                                            std::stoul(event.value().session_id));
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload_result = rfl::json::read<UpdateClientSettingsRequest>(req.body);
  if (!payload_result) {
    auto res = GenericErrorResponse{.error = "Invalid request format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  const auto &payload = payload_result.value();
  auto current_client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
  if (!current_client) {
    auto res = GenericErrorResponse{.error = "Client not found"};
    send_http(socket, 404, rfl::json::write(res));
    return;
  }

  // Edit only the settings that are being passed in the payload
  auto current_settings = current_client->settings;
  auto new_settings = payload.settings.get().value_or(PartialClientSettings{});
  auto merged_client = config::PairedClient{
      .client_cert = current_client->client_cert, // Immutable, changing this would mean a new client
      .app_state_folder = payload.app_state_folder.get().value_or(current_client->app_state_folder),
      .settings = config::ClientSettings{
          .run_uid = new_settings.run_gid.value_or(current_settings.run_uid),
          .run_gid = new_settings.run_gid.value_or(current_settings.run_gid),
          .controllers_override = new_settings.controllers_override.value_or(current_settings.controllers_override),
          .mouse_acceleration = new_settings.mouse_acceleration.value_or(current_settings.mouse_acceleration),
          .v_scroll_acceleration = new_settings.v_scroll_acceleration.value_or(current_settings.v_scroll_acceleration),
          .h_scroll_acceleration = new_settings.h_scroll_acceleration.value_or(current_settings.h_scroll_acceleration),
      }};

  update_client_settings(this->state_->app_state->config, std::stoull(payload.client_id.value()), merged_client);

  auto res = GenericSuccessResponse{.success = true};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto icon_path = utils::split(req.query_string, '=');
  if (icon_path.size() != 2 || icon_path[0] != "icon_path") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'icon_path' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }
  // TODO: implement coroutines for CURL
  std::thread([this, socket, icon_path = utils::to_string(icon_path[1])]() {
    if (auto icon = utils::get_icon(this->state_->app_state->host->local_base_state_folder, icon_path)) {
      send_http(socket,
                200,
                {"Content-Length: " + std::to_string(icon->size()), "Content-Type: image/png"},
                icon.value());
    } else {
      auto res = GenericErrorResponse{.error = "Icon not found"};
      send_http(socket, 404, rfl::json::write(res));
    }
  }).detach();
}

void UnixSocketServer::endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto image_name = utils::split(req.query_string, '=');
  if (image_name.size() != 2 || image_name[0] != "image_name") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'image_name' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto response = docker_api.inspect_image(image_name[1])) {
    send_http(socket, 200, response.value());
  } else {
    auto res = GenericErrorResponse{.error = "Image not found"};
    send_http(socket, 404, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_payload = rfl::json::read<DockerPullImageRequest>(req.body);
  if (input_payload) {
    // TODO: implement coroutines for CURL
    std::thread([this, socket, image = input_payload.value().image_name]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      bool first_send = true;
      broadcast_event("DockerPullImageStartEvent",
                      rfl::json::write(events::DockerPullImageStartEvent{.image_name = image}));
      if (docker_api.pull_image(image,
                                {},
                                [this, &first_send, socket](const docker::DockerAPI::DockerProgressEvent &progress_ev) {
                                  if (first_send) {
                                    send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
                                    first_send = false;
                                  }
                                  auto serialized_ev = rfl::json::write(progress_ev) + "\r\n";
                                  send_data(socket, serialized_ev);
                                })) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        auto final_result = rfl::json::write(GenericSuccessResponse{.success = true});
        send_data(socket, final_result + "\r\n");
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = true}));
      } else {
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to pull image"}));
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = false}));
      }
    }).detach();
  }
}

void UnixSocketServer::endpoint_HubCreate(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<HubCreateRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session not found"}));
    return;
  }

  // Determine the correct target for PlugDeviceEvents.
  // If this session is connected to a lobby, we must push devices directly
  // to the lobby's device queue (the event bus handler won't match lobby IDs).
  std::string device_target = payload->session_id;
  std::shared_ptr<events::devices_atom_queue> device_queue = nullptr;
  {
    auto lobbies = state_->app_state->lobbies->load();
    if (auto lobby = state::get_lobby_by_connected_session(lobbies.get(), payload->session_id)) {
      device_target = lobby->id;
      device_queue = lobby->plugged_devices_queue;
      logs::log(logs::info, "[API] HubCreate: session {} is in lobby {}, pushing devices to lobby queue",
                payload->session_id, lobby->id);
    }
  }

  auto hub = control::create_hub(payload->num_slots,
                                  device_target,
                                  state_->app_state->event_bus,
                                  device_queue);
  if (!hub) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to create controller hub"}));
    return;
  }

  // Unplug any existing physical joypads so the game container drops them.
  // The hub's virtual slots will be the ONLY input devices the game sees.
  {
    auto joypads = session->joypads->load();
    for (auto &[controller_number, joypad] : joypads.get()) {
      events::UnplugDeviceEvent unplug_ev{.session_id = device_target};
      std::visit(
          [&unplug_ev](auto &pad) {
            unplug_ev.udev_events = pad.get_udev_events();
            unplug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
          },
          *joypad);
      state_->app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>(unplug_ev));
      logs::log(logs::info, "[API] HubCreate: unplugged old joypad {} from {}", controller_number, device_target);
    }
  }

  // Assign hub into the session's shared pointer (visible to all copies including control thread)
  *session->controller_hub = hub;

  send_http(socket, 200, rfl::json::write(HubCreateResponse{.num_slots = payload->num_slots}));
}

void UnixSocketServer::endpoint_HubPair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<HubPairRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session || !*session->controller_hub) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session or hub not found"}));
    return;
  }

  auto &hub = **session->controller_hub;
  if (control::pair_controller(hub, payload->controller_number, payload->slot_number)) {
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Failed to pair controller"}));
  }
}

void UnixSocketServer::endpoint_HubUnpair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<HubUnpairRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session || !*session->controller_hub) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session or hub not found"}));
    return;
  }

  auto &hub = **session->controller_hub;
  control::unpair_slot(hub, payload->slot_number);
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
}

void UnixSocketServer::endpoint_HubRoute(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<HubRouteRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session || !*session->controller_hub) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session or hub not found"}));
    return;
  }

  auto &hub = **session->controller_hub;
  if (control::route_slot(hub, payload->slot_number, payload->target_session_id, state_->app_state->event_bus)) {
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Failed to route slot"}));
  }
}

void UnixSocketServer::endpoint_HubSwap(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<HubSwapRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session || !*session->controller_hub) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session or hub not found"}));
    return;
  }

  auto &hub = **session->controller_hub;
  control::swap_slots(hub, payload->slot_a, payload->slot_b);
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
}

void UnixSocketServer::endpoint_HubStatus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // Find the session from query string: ?session_id=12345
  auto params = utils::split(req.query_string, '=');
  if (params.size() != 2 || params[0] != "session_id") {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Missing session_id query parameter"}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(std::string(params[1]));
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session not found"}));
    return;
  }

  // Auto-rejoin: if session has no hub but AppState has active party with hub, attach it
  if (!*session->controller_hub) {
    auto active = std::static_pointer_cast<streaming::ActivePartyState>(*state_->app_state->active_party);
    if (active && active->hub) {
      *session->controller_hub = active->hub;
      // Also switch stream to party compositor
      state_->app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>(
          events::SwitchStreamProducerEvents{.session_id = session_id,
                                              .interpipe_src_id = active->party_base_interpipe}));
      logs::log(logs::info, "[HUB] Auto-rejoined session {} to active party hub", session_id);
    } else {
      send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "No hub active"}));
      return;
    }
  }

  auto &hub = **session->controller_hub;
  std::lock_guard<std::mutex> lock(hub.mtx);
  HubStatusResponse res;
  for (const auto &slot : hub.slots) {
    res.slots.push_back(HubSlotInfo{
        .slot_number = slot.slot_number,
        .paired_controller = slot.paired_controller_number,
        .target_session_id = slot.target_session_id,
        .physical_connected = slot.physical_connected});
  }
  res.unpaired_controllers = std::vector<int>(hub.unpaired_controllers.begin(), hub.unpaired_controllers.end());

  // Active controllers = any controller with input in the last 500ms
  auto now = std::chrono::steady_clock::now();
  for (const auto &[ctrl, last] : hub.last_activity) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 500) {
      res.active_controllers.push_back(ctrl);
    }
  }

  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_PartyStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<PartyStartRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session not found"}));
    return;
  }

  if (*session->party_compositor != nullptr) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Party mode already active"}));
    return;
  }

  auto layout_opt = streaming::get_layout(payload->layout);
  if (!layout_opt) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid layout: " + payload->layout}));
    return;
  }
  auto *layout = &*layout_opt;

  if (payload->lobby_ids.size() != layout->tiles.size()) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{
        .error = fmt::format("lobby_ids count ({}) does not match layout tile count ({})",
                             payload->lobby_ids.size(), layout->tiles.size())}));
    return;
  }

  auto result = streaming::start_party(session_id, payload->lobby_ids, *layout, state_->app_state->event_bus,
                                       session->app->render_node, state_->app_state->gst_context);
  if (!result) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to start party compositor"}));
    return;
  }

  *session->party_compositor = result;
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
}

void UnixSocketServer::endpoint_PartyStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // Party state lives on AppState, not StreamSession
  auto active = std::static_pointer_cast<streaming::ActivePartyState>(*state_->app_state->active_party);
  if (!active) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Party mode not active"}));
    return;
  }

  // Find the CURRENT Moonlight session to switch its stream back to Wolf-UI
  auto sessions = state_->app_state->running_sessions->load();
  std::size_t current_session_id = 0;
  for (const auto &session : sessions.get()) {
    current_session_id = session.session_id;

    // Re-route all hub slots back to the main session so controllers work in Wolf-UI.
    // The hub stays active — virtual joypads are re-plugged into the main session container.
    if (active->hub && *session.controller_hub) {
      auto session_id_str = std::to_string(session.session_id);
      std::lock_guard<std::mutex> lock(active->hub->mtx);
      for (auto &slot : active->hub->slots) {
        // Plug virtual joypad into the main session
        events::PlugDeviceEvent plug_ev{.session_id = session_id_str,
                                        .udev_events = slot.udev_events,
                                        .udev_hw_db_entries = slot.udev_hw_db_entries};
        state_->app_state->event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
        slot.target_session_id = session_id_str;
      }
      logs::log(logs::info, "[PARTY] Re-routed {} hub slots back to session {}",
                active->hub->slots.size(), session_id_str);
    }
    break;
  }

  // Clear party state BEFORE stop_party — stop_party fires StopStreamEvent which
  // triggers a reconnect. Must be null so reconnect gets a fresh Wolf-UI.
  *state_->app_state->active_party = nullptr;

  if (current_session_id > 0) {
    streaming::stop_party(active->compositor, current_session_id, state_->app_state->event_bus);
  } else {
    streaming::stop_party(active->compositor, active->original_session_id, state_->app_state->event_bus);
  }
  logs::log(logs::info, "[PARTY] Party stopped from web UI");
  send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
}

void UnixSocketServer::endpoint_PartyStatus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // Party state lives on AppState — no session_id needed
  auto active = std::static_pointer_cast<streaming::ActivePartyState>(*state_->app_state->active_party);
  if (active) {
    send_http(socket, 200, rfl::json::write(PartyStatusResponse{
        .active = true,
        .layout = active->layout_name,
        .tile_aspect = active->tile_aspect,
        .lobby_ids = active->lobby_ids,
        .focused_slot = active->focused_slot}));
  } else {
    send_http(socket, 200, rfl::json::write(PartyStatusResponse{.active = false}));
  }
}

void UnixSocketServer::endpoint_PartyFocus(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<PartyFocusRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  auto active = std::static_pointer_cast<streaming::ActivePartyState>(*state_->app_state->active_party);
  if (!active || !active->compositor) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Party mode not active"}));
    return;
  }

  if (payload->slot_number < 1 || payload->slot_number > static_cast<int>(active->lobby_ids.size())) {
    send_http(socket,
              400,
              rfl::json::write(GenericErrorResponse{
                  .error = fmt::format("slot_number must be between 1 and {}", active->lobby_ids.size())}));
    return;
  }

  std::size_t session_id = 0;
  if (payload->session_id) {
    session_id = std::stoul(*payload->session_id);
  } else {
    session_id = active->original_session_id;
  }

  auto sessions = state_->app_state->running_sessions->load();
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session not found"}));
    return;
  }

  auto lobby_id = active->lobby_ids[payload->slot_number - 1];
  auto lobbies = state_->app_state->lobbies->load();
  auto lobby = state::get_lobby_by_id(lobbies.get(), lobby_id);
  if (!lobby) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Target lobby not found"}));
    return;
  }

  auto wl_state = *lobby->wayland_display->load();
  if (!wl_state) {
    send_http(socket,
              400,
              rfl::json::write(GenericErrorResponse{.error = "Target lobby display is not ready yet"}));
    return;
  }

  session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
  session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
  session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
  active->focused_slot = payload->slot_number;

  logs::log(logs::info,
            "[PARTY] Session {} input focused on slot {} (lobby {})",
            session_id,
            payload->slot_number,
            lobby->id);

  send_http(socket,
            200,
            rfl::json::write(PartyFocusResponse{.session_id = std::to_string(session_id),
                                                .focused_slot = payload->slot_number,
                                                .lobby_id = lobby->id}));
}

void UnixSocketServer::endpoint_PartySpawn(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload = rfl::json::read<PartySpawnRequest>(req.body);
  if (!payload) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = payload.error().what()}));
    return;
  }

  // 1. Find the Moonlight session
  auto sessions = state_->app_state->running_sessions->load();
  auto session_id = std::stoul(payload->session_id);
  auto session = state::get_session_by_id(sessions.get(), session_id);
  if (!session) {
    send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Session not found"}));
    return;
  }

  // If party already active (on AppState), stop it first
  auto old_party = std::static_pointer_cast<streaming::ActivePartyState>(*state_->app_state->active_party);
  if (old_party) {
    logs::log(logs::info, "[PARTY SPAWN] Stopping existing party before starting new one");
    *state_->app_state->active_party = nullptr;
    streaming::stop_party(old_party->compositor, session_id, state_->app_state->event_bus);
    *session->controller_hub = nullptr;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  // 2. Validate layout (with tile aspect ratio)
  auto layout_opt = streaming::get_layout(payload->layout, payload->tile_aspect);
  if (!layout_opt) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{
        .error = fmt::format("Unknown layout '{}'. Options: 2p, 2p-lr, 2p-tb, 3p, 3p-fill, 4p", payload->layout)}));
    return;
  }
  auto *layout = &*layout_opt;
  int player_count = static_cast<int>(layout->tiles.size());
  if (payload->players.size() != static_cast<std::size_t>(player_count)) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{
        .error = fmt::format("Layout '{}' needs {} players, got {}", payload->layout, player_count, payload->players.size())}));
    return;
  }

  // 3. For each player: find profile + app, create lobby
  auto profiles = state_->app_state->config->profiles->load();
  std::vector<std::string> lobby_ids;
  std::vector<std::shared_future<bool>> setup_futures;

  for (std::size_t i = 0; i < payload->players.size(); ++i) {
    const auto &player = payload->players[i];
    const auto &tile = layout->tiles[i];

    // Find profile
    const events::Profile *found_profile = nullptr;
    for (const events::Profile &profile : profiles.get()) {
      if (profile.id == player.profile_id) {
        found_profile = &profile;
        break;
      }
    }
    if (!found_profile) {
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{
          .error = fmt::format("Profile not found: {}", player.profile_id)}));
      return;
    }

    // Find app in profile
    auto apps = found_profile->apps->load();
    const events::App *found_app = nullptr;
    for (const events::App &app : apps.get()) {
      if (app.base.title == player.app_title) {
        found_app = &app;
        break;
      }
    }
    if (!found_app) {
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{
          .error = fmt::format("App '{}' not found in profile '{}'", player.app_title, player.profile_id)}));
      return;
    }

    // Create lobby
    auto lobby_id = state::gen_uuid();
    lobby_ids.push_back(lobby_id);

    // Resolution strategy per app:
    // - Steam: always 1920x1080 (needs standard resolutions, letterbox in tiles)
    // - Non-16:9 tile aspect (e.g. 4:3): ALWAYS adaptive — lobby must match tile aspect
    //   to avoid double-letterboxing (16:9 content in 4:3 tile)
    // - Everything else + adaptive: scale tile to min 1280x720, clamp to session native
    // - Everything else non-adaptive: session native (1080p), letterbox
    bool is_steam = player.app_title.find("Steam") != std::string::npos;
    bool force_adaptive = (payload->tile_aspect != "16:9");
    int lobby_w, lobby_h;
    if (is_steam) {
      // Steam needs standard resolutions — always use session native
      lobby_w = session->display_mode.width;
      lobby_h = session->display_mode.height;
    } else if (payload->adaptive || force_adaptive) {
      float scale = std::max({1280.0f / tile.w, 720.0f / tile.h, 1.0f});
      lobby_w = static_cast<int>(tile.w * scale);
      lobby_h = static_cast<int>(tile.h * scale);
      lobby_w = std::min(lobby_w, session->display_mode.width);
      lobby_h = std::min(lobby_h, session->display_mode.height);
      lobby_w += lobby_w % 2;
      lobby_h += lobby_h % 2;
    } else {
      lobby_w = session->display_mode.width;
      lobby_h = session->display_mode.height;
    }
    auto video_settings = events::VideoSettings{
        .width = lobby_w,
        .height = lobby_h,
        .refresh_rate = session->display_mode.refreshRate,
        .wayland_render_node = found_app->render_node,
        .runner_render_node = found_app->render_node,
        .video_producer_buffer_caps = found_app->video_producer_buffer_caps};

    auto audio_settings = events::AudioSettings{
        .channel_count = session->audio_channel_count};

    auto create_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = player.profile_id,
        .name = fmt::format("party_{}_{}", player.profile_id, player.app_title),
        .stop_when_everyone_leaves = false,
        .video_settings = video_settings,
        .audio_settings = audio_settings,
        .runner_state_folder = fmt::format("profile-data/{}/{}",
                                         player.profile_id,
                                         rfl::get<wolf::config::AppDocker>(found_app->runner->serialize().variant()).name),
        .runner = found_app->runner};

    auto future = create_ev.on_setup_over.get()->get_future();
    setup_futures.push_back(future.share());

    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_ev));
    logs::log(logs::info, "[PARTY SPAWN] Created lobby {} for {}/{}", lobby_id, player.profile_id, player.app_title);
  }

  // 4. Wait for all lobbies to be ready
  for (std::size_t i = 0; i < setup_futures.size(); ++i) {
    auto status = setup_futures[i].wait_for(std::chrono::seconds(30));
    if (status == std::future_status::timeout) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{
          .error = fmt::format("Lobby {} timed out during setup", lobby_ids[i])}));
      return;
    }
  }

  logs::log(logs::info, "[PARTY SPAWN] All {} lobbies ready, starting compositor", lobby_ids.size());

  // 5. Start compositor
  auto compositor = streaming::start_party(session_id, lobby_ids, *layout, state_->app_state->event_bus,
                                            session->app->render_node, state_->app_state->gst_context);
  if (!compositor) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to start party compositor"}));
    return;
  }

  // 6. Controller hub — reuse existing if present (preserves pairings), else create new
  auto lobbies = state_->app_state->lobbies->load();
  std::vector<std::shared_ptr<events::devices_atom_queue>> device_queues;
  for (const auto &lid : lobby_ids) {
    auto lobby = state::get_lobby_by_id(lobbies.get(), lid);
    device_queues.push_back(lobby ? lobby->plugged_devices_queue : nullptr);
  }

  std::shared_ptr<control::ControllerHub> hub;
  if (*session->controller_hub) {
    // Existing hub — re-route slots to lobby containers, preserve pairings
    hub = *session->controller_hub;
    std::lock_guard<std::mutex> lock(hub->mtx);
    for (std::size_t i = 0; i < hub->slots.size() && i < lobby_ids.size(); ++i) {
      auto &slot = hub->slots[i];
      slot.target_session_id = lobby_ids[i];
      events::PlugDeviceEvent plug_ev{.session_id = lobby_ids[i],
                                      .udev_events = slot.udev_events,
                                      .udev_hw_db_entries = slot.udev_hw_db_entries};
      if (i < device_queues.size() && device_queues[i]) {
        device_queues[i]->push(immer::box<events::PlugDeviceEvent>(plug_ev));
      }
    }
    logs::log(logs::info, "[PARTY SPAWN] Existing hub re-routed {} slots to lobbies (pairings preserved)",
              std::min(hub->slots.size(), lobby_ids.size()));
  } else {
    // No existing hub — create new party hub
    // Expand lobby_ids for multi-controller-per-player: [A, B] with cpp=2 → [A, A, B, B]
    int cpp = std::clamp(payload->controllers_per_player, 1, 4);
    std::vector<std::string> expanded_lobby_ids;
    std::vector<std::shared_ptr<events::devices_atom_queue>> expanded_device_queues;
    for (std::size_t i = 0; i < lobby_ids.size(); ++i) {
      for (int c = 0; c < cpp; ++c) {
        expanded_lobby_ids.push_back(lobby_ids[i]);
        expanded_device_queues.push_back(i < device_queues.size() ? device_queues[i] : nullptr);
      }
    }
    hub = control::create_party_hub(expanded_lobby_ids, expanded_device_queues, state_->app_state->event_bus);
    if (hub) {
      *session->controller_hub = hub;
      logs::log(logs::info, "[PARTY SPAWN] New party hub created with {} slots ({} controllers/player)",
                expanded_lobby_ids.size(), cpp);
    }
  }

  // Release all buttons on the original session's virtual joypads so Wolf-UI
  // doesn't see stuck buttons after party mode takes over input routing.
  {
    auto joypads = session->joypads->load();
    for (auto it = joypads->begin(); it != joypads->end(); ++it) {
      std::visit([](auto &pad) {
        pad.set_pressed_buttons(0);
        pad.set_stick(inputtino::Joypad::LS, 0, 0);
        pad.set_stick(inputtino::Joypad::RS, 0, 0);
        pad.set_triggers(0, 0);
      }, *it->second);
    }
    logs::log(logs::info, "[PARTY SPAWN] Released all buttons on original session joypads");
  }

  // 7. Store party state on AppState (persists across reconnections)
  auto party_base = fmt::format("party_{}", session_id);
  auto active_state = std::make_shared<streaming::ActivePartyState>(streaming::ActivePartyState{
      .compositor = compositor,
      .hub = hub,
      .lobby_ids = lobby_ids,
      .layout_name = layout->name,
      .tile_aspect = payload->tile_aspect,
      .party_base_interpipe = party_base,
      .original_session_id = session_id,
      .focused_slot = 1});

  // Set controller offset for the original session (offset 0)
  active_state->next_offset = 100; // next session gets 100
  if (compositor->overlay) {
    compositor->overlay->session_ctrl_offsets[session_id] = 0;
  }

  *state_->app_state->active_party = active_state;
  *session->party_compositor = compositor; // So input handler can access overlay

  // Route pointer/keyboard input to the focused lobby while party mode is active.
  // The stream now shows the party compositor, so keeping input on Wolf-UI makes
  // mouse interactions appear broken (including Moonlight controller mouse emulation).
  if (!lobby_ids.empty()) {
    auto focus_index = std::clamp(active_state->focused_slot - 1, 0, static_cast<int>(lobby_ids.size()) - 1);
    auto all_lobbies = state_->app_state->lobbies->load();
    auto input_lobby = state::get_lobby_by_id(all_lobbies.get(), lobby_ids[focus_index]);
    if (input_lobby) {
      auto wl_state = *input_lobby->wayland_display->load();
      if (wl_state) {
        session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
        session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
        session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
        logs::log(logs::info,
                  "[PARTY SPAWN] Routed session {} input to slot {} (lobby {})",
                  session_id,
                  focus_index + 1,
                  input_lobby->id);
      } else {
        logs::log(logs::warning,
                  "[PARTY SPAWN] Focus lobby {} Wayland display not ready for input routing",
                  input_lobby->id);
      }
    }
  }

  // 8. Wire overlay callbacks — swap and end party actions from the quick-access menu
  if (compositor->overlay) {
    auto ov = compositor->overlay;
    std::weak_ptr<streaming::OverlayState> ov_weak = ov;
    std::weak_ptr<streaming::ActivePartyState> active_state_weak = active_state;

    // Populate player info for overlay display
    {
      std::lock_guard<std::mutex> lock(ov->info_mtx);
      ov->tile_aspect = payload->tile_aspect;
      ov->players.clear();
      for (std::size_t i = 0; i < payload->players.size(); ++i) {
        streaming::OverlayPlayerInfo info;
        info.slot_number = static_cast<int>(i + 1);
        info.profile_name = payload->players[i].profile_id;
        info.app_name = payload->players[i].app_title;
        ov->players.push_back(std::move(info));
      }
    }

    // Swap controller pairings between two slots
    ov->on_swap = [hub](int slot_a, int slot_b) {
      if (hub) {
        control::swap_slots(*hub, slot_a, slot_b);
        logs::log(logs::info, "[OVERLAY] Controller swap: P{} ↔ P{}", slot_a, slot_b);
      }
    };

    // Swap positions (for now same as controller swap — true tile swap is future work)
    ov->on_swap_position = [hub](int slot_a, int slot_b) {
      if (hub) {
        control::swap_slots(*hub, slot_a, slot_b);
        logs::log(logs::info, "[OVERLAY] Position swap: P{} ↔ P{}", slot_a, slot_b);
      }
    };

    // Pair callback — pairs a controller to a slot + updates overlay player info
    ov->on_pair = [hub, ov_weak](int controller_number, int slot_number) {
      if (hub) {
        control::pair_controller(*hub, controller_number, slot_number);
        // Update overlay display so player card shows green dot + controller number
        if (auto ov = ov_weak.lock()) {
          std::lock_guard<std::mutex> lock(ov->info_mtx);
          if (slot_number >= 1 && slot_number <= (int)ov->players.size()) {
            ov->players[slot_number - 1].controller_connected = true;
            ov->players[slot_number - 1].controller_number = controller_number;
          }
        }
        logs::log(logs::info, "[OVERLAY] Pair: ctrl {} → slot {}", controller_number, slot_number);
      }
    };

    // Restart a lobby's game container
    auto ev_bus = state_->app_state->event_bus;
    auto app_state = state_->app_state;
    ov->on_restart_game = [lobby_ids, ev_bus](int slot) {
      if (slot >= 1 && slot <= (int)lobby_ids.size()) {
        auto lobby_id = lobby_ids[slot - 1];
        ev_bus->fire_event(immer::box<events::StopLobbyEvent>(
            events::StopLobbyEvent{.lobby_id = lobby_id}));
        logs::log(logs::info, "[OVERLAY] Restart game for slot {} (lobby {})", slot, lobby_id);
      }
    };

    // Unpair all controllers
    ov->on_unpair_all = [hub, ov_weak]() {
      if (hub) {
        {
          std::lock_guard<std::mutex> lock(hub->mtx);
          for (auto &slot : hub->slots) {
            if (slot.paired_controller_number) {
              hub->unpaired_controllers.insert(*slot.paired_controller_number);
              slot.paired_controller_number = std::nullopt;
              slot.physical_connected = false;
            }
          }
          hub->pairing_table.clear();
        }
        // Update overlay display
        if (auto ov = ov_weak.lock()) {
          std::lock_guard<std::mutex> olock(ov->info_mtx);
          for (auto &p : ov->players) {
            p.controller_connected = false;
            p.controller_number = -1;
          }
        }
        logs::log(logs::info, "[OVERLAY] All controllers unpaired");
      }
    };

    // End party callback
    auto active_party_ptr = state_->app_state->active_party;
    ov->on_end_party = [active_state_weak, session_id, ev_bus, active_party_ptr]() {
      auto active_state = active_state_weak.lock();
      if (!active_state) {
        logs::log(logs::warning, "[OVERLAY] End party requested but active party state is already gone");
        return;
      }
      // Clear party state BEFORE stop_party — stop_party fires StopStreamEvent which
      // triggers a reconnect. The reconnect checks active_party to decide whether to
      // skip Wolf-UI. Must be null so reconnect gets a fresh Wolf-UI.
      *active_party_ptr = nullptr;
      streaming::stop_party(active_state->compositor, session_id, ev_bus);
      logs::log(logs::info, "[OVERLAY] Party ended via quick-access menu");
    };

    // Global/Private toggle — sync overlay flag with ActivePartyState
    ov->on_toggle_global = [active_state_weak](bool global) {
      if (auto active_state = active_state_weak.lock()) {
        active_state->global = global;
        logs::log(logs::info, "[OVERLAY] Party mode: {}", global ? "Global" : "Private");
      }
    };

    // Focus callback — retarget a stream session's mouse/keyboard/touch to slot N
    ov->on_focus_slot = [app_state, active_state_weak](std::size_t target_session_id, int slot_number) {
      auto active_state = active_state_weak.lock();
      if (!active_state || !active_state->compositor) {
        logs::log(logs::warning,
                  "[PARTY] Focus request ignored: no active party (session {}, slot {})",
                  target_session_id,
                  slot_number);
        return;
      }

      if (slot_number < 1 || slot_number > static_cast<int>(active_state->lobby_ids.size())) {
        logs::log(logs::warning,
                  "[PARTY] Focus request out of range: slot {} (valid 1..{})",
                  slot_number,
                  active_state->lobby_ids.size());
        return;
      }

      auto sessions = app_state->running_sessions->load();
      auto target_session = state::get_session_by_id(sessions.get(), target_session_id);
      if (!target_session) {
        logs::log(logs::warning,
                  "[PARTY] Focus request ignored: stream session {} not found",
                  target_session_id);
        return;
      }

      auto lobby_id = active_state->lobby_ids[slot_number - 1];
      auto lobbies = app_state->lobbies->load();
      auto lobby = state::get_lobby_by_id(lobbies.get(), lobby_id);
      if (!lobby) {
        logs::log(logs::warning,
                  "[PARTY] Focus request ignored: lobby {} not found for slot {}",
                  lobby_id,
                  slot_number);
        return;
      }

      auto wl_state = *lobby->wayland_display->load();
      if (!wl_state) {
        logs::log(logs::warning,
                  "[PARTY] Focus request ignored: lobby {} display not ready",
                  lobby_id);
        return;
      }

      target_session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
      target_session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
      target_session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
      active_state->focused_slot = slot_number;

      logs::log(logs::info,
                "[PARTY] Session {} input focused on slot {} (lobby {})",
                target_session_id,
                slot_number,
                lobby->id);
    };

    // Initialize player volumes
    ov->player_volumes.resize(payload->players.size(), 1.0f);
  }

  logs::log(logs::info, "[PARTY SPAWN] Party mode active for session {} with {} players", session_id, player_count);
  send_http(socket, 200, rfl::json::write(PartySpawnResponse{.lobby_ids = lobby_ids}));
}

} // namespace wolf::api
