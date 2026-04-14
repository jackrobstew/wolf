#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <gst/gst.h>
#include "party_compositor.hpp"

namespace streaming {

// ─── Player/Party info ───────────────────────────────────────────────

struct OverlayPlayerInfo {
  int slot_number = 0;
  std::string profile_name;
  std::string app_name;
  bool controller_connected = false;
  int controller_number = -1;
  // Live button state for visualization (updated by input handler)
  std::uint16_t buttons = 0;
  std::int16_t left_stick_x = 0, left_stick_y = 0;
  std::int16_t right_stick_x = 0, right_stick_y = 0;
  std::uint8_t left_trigger = 0, right_trigger = 0;
};

// ─── System stats for overlay display ────────────────────────────────

struct OverlayStats {
  float gpu_util = 0;
  float gpu_enc = 0;
  float gpu_temp = 0;
  float cpu_load = 0;
  std::chrono::steady_clock::time_point party_start{};
};

// ─── Notifications ───────────────────────────────────────────────────

struct DeviceNotification {
  int controller_number;
  int selected_slot;         // 1-based slot the user is hovering (default: first empty or 1)
  bool confirmed = false;    // user pressed A
  bool dismissing = false;   // slide-out animation in progress

  std::chrono::steady_clock::time_point appear_time;
  std::chrono::steady_clock::time_point last_input;
  std::chrono::steady_clock::time_point dismiss_start;

  static constexpr auto AUTO_DISMISS = std::chrono::seconds(15);
  static constexpr auto SLIDE_DURATION = std::chrono::milliseconds(300);

  float age_seconds() const {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - appear_time).count();
  }

  float slide_in_progress() const {
    float t = age_seconds() / 0.3f; // 300ms slide-in
    return t > 1.0f ? 1.0f : t;
  }

  float slide_out_progress() const {
    if (!dismissing) return 0.0f;
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - dismiss_start).count() / 0.3f;
    return t > 1.0f ? 1.0f : t;
  }

  bool should_remove() const {
    return dismissing && slide_out_progress() >= 1.0f;
  }

  bool timed_out() const {
    return !confirmed && !dismissing &&
           std::chrono::steady_clock::now() - appear_time > AUTO_DISMISS;
  }
};

// ─── Menu system ─────────────────────────────────────────────────────

enum class MenuScreen {
  NONE,
  PLAYER,
  PLAYER_SWAP_CTRL,
  PLAYER_SWAP_POS,
  PARTY,
};

enum class MenuAction {
  NONE,
  SWAP_CONTROLLERS,
  SWAP_POSITION,
  RESTART_GAME,
  UNPAIR_ALL,
  END_PARTY,
};

// ─── Main overlay state ──────────────────────────────────────────────

struct OverlayState {
  // ── Tile layout (set once at party start) ──
  std::vector<TilePosition> tiles;
  std::string tile_aspect;
  std::string layout_name;

  // ── Player info (updated by endpoints + input handler) ──
  std::mutex info_mtx;
  std::vector<OverlayPlayerInfo> players;
  std::vector<float> player_volumes; // 0.0–1.0 per slot

  // ── System stats (updated periodically) ──
  OverlayStats stats;

  // ── Audio pipeline reference (for runtime volume control) ──
  GstElement *audio_pipeline = nullptr; // set after audio pipeline starts

  // ── Overlay appsrc (for GPU-native rendering) ──
  GstElement *appsrc = nullptr; // set by setup_overlay, used by render thread

  // ── Per-session controller offset (prevents multi-device collisions) ──
  std::map<std::size_t, int> session_ctrl_offsets; // set by VideoSession handler

  // ── Party mode: global vs private ──
  std::atomic<bool> party_global{true}; // true = all clients join party
  std::function<void(bool)> on_toggle_global; // callback to sync with ActivePartyState

  // ── Startup hint (auto-dismiss after a few seconds) ──
  std::string hint_text;
  std::chrono::steady_clock::time_point hint_start{};
  static constexpr auto HINT_DURATION = std::chrono::seconds(8);

  // ── System health alert (set by stats thread) ──
  std::string alert_text;
  std::chrono::steady_clock::time_point alert_start{};
  static constexpr auto ALERT_DURATION = std::chrono::seconds(5);

  // ── Party splash (shown on party start) ──
  bool show_splash = true;
  std::chrono::steady_clock::time_point splash_start{};
  static constexpr auto SPLASH_DURATION = std::chrono::seconds(4);
  int splash_player_count = 0;
  std::string splash_layout;

  // ── Menu state ──
  std::atomic<MenuScreen> screen{MenuScreen::NONE};
  std::atomic<int> triggering_slot{-1};
  std::atomic<int> triggering_device{-1};
  std::atomic<int> menu_index{0};
  std::atomic<int> submenu_index{0};
  std::chrono::steady_clock::time_point menu_open_time{};

  static constexpr auto MENU_AUTO_DISMISS = std::chrono::seconds(15);
  static constexpr auto TOGGLE_COOLDOWN = std::chrono::milliseconds(500);
  std::chrono::steady_clock::time_point last_toggle{};

  // ── Notifications (independent of menus) ──
  std::mutex notification_mtx;
  std::vector<DeviceNotification> notifications;

