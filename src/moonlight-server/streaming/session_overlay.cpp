#include "session_overlay.hpp"
#include <cairo/cairo.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <helpers/logger.hpp>
#include <cmath>
#include <algorithm>
#include <thread>

namespace streaming {

// ── Drawing helpers ──────────────────────────────────────────────────

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

static const double PC[][3] = {
  {0.133, 0.773, 0.369}, // P1 green
  {0.231, 0.510, 0.965}, // P2 blue
  {0.961, 0.620, 0.043}, // P3 amber
  {0.925, 0.282, 0.600}, // P4 pink
  {0.596, 0.333, 0.878}, // P5 purple
  {0.082, 0.749, 0.773}, // P6 cyan
  {0.957, 0.357, 0.357}, // P7 red
  {0.459, 0.780, 0.235}, // P8 lime
};

// ── Party splash rendering ──────────────────────────────────────────

static void draw_splash(cairo_t *cr, OverlayState *state) {
  if (!state->show_splash) return;
  auto elapsed = std::chrono::steady_clock::now() - state->splash_start;
  if (elapsed > OverlayState::SPLASH_DURATION) {
    state->show_splash = false;
    return;
  }

  float age = std::chrono::duration<float>(elapsed).count();
  float total = std::chrono::duration<float>(OverlayState::SPLASH_DURATION).count();

  // Phase 1: 0-0.6s — fade in + scanline reveal
  // Phase 2: 0.6-2.5s — hold
  // Phase 3: 2.5-4s — fade out
  float alpha = 1.0f;
  if (age < 0.6f) alpha = age / 0.6f;
  else if (age > total - 1.5f) alpha = (total - age) / 1.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  // Dim background
  cairo_rectangle(cr, 0, 0, 1920, 1080);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6 * alpha);
  cairo_fill(cr);

  // Green accent line (scanline effect — grows from center)
  float line_width = age < 0.6f ? (age / 0.6f) * 400.0f : 400.0f;
  double lx = (1920 - line_width) / 2.0;
  cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.8 * alpha);
  cairo_set_line_width(cr, 2.0);
  cairo_move_to(cr, lx, 480);
  cairo_line_to(cr, lx + line_width, 480);
  cairo_stroke(cr);

  // "WOLF PARTY" title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 64);
  cairo_set_source_rgba(cr, 0.95, 0.95, 0.97, alpha);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, "WOLF PARTY", &ext);
  cairo_move_to(cr, (1920 - ext.width) / 2.0, 540);
  cairo_show_text(cr, "WOLF PARTY");

  // Subtitle: "4 Player • 4:3"
  if (age > 0.3f) {
    float sub_alpha = std::min((age - 0.3f) / 0.4f, 1.0f) * alpha;
    cairo_set_font_size(cr, 24);
    cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, sub_alpha);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    auto sub = fmt::format("{} Player", state->splash_player_count);
    if (!state->splash_layout.empty()) sub += fmt::format("  •  {}", state->splash_layout);
    cairo_text_extents(cr, sub.c_str(), &ext);
    cairo_move_to(cr, (1920 - ext.width) / 2.0, 580);
    cairo_show_text(cr, sub.c_str());
  }

  // Bottom accent line
  cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.8 * alpha);
  cairo_move_to(cr, lx, 600);
  cairo_line_to(cr, lx + line_width, 600);
  cairo_stroke(cr);

  // Tagline — fades in later
  if (age > 0.8f) {
    float tag_alpha = std::min((age - 0.8f) / 0.5f, 1.0f) * alpha;
    cairo_set_font_size(cr, 18);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.55, 0.7 * tag_alpha);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    const char *tagline = "Grab a controller and mash some buttons!";
    cairo_text_extents(cr, tagline, &ext);
    cairo_move_to(cr, (1920 - ext.width) / 2.0, 640);
    cairo_show_text(cr, tagline);
  }
}

// ── System health alert rendering ───────────────────────────────────

