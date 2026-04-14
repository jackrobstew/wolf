#pragma once

#include <events/events.hpp>
#include <gst-video-context.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace streaming {

/**
 * Layout definition for party mode tiling.
 * All positions/sizes are in pixels for a 1920x1080 output.
 */
struct TilePosition {
  int x, y, w, h;
};

struct PartyLayout {
  std::string name;
  std::vector<TilePosition> tiles;
};

/**
 * Constrain a tile to a target aspect ratio within its grid cell, centered.
 * If target_aspect <= 0, returns the cell unchanged (16:9 passthrough).
 *
 * @param cell_x, cell_y, cell_w, cell_h  The grid cell bounds
 * @param target_aspect  Width/height ratio (e.g. 4.0/3.0 = 1.333)
 * @return TilePosition centered in the cell with the target aspect
 */
TilePosition constrain_to_aspect(int cell_x, int cell_y, int cell_w, int cell_h, float target_aspect);

/**
 * Parse a tile_aspect string ("16:9", "4:3") into a float ratio.
 * Returns 0.0 for "16:9" (meaning: don't constrain, use cell as-is).
 * Returns 4.0/3.0 for "4:3".
 */
float parse_tile_aspect(const std::string &tile_aspect);

/**
 * Get a layout by name, optionally constrained to a tile aspect ratio.
 * Available layout names: "2p", "2p-lr", "2p-tb", "3p", "3p-fill", "4p"
 *
 * @param name     Layout name
 * @param tile_aspect  "16:9" (default, unconstrained) or "4:3" (retro)
 * @return Layout with tiles positioned for the given aspect, or nullptr if name unknown.
 *         Caller owns the returned pointer (heap-allocated).
 */
std::optional<PartyLayout> get_layout(const std::string &name, const std::string &tile_aspect = "16:9");

/**
 * Active party compositor state.
 * Stored on the session, similar to ControllerHubState.
 */
// Forward declaration
struct OverlayState;

struct PartyCompositorState {
  std::string party_interpipe_name; // e.g. "party_12345_video"
  std::string party_audio_interpipe_name; // e.g. "party_12345_audio"
  std::vector<std::string> source_lobby_ids;
  std::string layout_name; // "2p", "3p", "4p"

  // The GStreamer main loop thread — kept alive as long as party is active
  // Stopping the main loop tears down the pipeline
  std::shared_ptr<void> video_pipeline_handle;
  std::shared_ptr<void> audio_pipeline_handle;

  // Overlay state — shared with cairooverlay callback and input handler
  std::shared_ptr<OverlayState> overlay;
};

/**
 * Start a party compositor that reads video from N lobby interpipe sources,
 * tiles them into a single composited frame, and outputs to a new interpipe.
 *
 * @param session_id The Moonlight session ID (used to name the output interpipe)
 * @param lobby_ids List of lobby UUIDs whose video interpipes to read from
 * @param layout The layout to use (from get_layout)
 * @param event_bus The event bus (used to fire SwitchStreamProducerEvents)
 * @return PartyCompositorState, or nullptr on failure
 */
std::shared_ptr<PartyCompositorState> start_party(
    std::size_t session_id,
    const std::vector<std::string> &lobby_ids,
    const PartyLayout &layout,
    const std::shared_ptr<wolf::core::events::EventBusType> &event_bus,
    const std::string &render_node,
    std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context);

/**
 * Stop the party compositor and switch the Moonlight stream back to normal.
 *
 * @param state The party state to tear down
 * @param session_id The Moonlight session ID
 * @param event_bus The event bus (used to fire SwitchStreamProducerEvents to switch back)
 */
void stop_party(
    std::shared_ptr<PartyCompositorState> state,
    std::size_t session_id,
    const std::shared_ptr<wolf::core::events::EventBusType> &event_bus);

/**
 * Active party state — lives on AppState, not StreamSession.
 * Holds everything needed to manage a running party across reconnections.
 */
struct ActivePartyState {
  std::shared_ptr<PartyCompositorState> compositor;
  std::shared_ptr<wolf::core::events::ControllerHubState> hub;
  std::vector<std::string> lobby_ids;
  std::string layout_name;
  std::string tile_aspect; // "16:9" or "4:3"
  std::string party_base_interpipe; // e.g. "party_12345" for SwitchStreamProducerEvents
  std::size_t original_session_id;  // session that started the party
  bool global = true; // true: all Moonlight connections join party. false: only original session.
  int focused_slot = 1; // 1-based lobby slot that receives mouse/keyboard/touch

  // Per-session controller offset counter — prevents controller number collisions
  // Session A: 0-3, Session B: 100-103, Session C: 200-203, etc.
  int next_offset = 0;

  // Per-slot lobby creation data for restart
  struct LobbyRestartInfo {
    std::string profile_id;
    std::string app_title;
    std::string runner_state_folder;
    std::shared_ptr<wolf::core::events::Runner> runner;
    wolf::core::events::VideoSettings video_settings;
    wolf::core::events::AudioSettings audio_settings;
  };
  std::vector<LobbyRestartInfo> lobby_restart_info;
};

} // namespace streaming