  // ── Action callbacks (set by endpoints.cpp) ──
  std::function<void(int, int)> on_swap;
  std::function<void(int, int)> on_swap_position;
  std::function<void(int)> on_restart_game;   // restart lobby for slot
  std::function<void()> on_unpair_all;
  std::function<void()> on_end_party;
  std::function<void(std::size_t, int)> on_focus_slot; // retarget a stream session's mouse/keyboard/touch to slot
  std::function<void(int, int)> on_pair; // pair controller_number to slot

  // ── Menu navigation ──
  void open_player_menu(int slot, int device) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_toggle < TOGGLE_COOLDOWN) return;
    last_toggle = now;
    if (screen.load() != MenuScreen::NONE) {
      screen.store(MenuScreen::NONE);
      return;
    }
    triggering_slot.store(slot);
    triggering_device.store(device);
    menu_index.store(0);
    screen.store(MenuScreen::PLAYER);
    menu_open_time = now;
  }

  void open_party_menu(int device) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_toggle < TOGGLE_COOLDOWN) return;
    last_toggle = now;
    if (screen.load() != MenuScreen::NONE) {
      screen.store(MenuScreen::NONE);
      return;
    }
    triggering_device.store(device);
    menu_index.store(0);
    screen.store(MenuScreen::PARTY);
    menu_open_time = now;
  }

  void menu_up(int item_count) {
    menu_open_time = std::chrono::steady_clock::now();
    int idx = menu_index.load();
    menu_index.store(idx <= 0 ? item_count - 1 : idx - 1);
  }

  void menu_down(int item_count) {
    menu_open_time = std::chrono::steady_clock::now();
    int idx = menu_index.load();
    menu_index.store(idx >= item_count - 1 ? 0 : idx + 1);
  }

  void dismiss_menu() {
    screen.store(MenuScreen::NONE);
  }

  void check_menu_timeout() {
    if (screen.load() != MenuScreen::NONE) {
      if (std::chrono::steady_clock::now() - menu_open_time > MENU_AUTO_DISMISS) {
        screen.store(MenuScreen::NONE);
      }
    }
  }

  // ── Notification helpers ──
  void add_notification(int controller_number, int num_slots) {
    std::lock_guard<std::mutex> lock(notification_mtx);
    // Don't add duplicate
    for (auto &n : notifications) {
      if (n.controller_number == controller_number && !n.dismissing) return;
    }
    // Default to first empty slot
    int default_slot = 1;
    {
      std::lock_guard<std::mutex> info_lock(info_mtx);
      for (auto &p : players) {
        if (!p.controller_connected) { default_slot = p.slot_number; break; }
      }
    }
    auto now = std::chrono::steady_clock::now();
    notifications.push_back({
      .controller_number = controller_number,
      .selected_slot = default_slot,
      .appear_time = now,
      .last_input = now,
    });
  }

  void dismiss_notification(int controller_number) {
    std::lock_guard<std::mutex> lock(notification_mtx);
    // Instant removal — no animation. Animation was causing Moonlight decode issues.
    notifications.erase(
      std::remove_if(notifications.begin(), notifications.end(),
        [controller_number](const DeviceNotification &n) {
          return n.controller_number == controller_number;
        }),
      notifications.end());
  }

  // Caller MUST hold notification_mtx
  void cleanup_notifications() {
    notifications.erase(
      std::remove_if(notifications.begin(), notifications.end(),
        [](const DeviceNotification &n) { return n.should_remove(); }),
      notifications.end());
    // Auto-dismiss timed out notifications
    for (auto &n : notifications) {
      if (n.timed_out() && !n.dismissing) {
        n.dismissing = true;
        n.dismiss_start = std::chrono::steady_clock::now();
      }
    }
  }

  DeviceNotification* find_notification(int controller_number) {
    // Caller must hold notification_mtx
    for (auto &n : notifications) {
      if (n.controller_number == controller_number && !n.dismissing) return &n;
    }
    return nullptr;
  }

  // ── Process pending actions ──
  std::atomic<MenuAction> pending_action{MenuAction::NONE};
  std::atomic<int> action_param_a{0};
  std::atomic<int> action_param_b{0};

  void process_actions() {
    auto action = pending_action.exchange(MenuAction::NONE);
    if (action == MenuAction::SWAP_CONTROLLERS && on_swap)
      on_swap(action_param_a.load(), action_param_b.load());
    else if (action == MenuAction::SWAP_POSITION && on_swap_position)
      on_swap_position(action_param_a.load(), action_param_b.load());
    else if (action == MenuAction::RESTART_GAME && on_restart_game)
      on_restart_game(action_param_a.load());
    else if (action == MenuAction::UNPAIR_ALL && on_unpair_all)
      on_unpair_all();
    else if (action == MenuAction::END_PARTY && on_end_party)
      on_end_party();
  }
};

// ── Menu item counts (shared with input handler) ────────────────────
constexpr int PLAYER_MENU_COUNT = 4; // Swap, Volume, Reset Game, Party Settings
constexpr int PARTY_MENU_COUNT = 4;  // Global/Private, Unpair All, Restart Party, End Party

// ── GStreamer callbacks ──────────────────────────────────────────────

void on_overlay_draw(GstElement *overlay, void *cr, guint64 timestamp,
                     guint64 duration, gpointer user_data);

void setup_overlay(GstElement *pipeline, std::shared_ptr<OverlayState> state);

} // namespace streaming