static void draw_alert(cairo_t *cr, OverlayState *state) {
  if (state->alert_text.empty()) return;
  auto elapsed = std::chrono::steady_clock::now() - state->alert_start;
  if (elapsed > OverlayState::ALERT_DURATION) {
    state->alert_text.clear();
    return;
  }

  float age = std::chrono::duration<float>(elapsed).count();
  float total = std::chrono::duration<float>(OverlayState::ALERT_DURATION).count();
  float alpha = 1.0f;
  if (age < 0.3f) alpha = age / 0.3f;
  else if (age > total - 1.0f) alpha = (total - age) / 1.0f;

  // Top-left alert badge
  const double pad = 12;
  cairo_set_font_size(cr, 16);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, state->alert_text.c_str(), &ext);
  double bw = ext.width + pad * 2 + 20; // 20 for icon space
  double bh = 32;
  double bx = 16, by = 16;

  // Background
  rounded_rect(cr, bx, by, bw, bh, 6);
  cairo_set_source_rgba(cr, 0.15, 0.05, 0.02, 0.9 * alpha);
  cairo_fill(cr);
  // Border — amber/red warning
  rounded_rect(cr, bx, by, bw, bh, 6);
  cairo_set_source_rgba(cr, 0.961, 0.620, 0.043, 0.7 * alpha);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  // Warning dot
  cairo_arc(cr, bx + 16, by + bh / 2, 4, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 0.961, 0.620, 0.043, alpha);
  cairo_fill(cr);

  // Text
  cairo_set_source_rgba(cr, 0.95, 0.85, 0.70, alpha);
  cairo_move_to(cr, bx + 28, by + 22);
  cairo_show_text(cr, state->alert_text.c_str());
}

// ── Hint banner rendering ────────────────────────────────────────────

static void draw_hint(cairo_t *cr, OverlayState *state) {
  if (state->hint_text.empty()) return;
  auto elapsed = std::chrono::steady_clock::now() - state->hint_start;
  if (elapsed > OverlayState::HINT_DURATION) {
    state->hint_text.clear();
    return;
  }

  // Fade in first 0.8s, fade out last 2.5s
  float age = std::chrono::duration<float>(elapsed).count();
  float total = std::chrono::duration<float>(OverlayState::HINT_DURATION).count();
  float alpha = 1.0f;
  if (age < 0.8f) alpha = age / 0.8f;
  else if (age > total - 2.5f) alpha = (total - age) / 2.5f;

  // Bottom-center banner
  const double bar_w = 520;
  const double bar_h = 44;
  double bx = (1920 - bar_w) / 2.0;
  double by = 1080 - 80;

  rounded_rect(cr, bx, by, bar_w, bar_h, 10);
  cairo_set_source_rgba(cr, 0.06, 0.08, 0.06, 0.85 * alpha);
  cairo_fill(cr);
  rounded_rect(cr, bx, by, bar_w, bar_h, 10);
  cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.3 * alpha);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 18);
  cairo_set_source_rgba(cr, 0.8, 0.8, 0.83, 0.9 * alpha);

  // Center text
  cairo_text_extents_t extents;
  cairo_text_extents(cr, state->hint_text.c_str(), &extents);
  cairo_move_to(cr, bx + (bar_w - extents.width) / 2.0, by + 28);
  cairo_show_text(cr, state->hint_text.c_str());
}

// ── Notification rendering ───────────────────────────────────────────

