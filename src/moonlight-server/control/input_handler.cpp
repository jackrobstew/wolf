#include "state/sessions.hpp"

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <boost/locale.hpp>
#include <control/controller_hub.hpp>
#include <control/input_handler.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <streaming/session_overlay.hpp>
#include <immer/box.hpp>
#include <platforms/input.hpp>
#include <string>

namespace control {

using namespace wolf::core::virtual_display;
using namespace wolf::core::input;
using namespace wolf::core;
using namespace std::string_literals;
using namespace moonlight::control;

std::shared_ptr<events::JoypadTypes> create_new_joypad(const events::StreamSession &session,
                                                       immer::box<std::shared_ptr<ENetPeer>> connected_client,
                                                       int controller_number,
                                                       CONTROLLER_TYPE requested_type,
                                                       uint8_t capabilities) {

  auto on_rumble_fn = ([connected_client, controller_number, aes_key = session.aes_key](int low_freq, int high_freq) {
    auto rumble_pkt = ControlRumblePacket{
        .header = {.type = RUMBLE_DATA, .length = sizeof(ControlRumblePacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .low_freq = boost::endian::native_to_little((uint16_t)low_freq),
        .high_freq = boost::endian::native_to_little((uint16_t)high_freq)};
    std::string plaintext = {(char *)&rumble_pkt, sizeof(rumble_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  auto on_led_fn = ([connected_client, controller_number, aes_key = session.aes_key](int r, int g, int b) {
    auto led_pkt = ControlRGBLedPacket{
        .header{.type = RGB_LED_EVENT, .length = sizeof(ControlRGBLedPacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .r = static_cast<uint8_t>(r),
        .g = static_cast<uint8_t>(g),
        .b = static_cast<uint8_t>(b)};
    std::string plaintext = {(char *)&led_pkt, sizeof(led_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  auto on_adaptive_trigger_fn = ([connected_client, controller_number, aes_key = session.aes_key](
                                     const inputtino::PS5Joypad::TriggerEffect &effect) {
    auto rumble_pkt = ControlAdaptiveTriggerPacket{
        .header{.type = ADAPTIVE_TRIGGER_EVENT, .length = sizeof(ControlAdaptiveTriggerPacket) - sizeof(ControlPacket)},
        .controller_number = boost::endian::native_to_little((uint16_t)controller_number),
        .effect = effect};
    std::string plaintext = {(char *)&rumble_pkt, sizeof(rumble_pkt)};
    encrypt_and_send(plaintext, aes_key, connected_client);
  });

  std::shared_ptr<events::JoypadTypes> new_pad;
  auto controllers_override = session.client_settings->controllers_override;
  auto final_type = controllers_override.size() > controller_number ? controllers_override[controller_number]
                                                                    : wolf::config::ControllerType::AUTO;
  if (final_type == wolf::config::ControllerType::AUTO) {
    switch (requested_type) {
    case XBOX:
      final_type = wolf::config::ControllerType::XBOX;
      break;
    case PS:
      final_type = wolf::config::ControllerType::PS;
      break;
    case NINTENDO:
      final_type = wolf::config::ControllerType::NINTENDO;
      break;
    default:
      final_type = wolf::config::ControllerType::AUTO;
      break;
    }
  }
  switch (final_type) {
  case wolf::config::ControllerType::AUTO:
  case wolf::config::ControllerType::XBOX: {
    logs::log(logs::info,
              "Creating Xbox joypad for controller {} in session {}",
              controller_number,
              session.session_id);
    auto result =
        XboxOneJoypad::create({.name = "Wolf X-Box One (virtual) pad " + std::to_string(controller_number),
                               // https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c#L147
                               .vendor_id = 0x045E,
                               .product_id = 0x02EA,
                               .version = 0x0408});
    if (!result) {
      logs::log(logs::error, "Failed to create Xbox One joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));
    }
    break;
  }
  case wolf::config::ControllerType::PS: {
    logs::log(logs::info, "Creating PS joypad for controller {}", controller_number);
    auto result = PS5Joypad::create(
        {.name = "Wolf DualSense (virtual) pad " + std::to_string(controller_number), .vendor_id = 0x054C, .product_id = 0x0CE6, .version = 0x8111});
    if (!result) {
      logs::log(logs::error, "Failed to create PS5 joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      (*result).set_on_led(on_led_fn);
      (*result).set_on_trigger_effect(on_adaptive_trigger_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));

      // Let's wait for the kernel to pick it up and mount the /dev/ devices
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      std::visit(
          [&session](auto &pad) {
            if (auto wl = *session.wayland_display->load()) {
              for (const auto node : pad.get_udev_events()) {
                if (node.find("ID_INPUT_TOUCHPAD") != node.end()) {
                  add_input_device(*wl, node.at("DEVNAME"));
                }
              }
            }
          },
          *new_pad);
    }
    break;
  }
  case wolf::config::ControllerType::NINTENDO:
    logs::log(logs::info, "Creating Nintendo joypad for controller {}", controller_number);
    auto result = SwitchJoypad::create({.name = "Wolf Nintendo (virtual) pad " + std::to_string(controller_number),
                                        // https://github.com/torvalds/linux/blob/master/drivers/hid/hid-ids.h#L981
                                        .vendor_id = 0x057e,
                                        .product_id = 0x2009,
                                        .version = 0x8111});
    if (!result) {
      logs::log(logs::error, "Failed to create Switch joypad: {}", result.getErrorMessage());
      return {};
    } else {
      (*result).set_on_rumble(on_rumble_fn);
      new_pad = std::make_shared<events::JoypadTypes>(std::move(*result));
    }
    break;
  }

  if (capabilities & ACCELEROMETER && final_type == wolf::config::ControllerType::PS) {
    // Request acceleromenter events from the client at 100 Hz
    logs::log(logs::info, "Requesting accelerometer events for controller {}", controller_number);
    auto accelerometer_pkt = ControlMotionEventPacket{
        .header{.type = MOTION_EVENT, .length = sizeof(ControlMotionEventPacket) - sizeof(ControlPacket)},
        .controller_number = static_cast<uint16_t>(controller_number),
        .reportrate = 100,
        .type = ACCELERATION};
    std::string plaintext = {(char *)&accelerometer_pkt, sizeof(accelerometer_pkt)};
    encrypt_and_send(plaintext, session.aes_key, connected_client);
  }

  if (capabilities & GYRO && final_type == wolf::config::ControllerType::PS) {
    // Request gyroscope events from the client at 100 Hz
    logs::log(logs::info, "Requesting gyroscope events for controller {}", controller_number);
    auto gyro_pkt = ControlMotionEventPacket{
        .header{.type = MOTION_EVENT, .length = sizeof(ControlMotionEventPacket) - sizeof(ControlPacket)},
        .controller_number = static_cast<uint16_t>(controller_number),
        .reportrate = 100,
        .type = GYROSCOPE};
    std::string plaintext = {(char *)&gyro_pkt, sizeof(gyro_pkt)};
    encrypt_and_send(plaintext, session.aes_key, connected_client);
  }

  session.joypads->update([&](events::JoypadList joypads) {
    logs::log(logs::debug,
              "[INPUT] Sending PlugDeviceEvent for joypad {} of type: {}",
              controller_number,
              (int)final_type);

    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session.session_id)};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *new_pad);
    session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    return joypads.set(controller_number, new_pad);
  });
  return new_pad;
}

/**
 * Creates a new PenTablet and saves it into the session;
 * will also trigger a PlugDeviceEvent
 */
bool create_pen_tablet(events::StreamSession &session) {
  logs::log(logs::debug, "[INPUT] Creating new pen tablet");
  auto tablet = PenTablet::create();
  if (!tablet) {
    logs::log(logs::error, "Failed to create pen tablet: {}", tablet.getErrorMessage());
    return false;
  }
  auto tablet_ptr = std::make_shared<PenTablet>(std::move(*tablet));
  session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(
      events::PlugDeviceEvent{.session_id = std::to_string(session.session_id),
                              .udev_events = tablet_ptr->get_udev_events(),
                              .udev_hw_db_entries = tablet_ptr->get_udev_hw_db_entries()}));
  if (auto wl = *session.wayland_display->load()) {
    for (const auto node : tablet_ptr->get_nodes()) {
      add_input_device(*wl, node);
    }
  }
  session.pen_tablet->emplace(std::move(*tablet_ptr));
  return true;
}

/**
 * Creates a new Touch screen and saves it into the session;
 * will also trigger a PlugDeviceEvent
 */
bool create_touch_screen(events::StreamSession &session) {
  logs::log(logs::debug, "[INPUT] Creating new touch screen");
  auto touch = TouchScreen::create();
  if (!touch) {
    logs::log(logs::error, "Failed to create touch screen: {}", touch.getErrorMessage());
    return false;
  }
  auto touch_screen = std::make_shared<TouchScreen>(std::move(*touch));
  session.event_bus->fire_event(immer::box<events::PlugDeviceEvent>(
      events::PlugDeviceEvent{.session_id = std::to_string(session.session_id),
                              .udev_events = touch_screen->get_udev_events(),
                              .udev_hw_db_entries = touch_screen->get_udev_hw_db_entries()}));
  if (auto wl = *session.wayland_display->load()) {
    for (const auto node : touch_screen->get_nodes()) {
      add_input_device(*wl, node);
    }
  }
  session.touch_screen->emplace(std::move(*touch_screen));
  return true;
}

float netfloat_to_0_1(const utils::netfloat &f) {
  return std::clamp(utils::from_netfloat(f), 0.0f, 1.0f);
}

static inline float deg2rad(float degree) {
  return degree * (M_PI / 180.f);
}

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    auto pointer_acceleration = session.client_settings->mouse_acceleration;
    auto delta_x = static_cast<float>(boost::endian::big_to_native(pkt.delta_x)) * pointer_acceleration;
    auto delta_y = static_cast<float>(boost::endian::big_to_native(pkt.delta_y)) * pointer_acceleration;
    std::visit([delta_x, delta_y](auto &mouse) { mouse.move(delta_x, delta_y); }, session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_MOVE_REL_PACKET but no mouse device is present");
  }
}

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    auto pointer_acceleration = session.client_settings->mouse_acceleration;
    float x = boost::endian::big_to_native(pkt.x);
    float y = boost::endian::big_to_native(pkt.y);
    float window_width = boost::endian::big_to_native(pkt.width);
    float window_height = boost::endian::big_to_native(pkt.height);

    auto absolute_x = (x / window_width) * static_cast<float>(session.display_mode.width) * pointer_acceleration;
    auto absolute_y = (y / window_height) * static_cast<float>(session.display_mode.height) * pointer_acceleration;

    std::visit([absolute_x,
                absolute_y,
                screen_width = session.display_mode.width,
                screen_height = session.display_mode.height](
                   auto &mouse) { mouse.move_abs(absolute_x, absolute_y, screen_width, screen_height); },
               session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_MOVE_ABS_PACKET but no mouse device is present");
  }
}

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    if (std::holds_alternative<state::input::Mouse>(session.mouse->value())) {
      Mouse::MOUSE_BUTTON btn_type;

      switch (pkt.button) {
      case 1:
        btn_type = Mouse::LEFT;
        break;
      case 2:
        btn_type = Mouse::MIDDLE;
        break;
      case 3:
        btn_type = Mouse::RIGHT;
        break;
      case 4:
        btn_type = Mouse::SIDE;
        break;
      default:
        btn_type = Mouse::EXTRA;
        break;
      }
      if (pkt.type == MOUSE_BUTTON_PRESS) {
        std::get<state::input::Mouse>(session.mouse->value()).press(btn_type);
      } else {
        std::get<state::input::Mouse>(session.mouse->value()).release(btn_type);
      }
    } else if (std::holds_alternative<wolf::core::virtual_display::WaylandMouse>(session.mouse->value())) {
      if (pkt.type == MOUSE_BUTTON_PRESS) {
        std::get<wolf::core::virtual_display::WaylandMouse>(session.mouse->value()).press(pkt.button);
      } else {
        std::get<wolf::core::virtual_display::WaylandMouse>(session.mouse->value()).release(pkt.button);
      }
    }
  } else {
    logs::log(logs::warning, "Received MOUSE_BUTTON_PACKET but no mouse device is present");
  }
}

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    std::visit(
        [session, scroll_amount = boost::endian::big_to_native(pkt.scroll_amt1)](auto &mouse) {
          auto scroll_acceleration = session.client_settings->v_scroll_acceleration;
          mouse.vertical_scroll(scroll_amount * scroll_acceleration);
        },
        session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_SCROLL_PACKET but no mouse device is present");
  }
}

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session) {
  if (session.mouse->has_value()) {
    std::visit(
        [session, scroll_amount = boost::endian::big_to_native(pkt.scroll_amount)](auto &mouse) {
          auto scroll_acceleration = session.client_settings->h_scroll_acceleration;
          mouse.horizontal_scroll(scroll_amount * scroll_acceleration);
        },
        session.mouse->value());
  } else {
    logs::log(logs::warning, "Received MOUSE_HSCROLL_PACKET but no mouse device is present");
  }
}

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session) {
  // moonlight always sets the high bit; not sure why but mask it off here
  short moonlight_key = (short)boost::endian::little_to_native(pkt.key_code) & (short)0x7fff;

  // ── Party overlay keyboard shortcuts (checked BEFORE keyboard device check,
  //    because the virtual keyboard may not be ready yet during party reconnect) ──
  if (pkt.type == KEY_PRESS) {
    if (moonlight_key == 0x70) { // F1 debug
      auto comp_check = *session.party_compositor;
      logs::log(logs::info, "[OVERLAY] F1 key detected — party_compositor={}, has_overlay={}",
                comp_check ? "SET" : "NULL", (comp_check && comp_check->overlay) ? "YES" : "NO");
    }
    auto comp = *session.party_compositor;
    if (comp && comp->overlay) {
      auto &ov = *comp->overlay;
      auto screen = ov.screen.load();

      constexpr short VK_F1 = 0x70;
      constexpr short VK_F2 = 0x71;
      constexpr short VK_F3 = 0x72;
      constexpr short VK_F4 = 0x73;
      constexpr short VK_F5 = 0x74;
      constexpr short SC_F1 = 0x3B;
      constexpr short SC_F2 = 0x3C;
      constexpr short SC_F3 = 0x3D;
      constexpr short SC_F4 = 0x3E;
      constexpr short SC_F5 = 0x3F;
      constexpr short VK_ESCAPE = 0x1B;
      constexpr short VK_RETURN = 0x0D;
      constexpr short VK_UP = 0x26;
      constexpr short VK_DOWN = 0x28;
      constexpr short VK_LEFT = 0x25;
      constexpr short VK_RIGHT = 0x27;

      auto key_is = [moonlight_key](short vk, short scan) {
        return moonlight_key == vk || moonlight_key == scan;
      };

      // F1: toggle party menu
      if (key_is(VK_F1, SC_F1)) {
        logs::log(logs::info, "[OVERLAY] F1 pressed — screen={}, opening party menu", (int)screen);
        ov.open_party_menu(-1);
        return;
      }

      // F2..F5: direct focus switch (slot 1..4)
      int focus_slot = 0;
      if (key_is(VK_F2, SC_F2))
        focus_slot = 1;
      else if (key_is(VK_F3, SC_F3))
        focus_slot = 2;
      else if (key_is(VK_F4, SC_F4))
        focus_slot = 3;
      else if (key_is(VK_F5, SC_F5))
        focus_slot = 4;

      if (focus_slot > 0) {
        if (ov.on_focus_slot) {
          ov.on_focus_slot(session.session_id, focus_slot);
        } else {
          logs::log(logs::warning,
                    "[PARTY] Focus hotkey pressed but no focus callback is set (session {}, slot {})",
                    session.session_id,
                    focus_slot);
        }
        return;
      }

      // Menu navigation with keyboard (when menu is open)
      if (screen != streaming::MenuScreen::NONE) {
        if (moonlight_key == VK_ESCAPE) {
          if (screen == streaming::MenuScreen::PLAYER_SWAP_CTRL ||
              screen == streaming::MenuScreen::PLAYER_SWAP_POS) {
            ov.screen.store(streaming::MenuScreen::PLAYER);
          } else {
            ov.dismiss_menu();
          }
          return;
        }

        int item_count = 0;
        if (screen == streaming::MenuScreen::PLAYER)
          item_count = streaming::PLAYER_MENU_COUNT;
        else if (screen == streaming::MenuScreen::PARTY)
          item_count = streaming::PARTY_MENU_COUNT;

        if (moonlight_key == VK_UP && item_count > 0) { ov.menu_up(item_count); return; }
        if (moonlight_key == VK_DOWN && item_count > 0) { ov.menu_down(item_count); return; }

        // Enter: same as A button — trigger the selected action
        if (moonlight_key == VK_RETURN) {
          int idx = ov.menu_index.load();
          if (screen == streaming::MenuScreen::PARTY) {
            if (idx == 0) { bool c = ov.party_global.load(); ov.party_global.store(!c); if (ov.on_toggle_global) ov.on_toggle_global(!c); }
            else if (idx == 1) { ov.pending_action.store(streaming::MenuAction::UNPAIR_ALL); ov.dismiss_menu(); }
            else if (idx == 3) { ov.pending_action.store(streaming::MenuAction::END_PARTY); ov.dismiss_menu(); }
          }
          return;
        }

        // Volume: left/right when on volume item in player menu
        if (screen == streaming::MenuScreen::PLAYER && ov.menu_index.load() == 1) {
          int slot = ov.triggering_slot.load();
          if (moonlight_key == VK_LEFT) {
            std::lock_guard<std::mutex> lock(ov.info_mtx);
            if (slot - 1 < (int)ov.player_volumes.size())
              ov.player_volumes[slot - 1] = std::max(0.0f, ov.player_volumes[slot - 1] - 0.1f);
            return;
          }
          if (moonlight_key == VK_RIGHT) {
            std::lock_guard<std::mutex> lock(ov.info_mtx);
            if (slot - 1 < (int)ov.player_volumes.size())
              ov.player_volumes[slot - 1] = std::min(1.0f, ov.player_volumes[slot - 1] + 0.1f);
            return;
          }
        }

        return; // consume all keyboard input while menu is open
      }
    }
  }

  // Virtual keyboard device must exist for normal key passthrough
  if (!session.keyboard->has_value()) {
    return;
  }

  if (pkt.type == KEY_PRESS) {
    int wolf_ui_combo_pressed = 0;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::SHIFT && moonlight_key != M_SHIFT)
      wolf_ui_combo_pressed++;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::CTRL && moonlight_key != M_CTRL)
      wolf_ui_combo_pressed++;
    if (pkt.modifiers & KEYBOARD_MODIFIERS::ALT && moonlight_key != M_ALT)
      wolf_ui_combo_pressed++;

    // CTRL + ALT + SHIFT + W
    const bool wolf_ui_combo = (wolf_ui_combo_pressed == 3 && moonlight_key == 0x57);
    if (wolf_ui_combo) {
      // Ensure modifiers are released before we return to overlay
      std::visit(
          [](auto &keyboard) {
            keyboard.release(M_SHIFT);
            keyboard.release(M_CTRL);
            keyboard.release(M_ALT);
          },
          session.keyboard->value());
      session.event_bus->fire_event(
          immer::box<events::ClientWolfUIComboEvent>{events::ClientWolfUIComboEvent{.session_id = session.session_id}});
      return;
    }

    // Press the virtual modifiers
    if (pkt.modifiers & KEYBOARD_MODIFIERS::SHIFT && moonlight_key != M_SHIFT)
      std::visit([](auto &keyboard) { keyboard.press(M_SHIFT); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::CTRL && moonlight_key != M_CTRL)
      std::visit([](auto &keyboard) { keyboard.press(M_CTRL); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::ALT && moonlight_key != M_ALT)
      std::visit([](auto &keyboard) { keyboard.press(M_ALT); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::META && moonlight_key != M_META)
      std::visit([](auto &keyboard) { keyboard.press(M_META); }, session.keyboard->value());

    // Press the actual key
    std::visit([moonlight_key](auto &keyboard) { keyboard.press(moonlight_key); }, session.keyboard->value());

    // Release the virtual modifiers
    if (pkt.modifiers & KEYBOARD_MODIFIERS::SHIFT && moonlight_key != M_SHIFT)
      std::visit([](auto &keyboard) { keyboard.release(M_SHIFT); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::CTRL && moonlight_key != M_CTRL)
      std::visit([](auto &keyboard) { keyboard.release(M_CTRL); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::ALT && moonlight_key != M_ALT)
      std::visit([](auto &keyboard) { keyboard.release(M_ALT); }, session.keyboard->value());
    if (pkt.modifiers & KEYBOARD_MODIFIERS::META && moonlight_key != M_META)
      std::visit([](auto &keyboard) { keyboard.release(M_META); }, session.keyboard->value());

  } else {
    std::visit([moonlight_key](auto &keyboard) { keyboard.release(moonlight_key); }, session.keyboard->value());
  }
}

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session) {
  if (session.keyboard->has_value()) {
    /* Here we receive a single UTF-8 encoded char at a time,
     * the trick is to convert it to UTF-32 then send CTRL+SHIFT+U+<HEXCODE> in order to produce any
     * unicode character, see: https://en.wikipedia.org/wiki/Unicode_input
     *
     * ex:
     * - when receiving UTF-8 [0xF0 0x9F 0x92 0xA9] (which is '💩')
     * - we'll convert it to UTF-32 [0x1F4A9]
     * - then type: CTRL+SHIFT+U+1F4A9
     * see the conversion at: https://www.compart.com/en/unicode/U+1F4A9
     */
    auto size = boost::endian::big_to_native(pkt.data_size) - sizeof(pkt.packet_type) - 2;
    /* Reading input text as UTF-8 */
    auto utf8 = boost::locale::conv::to_utf<wchar_t>(pkt.text, pkt.text + size, "UTF-8");
    /* Converting to UTF-32 */
    auto utf32 = boost::locale::conv::utf_to_utf<char32_t>(utf8);
    wolf::platforms::input::paste_utf(session.keyboard->value(), utf32);
  } else {
    logs::log(logs::warning, "Received UTF8_TEXT_PACKET but no keyboard device is present");
  }
}

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session) {
  bool has_touch_device = session.touch_screen->has_value();
  if (!has_touch_device) {
    has_touch_device = create_touch_screen(session);
  }
  if (has_touch_device) {
    auto finger_id = boost::endian::little_to_native(pkt.pointer_id);
    auto x = netfloat_to_0_1(pkt.x);
    auto y = netfloat_to_0_1(pkt.y);
    auto pressure_or_distance = netfloat_to_0_1(pkt.pressure_or_distance);
    std::visit(
        [&pkt, &session, &x, &y, &finger_id, &pressure_or_distance](auto &screen) {
          using T = std::decay_t<decltype(screen)>;
          if constexpr (std::is_same_v<T, input::TouchScreen>) {
            // Inputtino TouchScreen only defines place_finger and release_finger methods
            switch (pkt.event_type) {
            case pkts::TOUCH_EVENT_HOVER:
            case pkts::TOUCH_EVENT_DOWN:
            case pkts::TOUCH_EVENT_MOVE: {
              // Convert our 0..360 range to -90..90 relative to Y axis
              int adjusted_angle = pkt.rotation;

              if (adjusted_angle > 90 && adjusted_angle < 270) {
                // Lower hemisphere
                adjusted_angle = 180 - adjusted_angle;
              }

              // Wrap the value if it's out of range
              if (adjusted_angle > 90) {
                adjusted_angle -= 360;
              } else if (adjusted_angle < -90) {
                adjusted_angle += 360;
              }
              screen.place_finger(finger_id, x, y, pressure_or_distance, adjusted_angle);
              break;
            }
            case pkts::TOUCH_EVENT_UP:
            case pkts::TOUCH_EVENT_HOVER_LEAVE:
            case pkts::TOUCH_EVENT_CANCEL:
              screen.release_finger(finger_id);
              break;
            default:
              logs::log(logs::warning, "[INPUT] Unknown touch event type {}", (int)pkt.event_type);
            }
          } else if constexpr (std::is_same_v<T, virtual_display::WaylandTouchScreen>) {
            // For WaylandTouchScreen, we handle the events differently
            switch (pkt.event_type) {
            case pkts::TOUCH_EVENT_HOVER:
            case pkts::TOUCH_EVENT_DOWN:
              screen.down(finger_id, x, y);
              break;
            case pkts::TOUCH_EVENT_MOVE:
              screen.motion(finger_id, x, y);
              break;
            case pkts::TOUCH_EVENT_UP:
              screen.up(finger_id);
              break;
            case pkts::TOUCH_EVENT_HOVER_LEAVE:
            case pkts::TOUCH_EVENT_CANCEL:
              screen.cancel();
              break;
            default:
              logs::log(logs::warning, "[INPUT] Unknown touch event type {}", (int)pkt.event_type);
            }
            // Moonlight TOUCH_EVENT does not include TouchFrame events, so we trigger it manually every time
            screen.frame();
          }
        },
        **session.touch_screen);
  }
}

void pen(const PEN_PACKET &pkt, events::StreamSession &session) {
  bool has_pen_device = session.pen_tablet->has_value();
  if (!has_pen_device) {
    create_pen_tablet(session);
  }

  if (has_pen_device) {
    // First set the buttons
    session.pen_tablet->value().set_btn(PenTablet::PRIMARY, pkt.pen_buttons & PEN_BUTTON_TYPE_PRIMARY);
    session.pen_tablet->value().set_btn(PenTablet::SECONDARY, pkt.pen_buttons & PEN_BUTTON_TYPE_SECONDARY);
    session.pen_tablet->value().set_btn(PenTablet::TERTIARY, pkt.pen_buttons & PEN_BUTTON_TYPE_TERTIARY);

    // Set the tool
    PenTablet::TOOL_TYPE tool;
    switch (pkt.tool_type) {
    case moonlight::control::pkts::TOOL_TYPE_PEN:
      tool = PenTablet::PEN;
      break;
    case moonlight::control::pkts::TOOL_TYPE_ERASER:
      tool = PenTablet::ERASER;
      break;
    default:
      tool = PenTablet::SAME_AS_BEFORE;
      break;
    }

    auto pressure_or_distance = netfloat_to_0_1(pkt.pressure_or_distance);

    // Normalize rotation value to 0-359 degree range
    auto rotation = boost::endian::little_to_native(pkt.rotation);
    if (rotation != PEN_ROTATION_UNKNOWN) {
      rotation %= 360;
    }

    // Here we receive:
    //  - Rotation: degrees from vertical in Y dimension (parallel to screen, 0..360)
    //  - Tilt: degrees from vertical in Z dimension (perpendicular to screen, 0..90)
    float tilt_x = 0;
    float tilt_y = 0;
    // Convert polar coordinates into Y tilt angles
    if (pkt.tilt != PEN_TILT_UNKNOWN && rotation != PEN_ROTATION_UNKNOWN) {
      auto rotation_rads = deg2rad(rotation);
      auto tilt_rads = deg2rad(pkt.tilt);
      auto r = std::sin(tilt_rads);
      auto z = std::cos(tilt_rads);

      tilt_x = std::atan2(std::sin(-rotation_rads) * r, z) * 180.f / M_PI;
      tilt_y = std::atan2(std::cos(-rotation_rads) * r, z) * 180.f / M_PI;
    }

    bool is_touching = pkt.event_type == TOUCH_EVENT_DOWN || pkt.event_type == TOUCH_EVENT_MOVE;

    session.pen_tablet->value().place_tool(tool,
                                           netfloat_to_0_1(pkt.x),
                                           netfloat_to_0_1(pkt.y),
                                           is_touching ? pressure_or_distance : -1,
                                           is_touching ? -1 : pressure_or_distance,
                                           tilt_x,
                                           tilt_y);
  }
}

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  // If a Controller Hub is active, don't create new devices — just track the physical controller
  if (*session.controller_hub) {
    int ctrl_num = pkt.controller_number;
    if (auto comp = *session.party_compositor) {
      if (comp && comp->overlay) {
        auto it = comp->overlay->session_ctrl_offsets.find(session.session_id);
        if (it != comp->overlay->session_ctrl_offsets.end()) ctrl_num += it->second;
      }
    }
    hub_controller_arrival(**session.controller_hub, ctrl_num);
    return;
  }

  // Normal flow (no hub)
  auto joypads = session.joypads->load();
  if (joypads->find(pkt.controller_number)) {
    logs::log(logs::debug,
              "[INPUT] Received CONTROLLER_ARRIVAL for controller {} which is already present; skipping...",
              pkt.controller_number);
  } else {
    create_new_joypad(session,
                      connected_client,
                      pkt.controller_number,
                      (CONTROLLER_TYPE)pkt.controller_type,
                      pkt.capabilities);
  }
}

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  // If Controller Hub is active, route through pairing table
  if (*session.controller_hub) {
    auto &hub = **session.controller_hub;

    // Apply per-session controller offset to prevent multi-device collisions.
    // Session A: 0-3, Session B: 100-103, Session C: 200-203.
    int ctrl_num = pkt.controller_number;
    if (auto comp = *session.party_compositor) {
      if (comp && comp->overlay) {
        auto it = comp->overlay->session_ctrl_offsets.find(session.session_id);
        if (it != comp->overlay->session_ctrl_offsets.end()) {
          ctrl_num += it->second;
        }
      }
    }

    // Handle controller departure (Moonlight says this pad is no longer active)
    if (!(pkt.active_gamepad_mask & (1 << pkt.controller_number))) {
      hub_controller_departure(hub, ctrl_num);
      return;
    }

    // Auto-register controller if first time seen — don't auto-pair, let notifications handle it
    {
      std::lock_guard<std::mutex> lock(hub.mtx);
      if (hub.pairing_table.count(ctrl_num) == 0 &&
          hub.unpaired_controllers.count(ctrl_num) == 0) {
        hub.unpaired_controllers.insert(ctrl_num);
        logs::log(logs::info, "[HUB] Controller {} (session {}) detected — awaiting notification pairing",
                  ctrl_num, session.session_id);
      }
      hub.last_activity[ctrl_num] = std::chrono::steady_clock::now();
    }

    // ── Overlay system: notifications + menus ──
    // Only intercept input when overlay is actively showing something.
    // Paired controllers ALWAYS fall through to input routing below.
    if (auto comp = *session.party_compositor) {
      if (comp->overlay) {
        auto &ov = *comp->overlay;

        // Read pairing state under hub mutex (needed for correct is_paired check)
        bool is_paired;
        int ctrl_slot;
        {
          std::lock_guard<std::mutex> lock(hub.mtx);
          is_paired = hub.pairing_table.count(ctrl_num) > 0;
          ctrl_slot = is_paired ? hub.pairing_table.at(ctrl_num) : -1;
        }

        // Rising-edge detection — per-hub, not static global
        std::uint16_t bf = pkt.button_flags;
        std::uint16_t prev;
        std::uint16_t stick_pressed;
        {
          std::lock_guard<std::mutex> lock(hub.mtx);
          prev = hub.last_buttons_[ctrl_num];
          hub.last_buttons_[ctrl_num] = bf;

          // Convert analog stick to digital DPAD bits (with deadzone + rising edge)
          // Allows stick to navigate menus just like DPAD
          constexpr std::int16_t STICK_THRESHOLD = 16000; // ~50% deflection
          std::uint16_t stick_bits = 0;
          if (pkt.left_stick_y < -STICK_THRESHOLD) stick_bits |= 0x0001; // UP
          if (pkt.left_stick_y >  STICK_THRESHOLD) stick_bits |= 0x0002; // DOWN
          if (pkt.left_stick_x < -STICK_THRESHOLD) stick_bits |= 0x0004; // LEFT
          if (pkt.left_stick_x >  STICK_THRESHOLD) stick_bits |= 0x0008; // RIGHT
          auto prev_stick = hub.last_stick_digital_[ctrl_num];
          hub.last_stick_digital_[ctrl_num] = stick_bits;
          stick_pressed = stick_bits & ~prev_stick; // rising edge only
        }
        std::uint16_t pressed = (bf & ~prev) | stick_pressed;

        // ── Notification input (only unpaired controllers) ──
        if (!is_paired) {
          int pair_ctrl = -1, pair_slot = -1;
          bool notif_consumed = false;
          {
            std::lock_guard<std::mutex> nlock(ov.notification_mtx);
            auto *notif = ov.find_notification(ctrl_num);
            if (notif) {
              notif->last_input = std::chrono::steady_clock::now();
              int num_slots = static_cast<int>(ov.players.size());
              if (num_slots < 1) num_slots = 4;

              if (pressed & 0x0001) { // DPAD_UP
                notif->selected_slot = notif->selected_slot <= 1 ? num_slots : notif->selected_slot - 1;
              } else if (pressed & 0x0002) { // DPAD_DOWN
                notif->selected_slot = notif->selected_slot >= num_slots ? 1 : notif->selected_slot + 1;
              } else if (pressed & 0x1000) { // A button — confirm pairing
                pair_ctrl = ctrl_num;
                pair_slot = notif->selected_slot;
                notif->confirmed = true;
                // Remove notification inline (we hold the lock, don't call dismiss_notification)
                ov.notifications.erase(
                    std::remove_if(ov.notifications.begin(), ov.notifications.end(),
                        [cn = ctrl_num](const streaming::DeviceNotification &n) {
                          return n.controller_number == cn;
                        }),
                    ov.notifications.end());
              }
              notif_consumed = true;
            }
          } // notification_mtx released here

          // Fire pair callback OUTSIDE the notification lock (on_pair acquires hub.mtx)
          if (pair_ctrl >= 0 && ov.on_pair) {
            ov.on_pair(pair_ctrl, pair_slot);
            logs::log(logs::info, "[NOTIFY] Controller {} paired → Slot {} via notification",
                      pair_ctrl, pair_slot);
          }
          if (notif_consumed) return;

          // No notification yet — create one and consume input
          ov.add_notification(ctrl_num, static_cast<int>(hub.slots.size()));
          return;
        }

        // ── Below here: only PAIRED controllers ──

        constexpr std::uint16_t OVERLAY_COMBO_1 = 0x0400 | 0x0100 | 0x0200; // HOME+LB+RB
        constexpr std::uint16_t OVERLAY_COMBO_2 = 0x0020 | 0x2000 | 0x4000 | 0x1000; // SELECT+B+X+A

        // ── Quick menu combo (either combo works) ──
        bool combo_hit = ((bf & OVERLAY_COMBO_1) == OVERLAY_COMBO_1 && (pressed & OVERLAY_COMBO_1)) ||
                         ((bf & OVERLAY_COMBO_2) == OVERLAY_COMBO_2 && (pressed & OVERLAY_COMBO_2));
        if (combo_hit) {
          ov.open_player_menu(ctrl_slot, ctrl_num);
          logs::log(logs::info, "[OVERLAY] Player Menu opened by P{}", ctrl_slot);
          return;
        }

        // ── Menu navigation (intercepts ALL controllers while menu is open) ──
        auto screen = ov.screen.load();
        if (screen != streaming::MenuScreen::NONE) {
          int num_players;
          {
            std::lock_guard<std::mutex> lock(ov.info_mtx);
            num_players = static_cast<int>(ov.players.size());
          }
          if (num_players < 1) num_players = 4;

          if (screen == streaming::MenuScreen::PLAYER) {
            // Player menu: 0=Swap, 1=Volume, 2=Reset Game, 3=Party Settings
            if (pressed & 0x0001) ov.menu_up(streaming::PLAYER_MENU_COUNT);       // DPAD_UP
            else if (pressed & 0x0002) ov.menu_down(streaming::PLAYER_MENU_COUNT); // DPAD_DOWN
            else if (pressed & 0x1000) { // A
              int idx = ov.menu_index.load();
              if (idx == 0) { // Swap → pick target
                ov.submenu_index.store(ctrl_slot == 1 ? 2 : 1);
                ov.screen.store(streaming::MenuScreen::PLAYER_SWAP_CTRL);
              } else if (idx == 2) { // Reset Game
                ov.action_param_a.store(ctrl_slot);
                ov.pending_action.store(streaming::MenuAction::RESTART_GAME);
                ov.dismiss_menu();
              } else if (idx == 3) { // Party Settings
                ov.menu_index.store(0);
                ov.screen.store(streaming::MenuScreen::PARTY);
              }
            }
            else if (pressed & 0x2000) ov.dismiss_menu(); // B
            // Volume (idx 1): left/right adjusts when selected
            else if (ov.menu_index.load() == 1) {
              if (pressed & 0x0004) { // DPAD_LEFT → volume down
                std::lock_guard<std::mutex> lock(ov.info_mtx);
                if (ctrl_slot - 1 < (int)ov.player_volumes.size()) {
                  ov.player_volumes[ctrl_slot - 1] = std::max(0.0f, ov.player_volumes[ctrl_slot - 1] - 0.1f);
                }
              } else if (pressed & 0x0008) { // DPAD_RIGHT → volume up
                std::lock_guard<std::mutex> lock(ov.info_mtx);
                if (ctrl_slot - 1 < (int)ov.player_volumes.size()) {
                  ov.player_volumes[ctrl_slot - 1] = std::min(1.0f, ov.player_volumes[ctrl_slot - 1] + 0.1f);
                }
              }
            }

          } else if (screen == streaming::MenuScreen::PLAYER_SWAP_CTRL ||
                     screen == streaming::MenuScreen::PLAYER_SWAP_POS) {
            // Submenu: pick target player to swap with
            int target = ov.submenu_index.load();
            if (pressed & 0x0001) { // DPAD_UP
              target = target <= 1 ? num_players : target - 1;
              if (target == ctrl_slot) target = target <= 1 ? num_players : target - 1; // skip self
              ov.submenu_index.store(target);
            } else if (pressed & 0x0002) { // DPAD_DOWN
              target = target >= num_players ? 1 : target + 1;
              if (target == ctrl_slot) target = target >= num_players ? 1 : target + 1; // skip self
              ov.submenu_index.store(target);
            } else if (pressed & 0x1000) { // A → confirm swap
              ov.action_param_a.store(ctrl_slot);
              ov.action_param_b.store(target);
              if (screen == streaming::MenuScreen::PLAYER_SWAP_CTRL) {
                ov.pending_action.store(streaming::MenuAction::SWAP_CONTROLLERS);
              } else {
                ov.pending_action.store(streaming::MenuAction::SWAP_POSITION);
              }
              ov.dismiss_menu();
            } else if (pressed & 0x2000) { // B → back to player menu
              ov.screen.store(streaming::MenuScreen::PLAYER);
            }

          } else if (screen == streaming::MenuScreen::PARTY) {
            // Party menu: 0=Global/Private, 1=Unpair All, 2=Restart Party, 3=End Party
            if (pressed & 0x0001) ov.menu_up(streaming::PARTY_MENU_COUNT);
            else if (pressed & 0x0002) ov.menu_down(streaming::PARTY_MENU_COUNT);
            else if (pressed & 0x1000) { // A
              int idx = ov.menu_index.load();
              if (idx == 0) { // Toggle Global/Private
                bool current = ov.party_global.load();
                ov.party_global.store(!current);
                if (ov.on_toggle_global) ov.on_toggle_global(!current);
              } else if (idx == 1) { // Unpair All
                ov.pending_action.store(streaming::MenuAction::UNPAIR_ALL);
                ov.dismiss_menu();
              } else if (idx == 3) { // End Party
                ov.pending_action.store(streaming::MenuAction::END_PARTY);
                ov.dismiss_menu();
              }
              // idx 2 (Restart Party) = not implemented yet
            }
            else if (pressed & 0x2000) ov.dismiss_menu(); // B
          }
          return; // ALL input consumed while menu is open
        }
        // Paired controller, no menu open → fall through to input routing
      }
    }

    // ── Update overlay with live input state for button visualization ──
    if (auto comp = *session.party_compositor) {
      if (comp->overlay) {
        std::lock_guard<std::mutex> lock(comp->overlay->info_mtx);
        // Find the player slot for this controller
        int slot = -1;
        {
          std::lock_guard<std::mutex> hlock(hub.mtx);
          auto it = hub.pairing_table.find(ctrl_num);
          if (it != hub.pairing_table.end()) slot = it->second;
        }
        if (slot >= 1 && slot <= (int)comp->overlay->players.size()) {
          auto &p = comp->overlay->players[slot - 1];
          p.buttons = pkt.button_flags;
          p.left_stick_x = pkt.left_stick_x;
          p.left_stick_y = pkt.left_stick_y;
          p.right_stick_x = pkt.right_stick_x;
          p.right_stick_y = pkt.right_stick_y;
          p.left_trigger = pkt.left_trigger;
          p.right_trigger = pkt.right_trigger;
        }
      }
    }

    // ── Route input to paired virtual device ──
    auto paired_device = get_paired_device(hub, ctrl_num);
    if (paired_device) {
      std::visit(
          [pkt, &session](inputtino::Joypad &pad) {
            std::uint16_t bf = pkt.button_flags;
            std::uint32_t bf2 = pkt.buttonFlags2;
            auto pressed_buttons = bf | (bf2 << 16);
            if (pressed_buttons & inputtino::Joypad::START && pressed_buttons & inputtino::Joypad::DPAD_UP &&
                pressed_buttons & inputtino::Joypad::RIGHT_BUTTON) {
              session.event_bus->fire_event(immer::box<events::ClientWolfUIComboEvent>{
                  events::ClientWolfUIComboEvent{.session_id = session.session_id}});
            }
            pad.set_pressed_buttons(pressed_buttons);
            pad.set_stick(inputtino::Joypad::LS, pkt.left_stick_x, pkt.left_stick_y);
            pad.set_stick(inputtino::Joypad::RS, pkt.right_stick_x, pkt.right_stick_y);
            pad.set_triggers(pkt.left_trigger, pkt.right_trigger);
          },
          *paired_device);
    }
    return;
  }

  // Normal flow (no hub) — unchanged from upstream
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);

    // Check if Moonlight is sending the final packet for this pad
    if (!(pkt.active_gamepad_mask & (1 << pkt.controller_number))) {
      logs::log(logs::debug, "Removing joypad {}", pkt.controller_number);
      events::UnplugDeviceEvent unplug_ev{.session_id = std::to_string(session.session_id)};
      std::visit(
          [&unplug_ev](auto &pad) {
            unplug_ev.udev_events = pad.get_udev_events();
            unplug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
          },
          *selected_pad);
      session.event_bus->fire_event(immer::box<events::UnplugDeviceEvent>(unplug_ev));
      session.joypads->update([&](events::JoypadList joypads) { return joypads.erase(pkt.controller_number); });
    }
  } else {
    selected_pad = create_new_joypad(session, connected_client, pkt.controller_number, XBOX, ANALOG_TRIGGERS | RUMBLE);
  }
  if (selected_pad) {
    std::visit(
        [pkt, session](inputtino::Joypad &pad) {
          std::uint16_t bf = pkt.button_flags;
          std::uint32_t bf2 = pkt.buttonFlags2;
          auto pressed_buttons = bf | (bf2 << 16);
          if (pressed_buttons & inputtino::Joypad::START && pressed_buttons & inputtino::Joypad::DPAD_UP &&
              pressed_buttons & inputtino::Joypad::RIGHT_BUTTON) {
            session.event_bus->fire_event(immer::box<events::ClientWolfUIComboEvent>{
                events::ClientWolfUIComboEvent{.session_id = session.session_id}});
          }
          pad.set_pressed_buttons(pressed_buttons);
          pad.set_stick(inputtino::Joypad::LS, pkt.left_stick_x, pkt.left_stick_y);
          pad.set_stick(inputtino::Joypad::RS, pkt.right_stick_x, pkt.right_stick_y);
          pad.set_triggers(pkt.left_trigger, pkt.right_trigger);
        },
        *selected_pad);
  }
}

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    auto pointer_id = boost::endian::little_to_native(pkt.pointer_id);
    switch (pkt.event_type) {
    case TOUCH_EVENT_DOWN:
    case TOUCH_EVENT_HOVER:
    case TOUCH_EVENT_MOVE: {
      if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
        std::get<PS5Joypad>(*selected_pad)
            .place_finger(pointer_id,
                          netfloat_to_0_1(pkt.x) * (uint16_t)inputtino::PS5Joypad::touchpad_width,
                          netfloat_to_0_1(pkt.y) * (uint16_t)inputtino::PS5Joypad::touchpad_height);
      }
      break;
    }
    case TOUCH_EVENT_UP:
    case TOUCH_EVENT_HOVER_LEAVE:
    case TOUCH_EVENT_CANCEL: {
      if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
        std::get<PS5Joypad>(*selected_pad).release_finger(pointer_id);
      }
      break;
    }
    case TOUCH_EVENT_CANCEL_ALL:
      logs::log(logs::warning, "Received TOUCH_EVENT_CANCEL_ALL which isn't supported");
      break;                      // TODO: remove all fingers
    case TOUCH_EVENT_BUTTON_ONLY: // TODO: ???
      logs::log(logs::warning, "Received TOUCH_EVENT_BUTTON_ONLY which isn't supported");
      break;
    }
  } else {
    logs::log(logs::warning, "Received controller touch for unknown controller {}", pkt.controller_number);
  }
}

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
      auto x = utils::from_netfloat(pkt.x);
      auto y = utils::from_netfloat(pkt.y);
      auto z = utils::from_netfloat(pkt.z);

      if (pkt.motion_type == ACCELERATION) {
        std::get<PS5Joypad>(*selected_pad).set_motion(inputtino::PS5Joypad::ACCELERATION, x, y, z);
      } else if (pkt.motion_type == GYROSCOPE) {
        std::get<PS5Joypad>(*selected_pad)
            .set_motion(inputtino::PS5Joypad::GYROSCOPE, deg2rad(x), deg2rad(y), deg2rad(z));
      }
    }
  }
}

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session) {
  auto joypads = session.joypads->load();
  std::shared_ptr<events::JoypadTypes> selected_pad;
  if (auto joypad = joypads->find(pkt.controller_number)) {
    selected_pad = std::move(*joypad);
    if (std::holds_alternative<PS5Joypad>(*selected_pad)) {
      inputtino::PS5Joypad::BATTERY_STATE state;
      switch (pkt.battery_state) {
      case BATTERY_STATE_UNKNOWN:
      case BATTERY_NOT_PRESENT:
        return; // We can't set it, let's return
      case BATTERY_DISCHARGHING:
        state = inputtino::PS5Joypad::BATTERY_DISCHARGING;
        break;
      case BATTERY_CHARGING:
        state = inputtino::PS5Joypad::BATTERY_CHARGHING;
        break;
      case BATTERY_NOT_CHARGING:
        state = inputtino::PS5Joypad::CHARGHING_ERROR;
        break;
      case BATTERY_FULL:
        state = inputtino::PS5Joypad::BATTERY_FULL;
        break;
      }
      if (pkt.battery_percentage != BATTERY_PERCENTAGE_UNKNOWN) {
        std::get<PS5Joypad>(*selected_pad).set_battery(state, pkt.battery_percentage);
      }
    }
  }
}

void handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ENetPeer>> connected_client,
                  INPUT_PKT *pkt) {
  switch (pkt->type) {
  case MOUSE_MOVE_REL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_MOVE_REL");
    auto move_pkt = static_cast<MOUSE_MOVE_REL_PACKET *>(pkt);
    mouse_move_rel(*move_pkt, session);
    break;
  }
  case MOUSE_MOVE_ABS: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_MOVE_ABS");
    auto move_pkt = static_cast<MOUSE_MOVE_ABS_PACKET *>(pkt);
    mouse_move_abs(*move_pkt, session);
    break;
  }
  case MOUSE_BUTTON_PRESS:
  case MOUSE_BUTTON_RELEASE: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_BUTTON_PACKET");
    auto btn_pkt = static_cast<MOUSE_BUTTON_PACKET *>(pkt);
    mouse_button(*btn_pkt, session);
    break;
  }
  case MOUSE_SCROLL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_SCROLL_PACKET");
    auto scroll_pkt = (static_cast<MOUSE_SCROLL_PACKET *>(pkt));
    mouse_scroll(*scroll_pkt, session);
    break;
  }
  case MOUSE_HSCROLL: {
    logs::log(logs::trace, "[INPUT] Received input of type: MOUSE_HSCROLL_PACKET");
    auto scroll_pkt = (static_cast<MOUSE_HSCROLL_PACKET *>(pkt));
    mouse_h_scroll(*scroll_pkt, session);
    break;
  }
  case KEY_PRESS:
  case KEY_RELEASE: {
    logs::log(logs::trace, "[INPUT] Received input of type: KEYBOARD_PACKET");
    auto key_pkt = static_cast<KEYBOARD_PACKET *>(pkt);
    keyboard_key(*key_pkt, session);
    break;
  }
  case UTF8_TEXT: {
    logs::log(logs::trace, "[INPUT] Received input of type: UTF8_TEXT");
    auto txt_pkt = static_cast<UTF8_TEXT_PACKET *>(pkt);
    utf8_text(*txt_pkt, session);
    break;
  }
  case TOUCH: {
    logs::log(logs::trace, "[INPUT] Received input of type: TOUCH");
    auto touch_pkt = static_cast<TOUCH_PACKET *>(pkt);
    touch(*touch_pkt, session);
    break;
  }
  case PEN: {
    logs::log(logs::trace, "[INPUT] Received input of type: PEN");
    auto pen_pkt = static_cast<PEN_PACKET *>(pkt);
    pen(*pen_pkt, session);
    break;
  }
  case CONTROLLER_ARRIVAL: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_ARRIVAL");
    auto new_controller = static_cast<CONTROLLER_ARRIVAL_PACKET *>(pkt);
    controller_arrival(*new_controller, session, connected_client);
    break;
  }
  case CONTROLLER_MULTI: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_MULTI");
    auto controller_pkt = static_cast<CONTROLLER_MULTI_PACKET *>(pkt);
    controller_multi(*controller_pkt, session, connected_client);
    break;
  }
  case CONTROLLER_TOUCH: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_TOUCH");
    auto touch_pkt = static_cast<CONTROLLER_TOUCH_PACKET *>(pkt);
    controller_touch(*touch_pkt, session);
    break;
  }
  case CONTROLLER_MOTION: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_MOTION");
    auto motion_pkt = static_cast<CONTROLLER_MOTION_PACKET *>(pkt);
    controller_motion(*motion_pkt, session);
    break;
  }
  case CONTROLLER_BATTERY: {
    logs::log(logs::trace, "[INPUT] Received input of type: CONTROLLER_BATTERY");
    auto battery_pkt = static_cast<CONTROLLER_BATTERY_PACKET *>(pkt);
    controller_battery(*battery_pkt, session);
    break;
  }
  case HAPTICS:
    logs::log(logs::trace, "[INPUT] Received input of type: HAPTICS");
    break;
  }
}
} // namespace control
