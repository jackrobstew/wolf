#include <algorithm>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>
#include <streaming/party_compositor.hpp>
#include <streaming/session_overlay.hpp>

namespace wolf::core::sessions {

using session_devices = immer::map<std::string /* session_id */, std::shared_ptr<events::devices_atom_queue>>;

/**
 * Will stop the execution until an event of type RTPPingType is triggered
 * and the signature is matching the input `sess`.
 * Returns the RTPPingType event
 */
template <typename RTPPingType>
immer::box<RTPPingType> wait_for_ping(std::shared_ptr<events::EventBusType> ev_bus, const auto &sess) {
  auto ping_promise = std::make_shared<std::promise<RTPPingType>>();
  auto ping_future = ping_promise->get_future();

  auto handler =
      ev_bus->register_handler<immer::box<RTPPingType>>([sess, ping_promise](const immer::box<RTPPingType> &ping_ev) {
        // Check if this ping is for our session
        if (sess->rtp_secret_payload == ping_ev->payload || // Secret payload matching
            (!ping_ev->payload.has_value() && ping_ev->client_ip == sess->client_ip &&
             ping_ev->client_port == sess->port)) { // Legacy IP+port matching when no payload has been passed
          // Resolve the promise with the ping event data
          ping_promise->set_value(*ping_ev);
        }
      });

  // Wait for the promise to be fulfilled
  auto ping_ev = ping_future.get();

  // Unregister the handler since we only need it once
  handler.unregister();

  return ping_ev;
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_moonlight_handlers(const immer::box<state::AppState> &app_state,
                         const std::string &runtime_dir,
                         const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  /*
   * A queue of devices that are waiting to be plugged, mapped by session_id
   * This way we can accumulate devices here until the docker container is up and running
   */
  auto plugged_devices_queue = std::make_shared<immer::atom<session_devices>>();

  // During active party, upgrade pause (disconnect) to full stop.
  // Moonlight "resume" doesn't trigger VideoSession/StreamSession handlers,
  // so the bridge from compositor → encoder never gets created on resume.
  // Force a clean session kill so the next connection goes through the full bridge path.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
      [&app_state](const immer::box<events::PauseStreamEvent> &ev) {
        auto active = std::static_pointer_cast<streaming::ActivePartyState>(*app_state->active_party);
        if (active && active->compositor && active->global) {
          logs::log(logs::info,
                    "[PARTY] Session {} paused during global party — upgrading to full stop for clean reconnect",
                    ev->session_id);
          app_state->event_bus->fire_event(immer::box<events::StopStreamEvent>(
              events::StopStreamEvent{.session_id = ev->session_id}));
        }
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [&app_state, plugged_devices_queue](const immer::box<events::StopStreamEvent> &ev) {
        // If party mode is active, keep party resources alive but still remove the session.
        // The compositor, hub, and lobbies live on AppState — they survive session removal.
        // The encoding pipeline dies (old RTP endpoints are stale anyway), but the compositor
        // keeps running and drops frames until the next client connects. The VideoSession
        // hijack will create a fresh encoder and switch it to the compositor output.
        auto active = std::static_pointer_cast<streaming::ActivePartyState>(*app_state->active_party);
        if (active && active->compositor) {
          logs::log(logs::info,
                    "[PARTY] Session {} disconnected — party persists on AppState, session cleaned up",
                    ev->session_id);
        }

        // Remove session from app state — encoder dies, but party compositor survives
        app_state->running_sessions->update([&ev](const immer::vector<events::StreamSession> &ses_v) {
          return state::remove_session(ses_v, {.session_id = ev->session_id});
        });

        plugged_devices_queue->update([=](const auto map) { return map.erase(std::to_string(ev->session_id)); });
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [plugged_devices_queue, lobbies = app_state->lobbies](const immer::box<events::PlugDeviceEvent> &hotplug_ev) {
        logs::log(logs::debug, "{} received hot-plug device event", hotplug_ev->session_id);

        // If we are currently in a lobby we don't want to plug the device to the original wolf-ui session
        if (!state::get_lobby_by_connected_session(lobbies->load(), hotplug_ev->session_id)) {
          if (auto session_devices_queue = plugged_devices_queue->load()->find(hotplug_ev->session_id)) {
            session_devices_queue->get()->push(hotplug_ev);
          } else {
            logs::log(logs::warning, "Unable to find plugged_devices_queue for session {}", hotplug_ev->session_id);
          }
        } else {
          // This event will be picked up by the lobbies handler
          logs::log(logs::debug, "Session {} is in a lobby, ignoring hot-plug device event", hotplug_ev->session_id);
        }
      }));

  // Run process and our custom wayland as soon as a new StreamSession is created
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StreamSession>>(
      [=](const immer::box<events::StreamSession> &session) {
        /* Initialise plugged device queue */
        auto devices_q = std::make_shared<events::devices_atom_queue>();
        plugged_devices_queue->update(
            [=](const session_devices map) { return map.set(std::to_string(session->session_id), devices_q); });

        std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
            std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

        // If party mode is active AND global, skip Wolf-UI + video producer entirely.
        // The VideoSession handler will create a bridge from compositor → {session_id}_video.
        // If party is private, only the original session joins — others get normal Wolf-UI.
        auto active_party_check = std::static_pointer_cast<streaming::ActivePartyState>(*app_state->active_party);
        bool party_active = active_party_check && active_party_check->compositor && active_party_check->global;

        if (party_active) {
          logs::log(logs::info,
                    "[PARTY] Session {} connecting during active global party — skipping Wolf-UI",
                    session->session_id);

          // During global party mode this session does not run its own Wolf-UI,
          // so route pointer/keyboard input to the currently focused party slot.
          // This keeps mouse input (including Moonlight controller mouse emulation)
          // functional while viewing the party compositor.
          if (!active_party_check->lobby_ids.empty()) {
            auto focus_index = std::clamp(active_party_check->focused_slot - 1,
                                          0,
                                          static_cast<int>(active_party_check->lobby_ids.size()) - 1);
            auto lobbies = app_state->lobbies->load();
            auto lobby = state::get_lobby_by_id(lobbies.get(), active_party_check->lobby_ids[focus_index]);
            if (lobby) {
              auto wl_state = *lobby->wayland_display->load();
              if (wl_state) {
                session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
                session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
                session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
                logs::log(logs::info,
                          "[PARTY] Session {} input routed to slot {} (lobby {})",
                          session->session_id,
                          focus_index + 1,
                          lobby->id);
              } else {
                logs::log(logs::warning,
                          "[PARTY] Session {} lobby {} has no Wayland display yet",
                          session->session_id,
                          lobby->id);
              }
            } else {
              logs::log(logs::warning,
                        "[PARTY] Session {} could not find focused lobby {} for input routing",
                        session->session_id,
                        active_party_check->lobby_ids[focus_index]);
            }
          }
          // Fall through to VideoSession handler which creates the bridge
        } else if (session->app->start_virtual_compositor) {
          logs::log(logs::debug, "[STREAM_SESSION] Create wayland compositor");

          // Start Gstreamer producer pipeline
          std::thread([session, on_ready, gst_context = app_state->gst_context]() {
            streaming::start_video_producer(std::to_string(session->session_id),
                                            session->app->video_producer_buffer_caps,
                                            session->app->render_node,
                                            {.width = session->display_mode.width,
                                             .height = session->display_mode.height,
                                             .refreshRate = session->display_mode.refreshRate},
                                            gst_context,
                                            on_ready,
                                            session->event_bus);
          }).detach();
        } else {
          // Create virtual devices
          auto mouse = input::Mouse::create();
          if (!mouse) {
            logs::log(logs::error, "Failed to create mouse: {}", mouse.getErrorMessage());
          } else {
            auto mouse_ptr = input::Mouse(std::move(*mouse));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = mouse_ptr.get_udev_events(),
                                        .udev_hw_db_entries = mouse_ptr.get_udev_hw_db_entries()}));
            session->mouse->emplace(std::move(mouse_ptr));
          }

          auto keyboard = input::Keyboard::create();
          if (!keyboard) {
            logs::log(logs::error, "Failed to create keyboard: {}", keyboard.getErrorMessage());
          } else {
            auto keyboard_ptr = input::Keyboard(std::move(*keyboard));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = keyboard_ptr.get_udev_events(),
                                        .udev_hw_db_entries = keyboard_ptr.get_udev_hw_db_entries()}));
            session->keyboard->emplace(std::move(keyboard_ptr));
          }
          on_ready->set_value({});
        }

        if (!party_active) {
          /* Create audio virtual sink */
          logs::log(logs::debug, "[STREAM_SESSION] Create virtual audio sink");
          auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, session->session_id);
          std::shared_ptr<audio::VSink> v_device;
          if (session->app->start_audio_server && audio_server && audio_server->server) {
            v_device = audio::create_virtual_sink(
                audio_server->server,
                audio::AudioDevice{.sink_name = pulse_sink_name,
                                   .mode = state::get_audio_mode(session->audio_channel_count, true)});
            session->audio_sink->store(v_device);

            std::thread([session, audio_server = audio_server->server]() {
              auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, session->session_id);
              streaming::start_audio_producer(std::to_string(session->session_id),
                                              session->event_bus,
                                              session->audio_channel_count,
                                              sink_name,
                                              audio::get_server_name(audio_server));
            }).detach();
          }

          // TODO: timeout? What if the wayland display is never ready?
          auto w_display_ready = on_ready->get_future().then([session](auto fut) {
            streaming::WaylandDisplayReady ready = fut.get();

            auto wl_state = virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
            // Set the wayland display
            session->wayland_display->store(wl_state);

            // Set virtual devices
            session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
            session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
            session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

            logs::log(logs::debug, "[STREAM_SESSION] Start runner");
            session->event_bus->fire_event(immer::box<events::StartRunner>(
                events::StartRunner{.stop_stream_when_over = true,
                                    .runner = session->app->runner,
                                    .stream_session = std::make_shared<events::StreamSession>(*session)}));
          });
        }
        // When party_active: no Wolf-UI, no audio producer, no runner.
        // VideoSession handler creates bridge from compositor → {session_id}_video.
      }));

  /* Start runner */
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StartRunner>>(
      [=](const immer::box<events::StartRunner> &run_session) {
        auto session_id = std::to_string(run_session->stream_session->session_id);
        auto devices_q = plugged_devices_queue->load()->find(session_id);
        if (!devices_q) {
          logs::log(logs::warning, "No devices queue found for session {}", session_id);
          return;
        }

        std::thread([=]() {
          start_runner(
              run_session->runner,
              *devices_q,
              immer::box<RunnerArgs>{RunnerArgs{
                  .session_id = session_id,
                  .video_settings =
                      {
                          .width = run_session->stream_session->display_mode.width,
                          .height = run_session->stream_session->display_mode.height,
                          .refresh_rate = run_session->stream_session->display_mode.refreshRate,
                          .wayland_render_node = run_session->stream_session->app->render_node,
                          .runner_render_node = run_session->stream_session->app->render_node,
                          .video_producer_buffer_caps = run_session->stream_session->app->video_producer_buffer_caps,
                      },
                  .wayland_display = run_session->stream_session->wayland_display->load(),
                  .audio_server = audio_server,
                  .audio_sink = run_session->stream_session->audio_sink->load(),
                  .host = app_state->host,
                  .app_local_state_folder = run_session->stream_session->app_local_state_folder,
                  .app_host_state_folder = run_session->stream_session->app_host_state_folder,
                  .xdg_runtime_dir = runtime_dir,
                  .client_settings = run_session->stream_session->client_settings}});

          // Runner process ended
          if (run_session->stop_stream_when_over) {
            run_session->stream_session->wayland_display->store(nullptr);

            app_state->event_bus->fire_event(immer::box<events::StopStreamEvent>(
                events::StopStreamEvent{.session_id = run_session->stream_session->session_id}));
          }
        }).detach();
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::VideoSession>>(
      [ev_bus = app_state->event_bus,
       gst_context = app_state->gst_context,
       active_party_ptr = app_state->active_party,
       running_sessions = app_state->running_sessions](const immer::box<events::VideoSession> &sess) {
        // Start a thread that will wait for the RTP ping event
        std::thread([sess, ev_bus, gst_context, active_party_ptr, running_sessions]() {
          auto ping_ev = wait_for_ping<events::RTPVideoPingEvent>(ev_bus, sess);

          // Party mode: create an interpipe bridge from compositor → {session_id}_video.
          // Wolf-UI was SKIPPED in the StreamSession handler, so {session_id}_video is available.
          // The encoder's interpipesrc connects to {session_id}_video and gets compositor
          // frames from frame 1. No mid-stream switch, no format mismatch.
          auto active = std::static_pointer_cast<streaming::ActivePartyState>(*active_party_ptr);
          if (active && active->compositor && active->global) {
            auto sess_id = sess->session_id;
            auto party_video = fmt::format("{}_video", active->party_base_interpipe);
            auto party_audio = fmt::format("{}_audio", active->party_base_interpipe);
            auto session_video = fmt::format("{}_video", sess_id);
            auto session_audio = fmt::format("{}_audio", sess_id);

            // Video bridge: compositor output → {session_id}_video
            // Leaky downstream: drop old video frames if encoder is slow (shows latest)
            auto video_bridge_desc = fmt::format(
                "interpipesrc name=party_bridge_{}_video listen-to={} "
                "is-live=true stream-sync=restart-ts max-bytes=0 max-buffers=3 leaky-type=downstream "
                "! queue max-size-buffers=3 leaky=downstream "
                "! interpipesink name={} sync=true max-buffers=3 max-bytes=0",
                sess_id, party_video, session_video);
            // Audio bridge: compositor audio → {session_id}_audio
            // NOT leaky — audio must not drop frames (causes pops/stutters)
            auto audio_bridge_desc = fmt::format(
                "interpipesrc name=party_bridge_{}_audio listen-to={} "
                "is-live=true stream-sync=restart-ts max-bytes=0 max-buffers=30 "
                "! queue max-size-buffers=30 "
                "! interpipesink name={} sync=true max-buffers=30 max-bytes=0",
                sess_id, party_audio, session_audio);

            // Start bridges in background threads
            for (auto &desc : {video_bridge_desc, audio_bridge_desc}) {
              std::thread([desc]() {
                GError *err = nullptr;
                auto *pipeline = gst_parse_launch(desc.c_str(), &err);
                if (!pipeline) { if (err) g_error_free(err); return; }
                if (err) g_error_free(err);
                auto *ctx = g_main_context_new();
                g_main_context_push_thread_default(ctx);
                auto *loop = g_main_loop_new(ctx, FALSE);
                auto *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
                gst_bus_add_signal_watch(bus);
                g_signal_connect(bus, "message::error",
                    G_CALLBACK(+[](GstBus *, GstMessage *, gpointer data) {
                      g_main_loop_quit((GMainLoop *)data);
                    }), loop);
                g_signal_connect(bus, "message::eos",
                    G_CALLBACK(+[](GstBus *, GstMessage *, gpointer data) {
                      g_main_loop_quit((GMainLoop *)data);
                    }), loop);
                gst_object_unref(bus);
                gst_element_set_state(pipeline, GST_STATE_PLAYING);
                g_main_loop_run(loop);
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                g_main_loop_unref(loop);
                g_main_context_pop_thread_default(ctx);
                g_main_context_unref(ctx);
              }).detach();
            }
            // Brief delay for bridges to reach PLAYING before encoder connects
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Assign hub and compositor to the session
            auto sessions = running_sessions->load();
            auto session = state::get_session_by_id(sessions.get(), sess_id);
            if (session) {
              *session->controller_hub = active->hub;
              *session->party_compositor = active->compositor;
            }
            active->original_session_id = sess_id;

            // Assign session-scoped controller offset so each device's controllers
            // get unique numbers in the shared hub (Session A: 0-3, B: 100-103, etc.)
            int offset = active->next_offset;
            active->next_offset += 100;
            if (active->compositor && active->compositor->overlay) {
              active->compositor->overlay->session_ctrl_offsets[sess_id] = offset;
            }
            logs::log(logs::info, "[PARTY] Session {} controller offset: {}", sess_id, offset);

            logs::log(logs::info,
                      "[PARTY] Session {} bridge created — encoder gets compositor frames directly",
                      sess_id);
          }

          // Start streaming (this blocks in the GStreamer main loop)
          streaming::start_streaming_video(sess,
                                           ev_bus,
                                           ping_ev->client_ip,
                                           ping_ev->client_port,
                                           gst_context,
                                           ping_ev->video_socket.get());
        }).detach();
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::AudioSession>>(
      [ev_bus = app_state->event_bus, audio_server](const immer::box<events::AudioSession> &sess) {
        // Start a thread that will wait for the RTP ping event
        std::thread([sess, ev_bus, audio_server]() {
          auto ping_ev = wait_for_ping<events::RTPAudioPingEvent>(ev_bus, sess);

          // Start streaming
          auto audio_server_name = audio_server ? audio::get_server_name(audio_server->server)
                                                : std::optional<std::string>();
          auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, sess->session_id);
          auto server_name = audio_server_name ? audio_server_name.value() : "";

          streaming::start_streaming_audio(sess,
                                           ev_bus,
                                           ping_ev->client_ip,
                                           ping_ev->client_port,
                                           ping_ev->audio_socket.get(),
                                           sink_name,
                                           server_name);
        }).detach();
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