static void draw_notifications(cairo_t *cr, OverlayState *state) {
  std::lock_guard<std::mutex> lock(state->notification_mtx);
  if (state->notifications.empty()) return;
  state->cleanup_notifications();
  if (state->notifications.empty()) return; // all got cleaned up

  const double bar_w = 560;
  const double bar_h = 80;
  const double margin = 24;
  const double gap = 12;
  const double radius = 14;

  int idx = 0;
  for (auto &notif : state->notifications) {
    // Animation
    float slide_in = notif.slide_in_progress();
    float slide_out = notif.slide_out_progress();
    float x_offset = notif.dismissing
      ? (1.0f - slide_out) * 0.0f + slide_out * (bar_w + margin) // slide right off
      : (1.0f - slide_in) * (bar_w + margin); // slide in from right

    double bx = 1920 - bar_w - margin + x_offset;
    double by = margin + idx * (bar_h + gap);

    // Opacity for slide animations
    float alpha = notif.dismissing ? (1.0f - slide_out) : std::min(slide_in * 2.0f, 1.0f);

    // Background — dark with slight green tint for contrast
    rounded_rect(cr, bx, by, bar_w, bar_h, radius);
    cairo_set_source_rgba(cr, 0.06, 0.08, 0.06, 0.92 * alpha);
    cairo_fill(cr);

    // Border — subtle green glow
    rounded_rect(cr, bx, by, bar_w, bar_h, radius);
    cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.4 * alpha);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Activity dots (left side) — pulse based on last_input freshness
    double dot_x = bx + 28;
    double dot_y = by + bar_h / 2;
    auto ms_since_input = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - notif.last_input).count();
    bool active = ms_since_input < 300;

    for (int d = 0; d < 3; ++d) {
      double dx = dot_x + d * 16;
      double dot_r = active ? 7.0 : 5.0;
      bool dot_on = active && (ms_since_input < 100 * (d + 1));
      cairo_arc(cr, dx, dot_y, dot_r, 0, 2 * M_PI);
      if (dot_on) {
        cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.9 * alpha);
      } else {
        cairo_set_source_rgba(cr, 0.3, 0.3, 0.32, 0.6 * alpha);
      }
      cairo_fill(cr);
    }

    // Controller label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 22);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.88, alpha);
    cairo_move_to(cr, bx + 85, by + 32);
    auto label = fmt::format("Controller {}", notif.controller_number);
    cairo_show_text(cr, label.c_str());

    // Arrow + selected slot
    int sel = notif.selected_slot;
    int ci = (sel - 1) % 8;
    cairo_set_font_size(cr, 18);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.54, 0.7 * alpha);
    cairo_move_to(cr, bx + 85, by + 60);
    cairo_show_text(cr, "→");

    // Player badge with color
    cairo_set_source_rgba(cr, PC[ci][0], PC[ci][1], PC[ci][2], alpha);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24);
    cairo_move_to(cr, bx + 115, by + 62);
    std::string profile_name = "";
    {
      std::lock_guard<std::mutex> info_lock(state->info_mtx);
      if (sel >= 1 && sel <= (int)state->players.size()) {
        profile_name = state->players[sel - 1].profile_name;
      }
    }
    auto slot_label = fmt::format("P{}", sel);
    cairo_show_text(cr, slot_label.c_str());

    // Profile name
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 20);
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.73, 0.8 * alpha);
    cairo_move_to(cr, bx + 165, by + 62);
    cairo_show_text(cr, profile_name.empty() ? "(empty)" : profile_name.c_str());

    // Up/down arrows on right side
    cairo_set_font_size(cr, 28);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.54, 0.6 * alpha);
    cairo_move_to(cr, bx + bar_w - 55, by + 30);
    cairo_show_text(cr, "▲");
    cairo_move_to(cr, bx + bar_w - 55, by + 64);
    cairo_show_text(cr, "▼");

    idx++;
  }
}

// ── Player Menu rendering ────────────────────────────────────────────

static const char *PLAYER_MENU_ITEMS[] = {
  "Swap...",
  "Volume",
  "Reset Game",
  "Party Settings...",
};
// PLAYER_MENU_COUNT defined in session_overlay.hpp

