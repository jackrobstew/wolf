#include "party_compositor.hpp"
#include "session_overlay.hpp"
#include <control/controller_hub.hpp>
#include <core/gstreamer.hpp>
#include <fmt/format.h>
#include <gst/gst.h>
#include <gst-video-context.hpp>
#include <helpers/logger.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <cstdio>
#include <future>
#include <map>
#include <optional>
#include <thread>

namespace {

// CUDA context handler — same pattern as streaming.cpp
struct PartyContextData {
  const std::string device_path;
  std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context;
};

static void party_need_context_handler(GstBus *, GstMessage *msg, gpointer data) {
  auto ctx_data = static_cast<PartyContextData *>(data);
  if (auto gst_context = ctx_data->gst_context->load().get()) {
    gst_video_context::set_context(gst_context, msg);
  } else if (auto video_context = gst_video_context::need_context_for_device(ctx_data->device_path, msg)) {
    ctx_data->gst_context->store(video_context);
  }
}

static GstBusSyncReply party_bus_sync_handler(GstBus *, GstMessage *msg, gpointer data) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    party_need_context_handler(nullptr, msg, data);
  }
  return GST_BUS_PASS;
}

bool party_run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<wolf::core::events::EventBusHandlers>>(
        wolf::core::gstreamer::gst_element_ptr)> &on_pipeline_ready,
    std::shared_ptr<PartyContextData> ctx_data = nullptr,
    const std::function<void(bool)> &on_playing = nullptr) {
  GError *error = nullptr;
  wolf::core::gstreamer::gst_element_ptr pipeline(
      gst_parse_launch(pipeline_desc.c_str(), &error),
      [](const auto &p) { gst_object_unref(p); });
  if (!pipeline) {
    logs::log(logs::error, "[PARTY] Pipeline parse error: {}", error->message);
    g_error_free(error);
    if (on_playing) on_playing(false);
    return false;
  } else if (error) {
    logs::log(logs::warning, "[PARTY] Pipeline parse warning: {}", error->message);
    g_error_free(error);
  }

  wolf::core::gstreamer::gst_main_context_ptr context = {g_main_context_new(), ::g_main_context_unref};
  g_main_context_push_thread_default(context.get());
  wolf::core::gstreamer::gst_main_loop_ptr loop(g_main_loop_new(context.get(), FALSE), ::g_main_loop_unref);

  auto handlers = on_pipeline_ready(pipeline);

  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message::error", G_CALLBACK(wolf::core::gstreamer::pipeline_error_handler), loop.get());
  g_signal_connect(bus, "message::eos", G_CALLBACK(wolf::core::gstreamer::pipeline_eos_handler), loop.get());
  // Set up CUDA context sharing so cudacompositor can access GPU memory from lobby producers
  if (ctx_data) {
    gst_bus_set_sync_handler(bus, party_bus_sync_handler, ctx_data.get(), nullptr);
  }
  gst_object_unref(bus);

  auto state_ret = gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  if (state_ret == GST_STATE_CHANGE_FAILURE) {
    logs::log(logs::error, "[PARTY] Pipeline failed to transition to PLAYING");
    if (on_playing) on_playing(false);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    return false;
  }
  // Wait for PLAYING to actually be reached (interpipesink registers on PLAYING)
  GstState actual_state;
  gst_element_get_state(pipeline.get(), &actual_state, nullptr, 5 * GST_SECOND);
  if (on_playing) on_playing(actual_state == GST_STATE_PLAYING);
  if (actual_state != GST_STATE_PLAYING) {
    logs::log(logs::error, "[PARTY] Pipeline did not reach PLAYING (got {})", (int)actual_state);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    return false;
  }
  g_main_loop_run(loop.get());

  // Go straight to NULL — do NOT transition through PAUSED/READY.
  // PAUSED re-registers interpipesink names that stop_party already unregistered,
  // causing "not unique" errors when the next party tries to use the same name.
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
  gst_element_get_state(pipeline.get(), nullptr, nullptr, 3 * GST_SECOND);
  return true;
}
} // anonymous namespace