static void draw_player_menu(cairo_t *cr, OverlayState *state) {
  int slot = state->triggering_slot.load();
  if (slot < 1 || slot > (int)state->tiles.size()) return;
  auto &tile = state->tiles[slot - 1];
  int menu_idx = state->menu_index.load();

  const double panel_w = 480;
  const double panel_h = 340;
  const double radius = 14;
  const double padding = 22;
  const double item_h = 52;

  // Center on the triggering player's tile
  double px = tile.x + (tile.w - panel_w) / 2.0;
  double py = tile.y + (tile.h - panel_h) / 2.0;

  // Semi-transparent backdrop over the tile
  cairo_rectangle(cr, tile.x, tile.y, tile.w, tile.h);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
  cairo_fill(cr);

  // Panel background
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.95);
  cairo_fill(cr);
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  double cx = px + padding;
  double cy = py + padding;

  // Header
  int ci = (slot - 1) % 8;
  std::string profile = "";
  {
    std::lock_guard<std::mutex> lock(state->info_mtx);
    if (slot <= (int)state->players.size())
      profile = state->players[slot - 1].profile_name;
  }
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 26);
  cairo_set_source_rgb(cr, PC[ci][0], PC[ci][1], PC[ci][2]);
  cairo_move_to(cr, cx, cy + 24);
  auto header = fmt::format("P{}  {}", slot, profile);
  cairo_show_text(cr, header.c_str());
  cy += 48;

  // Divider
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
  cairo_move_to(cr, cx, cy); cairo_line_to(cr, cx + panel_w - padding * 2, cy);
  cairo_set_line_width(cr, 1); cairo_stroke(cr);
  cy += 8;

  // Menu items
  cairo_set_font_size(cr, 22);
  for (int i = 0; i < PLAYER_MENU_COUNT; ++i) {
    bool selected = (i == menu_idx);

    if (selected) {
      rounded_rect(cr, cx - 4, cy, panel_w - padding * 2 + 8, item_h - 4, 6);
      cairo_set_source_rgba(cr, PC[ci][0], PC[ci][1], PC[ci][2], 0.12);
      cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           selected ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgba(cr, 0.91, 0.91, 0.93, selected ? 1.0 : 0.5);

    // Volume item: show bar
    if (i == 1) {
      cairo_move_to(cr, cx + 8, cy + 36);
      cairo_show_text(cr, "Volume");

      // Volume bar
      float vol = 1.0f;
      {
        std::lock_guard<std::mutex> lock(state->info_mtx);
        if (slot - 1 < (int)state->player_volumes.size())
          vol = state->player_volumes[slot - 1];
      }
      double bar_x = cx + 150;
      double bar_y = cy + 22;
      double bar_w = 220;
      double bar_h = 12;
      // Background
      rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 4);
      cairo_set_source_rgba(cr, 0.2, 0.2, 0.22, 0.8);
      cairo_fill(cr);
      // Fill
      rounded_rect(cr, bar_x, bar_y, bar_w * vol, bar_h, 4);
      cairo_set_source_rgba(cr, PC[ci][0], PC[ci][1], PC[ci][2], selected ? 0.9 : 0.5);
      cairo_fill(cr);
      // Percentage
      cairo_set_font_size(cr, 18);
      cairo_set_source_rgba(cr, 0.7, 0.7, 0.73, 0.8);
      cairo_move_to(cr, bar_x + bar_w + 12, cy + 34);
      auto pct = fmt::format("{}%", (int)(vol * 100));
      cairo_show_text(cr, pct.c_str());
      cairo_set_font_size(cr, 22);
    } else {
      auto label = fmt::format("{}{}", selected ? "> " : "  ", PLAYER_MENU_ITEMS[i]);
      cairo_move_to(cr, cx + 8, cy + 36);
      cairo_show_text(cr, label.c_str());
    }

    cy += item_h;
  }

  // Footer hint
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, 0.4, 0.4, 0.44, 0.6);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_move_to(cr, cx, py + panel_h - 12);
  cairo_show_text(cr, "D-pad navigate | A select | B close");
}

// ── Party Menu rendering ─────────────────────────────────────────────

// Index 0 is dynamic ("Mode: Global" / "Mode: Private"), drawn specially
static const char *PARTY_MENU_ITEMS[] = {
  "",            // placeholder — drawn dynamically
  "Unpair All",
  "Restart Party",
  "End Party",
};
// PARTY_MENU_COUNT defined in session_overlay.hpp

static void draw_party_menu(cairo_t *cr, OverlayState *state) {
  int menu_idx = state->menu_index.load();
  int num_players = 0;
  {
    std::lock_guard<std::mutex> lock(state->info_mtx);
    num_players = (int)state->players.size();
  }

  const double panel_w = 620;
  const double panel_h = 180 + num_players * 44 + PARTY_MENU_COUNT * 56;
  const double radius = 16;
  const double padding = 28;

  double px = (1920 - panel_w) / 2.0;
  double py = (1080 - panel_h) / 2.0;

  // Full-screen dim
  cairo_rectangle(cr, 0, 0, 1920, 1080);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_fill(cr);

  // Panel
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.96);
  cairo_fill(cr);
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  double cx = px + padding;
  double cy = py + padding;

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 28);
  cairo_set_source_rgba(cr, 0.91, 0.91, 0.93, 1.0);
  cairo_move_to(cr, cx, cy + 26);
  cairo_show_text(cr, "WOLF PARTY");

  // Stats bar — compact system info
  cairo_set_font_size(cr, 14);
  cairo_set_source_rgba(cr, 0.35, 0.35, 0.40, 0.8);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  auto stats_str = fmt::format("{}  {}  GPU {}%  ENC {}%  {:.0f}°C  CPU {:.1f}",
      state->layout_name, state->tile_aspect,
      (int)state->stats.gpu_util, (int)state->stats.gpu_enc,
      state->stats.gpu_temp, state->stats.cpu_load);
  cairo_move_to(cr, cx, cy + 48);
  cairo_show_text(cr, stats_str.c_str());
  cy += 60;

  // Player rows
  {
    std::lock_guard<std::mutex> lock(state->info_mtx);
    cairo_set_font_size(cr, 20);
    for (int i = 0; i < num_players; ++i) {
      auto &p = state->players[i];
      int ci = i % 8;

      cairo_set_source_rgb(cr, PC[ci][0], PC[ci][1], PC[ci][2]);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      auto badge = fmt::format("P{}", p.slot_number);
      cairo_move_to(cr, cx, cy + 20);
      cairo_show_text(cr, badge.c_str());

      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_source_rgba(cr, 0.8, 0.8, 0.83, 0.9);
      cairo_move_to(cr, cx + 52, cy + 20);
      cairo_show_text(cr, p.profile_name.c_str());

      // Controller status dot
      double dot_x = cx + 260;
      cairo_arc(cr, dot_x, cy + 15, 5.5, 0, 2 * M_PI);
      if (p.controller_connected) {
        cairo_set_source_rgb(cr, 0.133, 0.773, 0.369);
      } else {
        cairo_set_source_rgb(cr, 0.937, 0.267, 0.267);
      }
      cairo_fill(cr);

      cairo_set_source_rgba(cr, 0.6, 0.6, 0.63, 0.7);
      cairo_set_font_size(cr, 16);
      cairo_move_to(cr, dot_x + 14, cy + 20);
      if (p.controller_connected) {
        auto ctrl = fmt::format("Ctrl {}", p.controller_number);
        cairo_show_text(cr, ctrl.c_str());
      } else {
        cairo_show_text(cr, "—");
      }

      // Volume
      float vol = (i < (int)state->player_volumes.size()) ? state->player_volumes[i] : 1.0f;
      cairo_move_to(cr, cx + 460, cy + 20);
      auto vol_str = fmt::format("{}%", (int)(vol * 100));
      cairo_show_text(cr, vol_str.c_str());

      cairo_set_font_size(cr, 20);
      cy += 44;
    }
  }

  // Divider
  cy += 4;
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
  cairo_move_to(cr, cx, cy); cairo_line_to(cr, cx + panel_w - padding * 2, cy);
  cairo_set_line_width(cr, 1); cairo_stroke(cr);
  cy += 10;

  // Menu items
  cairo_set_font_size(cr, 22);
  for (int i = 0; i < PARTY_MENU_COUNT; ++i) {
    bool selected = (i == menu_idx);

    if (selected) {
      rounded_rect(cr, cx - 4, cy, panel_w - padding * 2 + 8, 28, 6);
      cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.12);
      cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           selected ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);

    // "End Party" in red
    if (i == PARTY_MENU_COUNT - 1) {
      cairo_set_source_rgba(cr, 0.937, 0.267, 0.267, selected ? 1.0 : 0.6);
    } else if (i == 0) {
      // Global/Private toggle — green for global, blue for private
      bool is_global = state->party_global.load();
      if (is_global)
        cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, selected ? 1.0 : 0.6);
      else
        cairo_set_source_rgba(cr, 0.231, 0.510, 0.965, selected ? 1.0 : 0.6);
    } else {
      cairo_set_source_rgba(cr, 0.91, 0.91, 0.93, selected ? 1.0 : 0.5);
    }

    std::string label;
    if (i == 0) {
      bool is_global = state->party_global.load();
      label = fmt::format("{}Mode: {}", selected ? "> " : "  ", is_global ? "Global" : "Private");
    } else {
      label = fmt::format("{}{}", selected ? "> " : "  ", PARTY_MENU_ITEMS[i]);
    }
    cairo_move_to(cr, cx + 8, cy + 36);
    cairo_show_text(cr, label.c_str());
    cy += 56;
  }

  // Footer
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, 0.4, 0.4, 0.44, 0.6);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_move_to(cr, cx, py + panel_h - 12);
  cairo_show_text(cr, "D-pad navigate | A select | B close");
}