namespace streaming {

using namespace wolf::core;

// ---------------------------------------------------------------------------
// Tile aspect ratio helpers
// ---------------------------------------------------------------------------

float parse_tile_aspect(const std::string &tile_aspect) {
  if (tile_aspect == "4:3") return 4.0f / 3.0f;
  // "16:9" or anything else = no constraint (use grid cell as-is)
  return 0.0f;
}

TilePosition constrain_to_aspect(int cell_x, int cell_y, int cell_w, int cell_h, float target_aspect) {
  if (target_aspect <= 0.0f) {
    return {cell_x, cell_y, cell_w, cell_h};
  }
  float cell_aspect = static_cast<float>(cell_w) / static_cast<float>(cell_h);
  int tile_w, tile_h;
  if (cell_aspect > target_aspect) {
    // Cell is wider than target — constrain by height
    tile_h = cell_h;
    tile_w = static_cast<int>(cell_h * target_aspect);
  } else {
    // Cell is taller than target — constrain by width
    tile_w = cell_w;
    tile_h = static_cast<int>(cell_w / target_aspect);
  }
  // Ensure even dimensions (video encoders require it)
  tile_w -= tile_w % 2;
  tile_h -= tile_h % 2;
  int x_off = (cell_w - tile_w) / 2;
  int y_off = (cell_h - tile_h) / 2;
  return {cell_x + x_off, cell_y + y_off, tile_w, tile_h};
}

// ---------------------------------------------------------------------------
// Layout definitions — base grid cells on a 1920x1080 canvas.
// When tile_aspect is "4:3", tiles are constrained within their grid cells.
// ---------------------------------------------------------------------------

struct BaseLayout {
  std::string name;
  std::vector<TilePosition> cells; // Grid cells (unconstrained)
};

static const std::map<std::string, BaseLayout> BASE_LAYOUTS = {
    {"2p",      {.name = "2p",      .cells = {{480, 0, 960, 540}, {480, 540, 960, 540}}}},
    {"2p-lr",   {.name = "2p-lr",   .cells = {{0, 0, 960, 1080}, {960, 0, 960, 1080}}}},
    {"2p-tb",   {.name = "2p-tb",   .cells = {{0, 0, 1920, 540}, {0, 540, 1920, 540}}}},
    {"3p",      {.name = "3p",      .cells = {{0, 0, 960, 540}, {960, 0, 960, 540}, {0, 540, 1920, 540}}}},
    {"3p-fill", {.name = "3p-fill", .cells = {{0, 0, 960, 1080}, {960, 0, 960, 540}, {960, 540, 960, 540}}}},
    {"4p",      {.name = "4p",      .cells = {{0, 0, 960, 540}, {960, 0, 960, 540}, {0, 540, 960, 540}, {960, 540, 960, 540}}}},
};

std::optional<PartyLayout> get_layout(const std::string &name, const std::string &tile_aspect) {
  auto it = BASE_LAYOUTS.find(name);
  if (it == BASE_LAYOUTS.end()) return std::nullopt;

  float aspect = parse_tile_aspect(tile_aspect);
  PartyLayout layout{.name = it->second.name, .tiles = {}};
  layout.tiles.reserve(it->second.cells.size());

  for (const auto &cell : it->second.cells) {
    layout.tiles.push_back(constrain_to_aspect(cell.x, cell.y, cell.w, cell.h, aspect));
  }
  return layout;
}

// ---------------------------------------------------------------------------
// Pipeline handle — stores the GStreamer pipeline element so we can send EOS
// to tear it down.  Wolf's run_pipeline() has a message::eos handler that
// calls g_main_loop_quit(), so sending EOS is the canonical way to stop.
// ---------------------------------------------------------------------------

struct PipelineHandle {
  gstreamer::gst_element_ptr pipeline;
};

// ---------------------------------------------------------------------------
// Build the video compositor pipeline string.
// ---------------------------------------------------------------------------

static std::string build_video_pipeline(std::size_t session_id,
                                        const std::vector<std::string> &lobby_ids,
                                        const PartyLayout &layout) {
  std::string pipeline;

  // GPU compositor: normalize every lobby stream to the same CUDA format first.
  // Mixed input formats (e.g. RGBx + NV12) can fail caps negotiation on
  // cudacompositor and freeze the party switch.
  for (std::size_t i = 0; i < lobby_ids.size(); ++i) {
    pipeline += fmt::format(
        "interpipesrc name=party_src_{i} listen-to={lobby}_video "
        "is-live=true stream-sync=restart-ts max-bytes=0 max-buffers=3 "
        "leaky-type=downstream ! "
        "queue max-size-buffers=3 leaky=downstream ! "
        "cudaupload ! "
        "cudaconvertscale ! "
        "video/x-raw(memory:CUDAMemory),format=BGRA,width=1920,height=1080,pixel-aspect-ratio=1/1 ! "
        "compositor.sink_{i} ",
        fmt::arg("i", i),
        fmt::arg("lobby", lobby_ids[i]));
  }

  // Overlay layer: appsrc pushes BGRA frames (with alpha) rendered by Cairo at 15fps.
  // cudaupload sends them to GPU. Compositor alpha-blends on top of game tiles.
  // IMPORTANT: Fixed pixel-aspect-ratio=1/1 to avoid unfixed caps error.
  auto overlay_idx = lobby_ids.size();
  pipeline += fmt::format(
      "appsrc name=overlay_src format=time is-live=true do-timestamp=true "
      "min-latency=0 max-latency=100000000 max-bytes=0 ! "
      "video/x-raw,format=BGRA,width=1920,height=1080,framerate=15/1,pixel-aspect-ratio=1/1 ! "
      "queue max-size-buffers=1 leaky=downstream ! "
      "cudaupload ! "
      "compositor.sink_{} ",
      overlay_idx);

  pipeline += "cudacompositor name=compositor ";
  for (std::size_t i = 0; i < lobby_ids.size(); ++i) {
    const auto &tile = layout.tiles[i];
    pipeline += fmt::format(
        "sink_{i}::xpos={x} sink_{i}::ypos={y} sink_{i}::width={w} sink_{i}::height={h} ",
        fmt::arg("i", i),
        fmt::arg("x", tile.x),
        fmt::arg("y", tile.y),
        fmt::arg("w", tile.w),
        fmt::arg("h", tile.h));
  }
  // Overlay pad: full-screen, on top (zorder=100), alpha compositing
  pipeline += fmt::format(
      "sink_{o}::xpos=0 sink_{o}::ypos=0 sink_{o}::width=1920 sink_{o}::height=1080 "
      "sink_{o}::zorder=100 sink_{o}::operator=over ",
      fmt::arg("o", overlay_idx));

  // Convert compositor output to CPU NV12 to match Wolf-UI's output format.
  // Wolf-UI's waylanddisplaysrc outputs "video/x-raw" (CPU) — NOT CUDA memory.
  // The encoder negotiates caps with Wolf-UI first, then we switch to compositor.
  // If caps don't match (CUDA vs CPU, or different format/metadata), the switch
  // fails silently. pixel-aspect-ratio=1/1 matches Wolf-UI's metadata exactly.
  pipeline += fmt::format(
      "! cudadownload "
      "! video/x-raw,width=1920,height=1080,pixel-aspect-ratio=1/1 "
      "! videoconvert "
      "! video/x-raw,format=NV12,width=1920,height=1080,pixel-aspect-ratio=1/1 "
      "! videorate "
      "! video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1,pixel-aspect-ratio=1/1 "
      "! queue max-size-buffers=2 leaky=downstream "
      "! interpipesink name=party_{}_video sync=true max-buffers=3 max-bytes=0",
      session_id);

  return pipeline;
}

// ---------------------------------------------------------------------------
// Build the audio mixer pipeline string.
// ---------------------------------------------------------------------------

static std::string build_audio_pipeline(std::size_t session_id,
                                        const std::vector<std::string> &lobby_ids,
                                        const PartyLayout &layout) {
  std::string pipeline;

  // Per-lobby: interpipesrc → normalize → spatial pan → audiomixer
  // Pan is calculated from tile center-x: left=-1.0, center=0.0, right=1.0
  for (std::size_t i = 0; i < lobby_ids.size(); ++i) {
    const auto &tile = layout.tiles[i];
    float pan = (tile.x + tile.w / 2.0f) / 1920.0f * 2.0f - 1.0f;

    pipeline += fmt::format(
        "interpipesrc name=party_audio_src_{i} listen-to={lobby}_audio "
        "is-live=true stream-sync=restart-ts max-bytes=0 max-buffers=10 ! "
        "queue max-size-buffers=10 ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
        "volume name=party_vol_{i} ! "
        "audiopanorama panorama={pan} ! "
        "mixer. ",
        fmt::arg("i", i),
        fmt::arg("lobby", lobby_ids[i]),
        fmt::arg("pan", pan));
  }

  // audiomixer → interpipesink
  pipeline += fmt::format(
      "audiomixer name=mixer latency=20000000 ! "
      "interpipesink name=party_{}_audio sync=true max-buffers=10 max-bytes=0",
      session_id);

  return pipeline;
}

// ---------------------------------------------------------------------------
// start_party — build pipelines, launch in threads, redirect the stream.
// ---------------------------------------------------------------------------

std::shared_ptr<PartyCompositorState> start_party(
    std::size_t session_id,
    const std::vector<std::string> &lobby_ids,
    const PartyLayout &layout,
    const std::shared_ptr<events::EventBusType> &event_bus,
    const std::string &render_node,
    std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context) {

  if (lobby_ids.empty() || lobby_ids.size() > layout.tiles.size()) {
    logs::log(logs::error,
              "[PARTY] Cannot start party: {} lobbies but layout '{}' supports {}",
              lobby_ids.size(), layout.name, layout.tiles.size());
    return nullptr;
  }

  // --- Overlay state ---------------------------------------------------------
  auto overlay_state = std::make_shared<OverlayState>();
  overlay_state->tiles = layout.tiles;
  overlay_state->tile_aspect = ""; // set by caller if needed
  overlay_state->layout_name = layout.name;
  overlay_state->stats.party_start = std::chrono::steady_clock::now();

  // Background stats poller — reads GPU/CPU every 2 seconds.
  // Collects stats into local vars first, then briefly locks to copy them over.
  std::thread([ov = std::weak_ptr<OverlayState>(overlay_state)]() {
    while (!ov.expired()) {
      float cpu_load = 0, gpu_util = 0, gpu_enc = 0, gpu_temp = 0;

      FILE *f = fopen("/proc/loadavg", "r");
      if (f) { fscanf(f, "%f", &cpu_load); fclose(f); }

      FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,temperature.gpu "
                       "--format=csv,noheader,nounits 2>/dev/null", "r");
      if (p) { fscanf(p, "%f, %f, %f", &gpu_util, &gpu_enc, &gpu_temp); pclose(p); }

      if (auto state = ov.lock()) {
        state->stats.cpu_load = cpu_load;
        state->stats.gpu_util = gpu_util;
        state->stats.gpu_enc = gpu_enc;
        state->stats.gpu_temp = gpu_temp;

        // Process pending menu actions (swap, end party, etc.)
        state->process_actions();

        // System health alerts — show overlay warning when GPU/CPU is stressed
        if (state->alert_text.empty()) { // don't spam — only set when no alert active
          if (gpu_enc > 95)
            { state->alert_text = fmt::format("GPU Encoder High ({}%)", (int)gpu_enc);
              state->alert_start = std::chrono::steady_clock::now(); }
          else if (gpu_temp > 85)
            { state->alert_text = fmt::format("GPU Temp Warning ({}°C)", (int)gpu_temp);
              state->alert_start = std::chrono::steady_clock::now(); }
          else if (cpu_load > 16) // ~80% on 20-thread system
            { state->alert_text = fmt::format("High CPU Load ({:.1f})", cpu_load);
              state->alert_start = std::chrono::steady_clock::now(); }
        }

        // Sync volume sliders to GStreamer audio pipeline
        if (state->audio_pipeline) {
          std::lock_guard<std::mutex> lock(state->info_mtx);
          for (int i = 0; i < (int)state->player_volumes.size(); ++i) {
            auto name = fmt::format("party_vol_{}", i);
            auto *vol_el = gst_bin_get_by_name(GST_BIN(state->audio_pipeline), name.c_str());
            if (vol_el) {
              g_object_set(vol_el, "volume", (gdouble)state->player_volumes[i], nullptr);
              gst_object_unref(vol_el);
            }
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();

  // --- Video pipeline -------------------------------------------------------
  auto video_pipeline_desc = build_video_pipeline(session_id, lobby_ids, layout);
  logs::log(logs::info, "[PARTY] Video pipeline: {}", video_pipeline_desc);

  // Shared handle stores the GstElement pipeline pointer so stop_party() can
  // send EOS to it later — exactly the same pattern Wolf uses in
  // start_video_producer() and start_audio_producer().
  auto video_handle = std::make_shared<PipelineHandle>();
  auto video_ready = std::make_shared<std::promise<bool>>();
  auto video_future = video_ready->get_future();

  // CUDA context data for the video compositor pipeline
  auto ctx_data = std::make_shared<PartyContextData>(PartyContextData{
      .device_path = render_node, .gst_context = gst_context});

  // Launch in a detached thread, following Wolf's pattern.
  // Wait for lobby containers to start their video producers before launching
  // the compositor — interpipesrc fails immediately if the target sink doesn't exist.
  std::thread([video_pipeline_desc, video_handle, video_ready, ctx_data, overlay_state]() {
    // Brief delay for lobby containers to start their video producers.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    party_run_pipeline(video_pipeline_desc,
                 [video_handle, overlay_state](gstreamer::gst_element_ptr pipeline) {
      // Stash the pipeline element so we can send EOS from stop_party().
      video_handle->pipeline = pipeline;

      // GPU-native overlay: get appsrc, spawn render thread
      setup_overlay(pipeline.get(), overlay_state);

      return immer::array<immer::box<events::EventBusHandlers>>{};
    },
    ctx_data,
    [video_ready](bool ok) { video_ready->set_value(ok); });

    logs::log(logs::info, "[PARTY] Video compositor pipeline finished");
  }).detach();

  // --- Audio pipeline -------------------------------------------------------
  auto audio_pipeline_desc = build_audio_pipeline(session_id, lobby_ids, layout);
  logs::log(logs::info, "[PARTY] Audio pipeline: {}", audio_pipeline_desc);

  auto audio_handle = std::make_shared<PipelineHandle>();
  auto audio_ready = std::make_shared<std::promise<bool>>();
  auto audio_future = audio_ready->get_future();

  std::thread([audio_pipeline_desc, audio_handle, audio_ready, overlay_state]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    party_run_pipeline(audio_pipeline_desc,
                 [audio_handle, overlay_state](gstreamer::gst_element_ptr pipeline) {
      audio_handle->pipeline = pipeline;
      // Store audio pipeline reference so overlay can adjust volume elements at runtime
      if (overlay_state) {
        overlay_state->audio_pipeline = pipeline.get();
      }

      return immer::array<immer::box<events::EventBusHandlers>>{};
    },
    nullptr, // no CUDA context needed for audio
    [audio_ready](bool ok) { audio_ready->set_value(ok); });

    logs::log(logs::info, "[PARTY] Audio mixer pipeline finished");
  }).detach();

  // --- Wait for pipelines to reach PLAYING state ----------------------------
  // The interpipesinks must be registered before the encoder's interpipesrc
  // can switch to them.
  bool video_ok = false, audio_ok = false;
  try {
    video_ok = video_future.wait_for(std::chrono::seconds(30)) == std::future_status::ready
               && video_future.get();
  } catch (const std::future_error &e) {
    logs::log(logs::error, "[PARTY] Video pipeline future error: {}", e.what());
  }
  try {
    audio_ok = audio_future.wait_for(std::chrono::seconds(30)) == std::future_status::ready
               && audio_future.get();
  } catch (const std::future_error &e) {
    logs::log(logs::error, "[PARTY] Audio pipeline future error: {}", e.what());
  }

  if (!video_ok || !audio_ok) {
    logs::log(logs::error, "[PARTY] Compositor pipelines failed to start (video={}, audio={})",
              video_ok, audio_ok);
    // Clean up whichever pipeline DID start, so interpipesink names get unregistered
    if (video_handle->pipeline) {
      gst_element_set_state(video_handle->pipeline.get(), GST_STATE_NULL);
      gst_element_get_state(video_handle->pipeline.get(), nullptr, nullptr, 3 * GST_SECOND);
    }
    if (audio_handle->pipeline) {
      gst_element_set_state(audio_handle->pipeline.get(), GST_STATE_NULL);
      gst_element_get_state(audio_handle->pipeline.get(), nullptr, nullptr, 3 * GST_SECOND);
    }
    return nullptr;
  }

  // --- Redirect the Moonlight stream to our composited output ---------------
  auto party_base = fmt::format("party_{}", session_id);
  auto video_interpipe = fmt::format("party_{}_video", session_id);
  auto audio_interpipe = fmt::format("party_{}_audio", session_id);

  // Switch immediately — the compositor outputs black frames until lobbies start rendering.
  // The interpipesink is already registered (pipeline is PLAYING).
  event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>(
      events::SwitchStreamProducerEvents{.session_id = session_id,
                                          .interpipe_src_id = party_base}));
  logs::log(logs::info, "[PARTY] Stream switched to compositor output (lobbies may still be starting)");

  logs::log(logs::info,
            "[PARTY] Started party for session {} -- video: {}, audio: {}, layout: {}",
            session_id, video_interpipe, audio_interpipe, layout.name);

  // --- Build state ----------------------------------------------------------
  auto state = std::make_shared<PartyCompositorState>();
  state->party_interpipe_name = video_interpipe;
  state->party_audio_interpipe_name = audio_interpipe;
  state->source_lobby_ids = lobby_ids;
  state->layout_name = layout.name;
  // Store handles as shared_ptr<void> — the PartyCompositorState definition
  // uses shared_ptr<void> so any type can be stored.
  state->video_pipeline_handle = video_handle;
  state->audio_pipeline_handle = audio_handle;
  state->overlay = overlay_state;

  return state;
}

// ---------------------------------------------------------------------------
// stop_party — tear down the compositor pipelines and switch back.
// ---------------------------------------------------------------------------

void stop_party(std::shared_ptr<PartyCompositorState> state,
                std::size_t session_id,
                const std::shared_ptr<events::EventBusType> &event_bus) {
  if (!state) {
    logs::log(logs::warning, "[PARTY] stop_party called with null state");
    return;
  }

  logs::log(logs::info, "[PARTY] Stopping party for session {}", session_id);

  // 1. Kill overlay render thread
  if (state->overlay) {
    state->overlay->audio_pipeline = nullptr;
    state->overlay->appsrc = nullptr;
  }

  // 2. Destroy compositor pipelines FIRST.
  //    This unregisters party_{session_id}_video/audio interpipesinks.
  //    Any bridge pipelines (from reconnects) will lose their source and die,
  //    unregistering their {session_id}_video/audio interpipesinks.
  if (auto video_handle = std::static_pointer_cast<PipelineHandle>(state->video_pipeline_handle)) {
    if (video_handle->pipeline) {
      gst_element_set_state(video_handle->pipeline.get(), GST_STATE_NULL);
      gst_element_get_state(video_handle->pipeline.get(), nullptr, nullptr, 3 * GST_SECOND);
      video_handle->pipeline.reset();
    }
  }
  if (auto audio_handle = std::static_pointer_cast<PipelineHandle>(state->audio_pipeline_handle)) {
    if (audio_handle->pipeline) {
      gst_element_set_state(audio_handle->pipeline.get(), GST_STATE_NULL);
      gst_element_get_state(audio_handle->pipeline.get(), nullptr, nullptr, 3 * GST_SECOND);
      audio_handle->pipeline.reset();
    }
  }
  logs::log(logs::info, "[PARTY] Compositor pipelines torn down");

  // 3. Wait for bridge pipelines to die (they detect the compositor is gone
  //    and exit their main loops, unregistering interpipesink names).
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 4. Stop the lobbies
  for (const auto &lobby_id : state->source_lobby_ids) {
    logs::log(logs::info, "[PARTY] Stopping lobby {}", lobby_id);
    event_bus->fire_event(immer::box<events::StopLobbyEvent>(
        events::StopLobbyEvent{.lobby_id = lobby_id}));
  }

  // 5. Kill the Moonlight session LAST.
  //    All interpipesink names are now free. Moonlight reconnects → fresh Wolf-UI.
  event_bus->fire_event(immer::box<events::StopStreamEvent>(
      events::StopStreamEvent{.session_id = session_id}));

  logs::log(logs::info, "[PARTY] Party ended for session {} — client will reconnect to Wolf-UI",
            session_id);
}

} // namespace streaming