// ── Swap submenu rendering ───────────────────────────────────────────

static void draw_swap_submenu(cairo_t *cr, OverlayState *state) {
  int trigger_slot = state->triggering_slot.load();
  int target = state->submenu_index.load();
  auto screen = state->screen.load();
  bool is_ctrl_swap = (screen == MenuScreen::PLAYER_SWAP_CTRL);

  int num_players = 0;
  std::vector<OverlayPlayerInfo> players_copy;
  {
    std::lock_guard<std::mutex> lock(state->info_mtx);
    num_players = (int)state->players.size();
    players_copy = state->players;
  }
  if (num_players < 2) return;

  const double panel_w = 440;
  const double item_h = 52;
  const double padding = 24;
  const double radius = 14;
  const double header_h = 56;
  const double footer_h = 36;
  double panel_h = header_h + (num_players - 1) * item_h + footer_h + padding * 2;

  // Center on triggering player's tile
  auto &tile = state->tiles[trigger_slot - 1];
  double px = tile.x + (tile.w - panel_w) / 2.0;
  double py = tile.y + (tile.h - panel_h) / 2.0;

  // Dim the tile
  cairo_rectangle(cr, tile.x, tile.y, tile.w, tile.h);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_fill(cr);

  // Panel
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.96);
  cairo_fill(cr);
  rounded_rect(cr, px, py, panel_w, panel_h, radius);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  double cx = px + padding;
  double cy = py + padding;

  // Title
  int ci = (trigger_slot - 1) % 8;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 22);
  cairo_set_source_rgb(cr, PC[ci][0], PC[ci][1], PC[ci][2]);
  cairo_move_to(cr, cx, cy + 20);
  auto title = fmt::format("P{} {} with...",
      trigger_slot, is_ctrl_swap ? "Swap Ctrl" : "Swap Pos");
  cairo_show_text(cr, title.c_str());
  cy += header_h;

  // Player list (skip self)
  cairo_set_font_size(cr, 20);
  for (int i = 0; i < num_players; ++i) {
    int slot = i + 1;
    if (slot == trigger_slot) continue;

    bool selected = (slot == target);
    int pci = i % 8;

    if (selected) {
      rounded_rect(cr, cx - 6, cy, panel_w - padding * 2 + 12, item_h - 4, 8);
      cairo_set_source_rgba(cr, PC[pci][0], PC[pci][1], PC[pci][2], 0.15);
      cairo_fill(cr);
      // Selection indicator
      rounded_rect(cr, cx - 6, cy, 4, item_h - 4, 2);
      cairo_set_source_rgb(cr, PC[pci][0], PC[pci][1], PC[pci][2]);
      cairo_fill(cr);
    }

    // Badge
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_rgb(cr, PC[pci][0], PC[pci][1], PC[pci][2]);
    cairo_move_to(cr, cx + 8, cy + 32);
    auto badge = fmt::format("P{}", slot);
    cairo_show_text(cr, badge.c_str());

    // Profile name
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           selected ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.88, selected ? 1.0 : 0.5);
    cairo_move_to(cr, cx + 56, cy + 32);
    cairo_show_text(cr, players_copy[i].profile_name.c_str());

    // Controller status
    if (players_copy[i].controller_connected) {
      cairo_set_source_rgba(cr, 0.133, 0.773, 0.369, 0.7);
      cairo_set_font_size(cr, 16);
      cairo_move_to(cr, cx + panel_w - padding * 2 - 80, cy + 30);
      auto ctrl = fmt::format("Ctrl {}", players_copy[i].controller_number);
      cairo_show_text(cr, ctrl.c_str());
      cairo_set_font_size(cr, 20);
    }

    cy += item_h;
  }

  // Footer
  cy += 4;
  cairo_set_font_size(cr, 14);
  cairo_set_source_rgba(cr, 0.4, 0.4, 0.44, 0.6);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_move_to(cr, cx, cy + 14);
  cairo_show_text(cr, "A confirm | B back");
}

// ── Main draw callback ───────────────────────────────────────────────

void on_overlay_draw(GstElement *overlay, void *cr_ptr, guint64 timestamp,
                     guint64 duration, gpointer user_data) {
  auto *cr = static_cast<cairo_t *>(cr_ptr);
  if (!cr || !user_data) return;
  auto *state = static_cast<OverlayState *>(user_data);

  // Execute any pending menu actions (atomic exchange, negligible cost)
  state->process_actions();

  // Sync volume sliders to GStreamer audio pipeline (only if audio pipeline exists)
  if (state->audio_pipeline) {
    std::lock_guard<std::mutex> lock(state->info_mtx);
    for (int i = 0; i < (int)state->player_volumes.size(); ++i) {
      auto name = fmt::format("party_vol_{}", i);
      auto *vol_el = gst_bin_get_by_name(GST_BIN(state->audio_pipeline), name.c_str());
      if (vol_el) {
        gdouble current_vol = 0;
        g_object_get(vol_el, "volume", &current_vol, nullptr);
        double target = state->player_volumes[i];
        if (std::abs(current_vol - target) > 0.01) {
          g_object_set(vol_el, "volume", target, nullptr);
        }
        gst_object_unref(vol_el);
      }
    }
  }

  auto screen = state->screen.load();

  // Check menu timeout (cheap atomic ops only)
  if (screen != MenuScreen::NONE) {
    state->check_menu_timeout();
    screen = state->screen.load();
  }

  // Draw notifications (has its own lock + empty check)
  draw_notifications(cr, state);

  // Menus only draw when active
  if (screen == MenuScreen::PLAYER) {
    draw_player_menu(cr, state);
  } else if (screen == MenuScreen::PLAYER_SWAP_CTRL ||
             screen == MenuScreen::PLAYER_SWAP_POS) {
    draw_swap_submenu(cr, state);
  } else if (screen == MenuScreen::PARTY) {
    draw_party_menu(cr, state);
  }
}

// ── Pipeline setup (GPU-native via appsrc) ──────────────────────────

void setup_overlay(GstElement *pipeline, std::shared_ptr<OverlayState> state) {
  auto *src = gst_bin_get_by_name(GST_BIN(pipeline), "overlay_src");
  if (!src) {
    logs::log(logs::warning, "[OVERLAY] appsrc 'overlay_src' not found in pipeline");
    return;
  }
  state->appsrc = src; // pipeline owns the element, we just hold a pointer

  // Party splash animation
  state->show_splash = true;
  state->splash_start = std::chrono::steady_clock::now();
  state->splash_player_count = static_cast<int>(state->tiles.size());
  state->splash_layout = state->layout_name;

  // Show controls hint (appears after splash fades)
  state->hint_text = "Menu: F1  |  Focus: F2-F5  |  Pad: HOME+LB+RB or Sel+B+X+A";
  state->hint_start = std::chrono::steady_clock::now() + std::chrono::seconds(4); // delay until after splash

  // Render thread: draws overlay with Cairo at ~15fps, pushes BGRA to appsrc
  std::thread([state_weak = std::weak_ptr<OverlayState>(state)]() {
    constexpr int W = 1920, H = 1080;
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    auto *cr = cairo_create(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const int buf_size = stride * H;

    logs::log(logs::info, "[OVERLAY] Render thread started ({}x{}, stride={}, {}KB/frame)",
              W, H, stride, buf_size / 1024);

    while (auto s = state_weak.lock()) {
      if (!s->appsrc) break;

      // Clear to fully transparent
      cairo_save(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      cairo_paint(cr);
      cairo_restore(cr);

      // Check if there's anything to draw
      auto screen = s->screen.load();
      bool has_content = (screen != MenuScreen::NONE);
      if (!s->hint_text.empty()) has_content = true;
      if (!s->alert_text.empty()) has_content = true;
      if (s->show_splash) has_content = true;
      {
        std::lock_guard<std::mutex> lock(s->notification_mtx);
        if (!s->notifications.empty()) has_content = true;
        if (has_content) s->cleanup_notifications();
      }

      if (has_content) {
        if (screen != MenuScreen::NONE) {
          s->check_menu_timeout();
          screen = s->screen.load();
        }
        // Layer order: splash (bottom) → game → alert → hint → notifications → menus (top)
        draw_splash(cr, s.get());
        draw_alert(cr, s.get());
        draw_hint(cr, s.get());
        draw_notifications(cr, s.get());
        if (screen == MenuScreen::PLAYER)
          draw_player_menu(cr, s.get());
        else if (screen == MenuScreen::PLAYER_SWAP_CTRL ||
                 screen == MenuScreen::PLAYER_SWAP_POS)
          draw_swap_submenu(cr, s.get());
        else if (screen == MenuScreen::PARTY)
          draw_party_menu(cr, s.get());
      }

      // Push BGRA buffer to appsrc
      cairo_surface_flush(surface);
      auto *data = cairo_image_surface_get_data(surface);
      auto *buffer = gst_buffer_new_allocate(nullptr, buf_size, nullptr);
      gst_buffer_fill(buffer, 0, data, buf_size);
      GST_BUFFER_DURATION(buffer) = GST_SECOND / 15;

      auto ret = gst_app_src_push_buffer(GST_APP_SRC(s->appsrc), buffer); // takes ownership
      if (ret != GST_FLOW_OK) break;

      std::this_thread::sleep_for(std::chrono::milliseconds(66)); // ~15fps
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    logs::log(logs::info, "[OVERLAY] Render thread stopped");
  }).detach();

  gst_object_unref(src);
  logs::log(logs::info, "[OVERLAY] GPU overlay ready (appsrc → cudaupload → compositor)");
}

} // namespace streaming
