#include "controller_hub.hpp"

#include <fmt/format.h>
#include <helpers/logger.hpp>
#include <immer/box.hpp>
#include <platforms/input.hpp>

namespace control {

using namespace wolf::core::input;
using namespace wolf::core::events;

std::shared_ptr<ControllerHub> create_hub(int num_slots,
                                           const std::string &default_target_session_id,
                                           const std::shared_ptr<EventBusType> &event_bus,
                                           std::shared_ptr<devices_atom_queue> device_queue) {
  auto hub = std::make_shared<ControllerHub>();

  for (int i = 0; i < num_slots; i++) {
    int slot_num = i + 1;

    logs::log(logs::info, "[HUB] Creating virtual controller Slot {} (pad {})", slot_num, i);

    auto result = XboxOneJoypad::create({.name = fmt::format("Wolf X-Box One (virtual) pad {}", i),
                                          .vendor_id = 0x045E,
                                          .product_id = 0x02EA,
                                          .version = 0x0408});
    if (!result) {
      logs::log(logs::error, "[HUB] Failed to create virtual controller for Slot {}: {}", slot_num,
                result.getErrorMessage());
      return nullptr;
    }

    auto device = std::make_shared<JoypadTypes>(std::move(*result));

    // No-op rumble — hub joypads don't route rumble to physical controllers (yet)
    std::visit([](auto &pad) {
      pad.set_on_rumble([](int, int) {});
    }, *device);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ControllerSlot slot;
    slot.slot_number = slot_num;
    slot.virtual_device = device;
    slot.target_session_id = default_target_session_id;
    slot.physical_connected = false;

    std::visit(
        [&slot](auto &pad) {
          slot.udev_events = pad.get_udev_events();
          slot.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *device);

    PlugDeviceEvent plug_ev{.session_id = default_target_session_id,
                            .udev_events = slot.udev_events,
                            .udev_hw_db_entries = slot.udev_hw_db_entries};
    if (device_queue) {
      // Push directly to the lobby's device queue (bypasses event bus routing)
      device_queue->push(immer::box<PlugDeviceEvent>(plug_ev));
    } else {
      event_bus->fire_event(immer::box<PlugDeviceEvent>(plug_ev));
    }

    logs::log(logs::info, "[HUB] Slot {} created → session {}", slot_num, default_target_session_id);
    hub->slots.push_back(std::move(slot));
  }

  logs::log(logs::info, "[HUB] Controller Hub ready ({} slots)", num_slots);
  return hub;
}

std::shared_ptr<ControllerHub> create_party_hub(
    const std::vector<std::string> &lobby_ids,
    const std::vector<std::shared_ptr<devices_atom_queue>> &device_queues,
    const std::shared_ptr<EventBusType> &event_bus) {

  auto hub = std::make_shared<ControllerHub>();
  int num_slots = static_cast<int>(lobby_ids.size());

  // Track how many devices have been created per lobby for sequential naming
  std::map<std::string, int> lobby_device_count;

  for (int i = 0; i < num_slots; i++) {
    int slot_num = i + 1;
    int device_num = lobby_device_count[lobby_ids[i]]++;

    logs::log(logs::info, "[HUB] Creating party slot {} → lobby {} (pad {})", slot_num, lobby_ids[i], device_num);

    auto result = XboxOneJoypad::create({.name = fmt::format("Wolf X-Box One (virtual) pad {}", device_num),
                                          .vendor_id = 0x045E,
                                          .product_id = 0x02EA,
                                          .version = 0x0408});
    if (!result) {
      logs::log(logs::error, "[HUB] Failed to create party slot {}: {}", slot_num, result.getErrorMessage());
      return nullptr;
    }

    auto device = std::make_shared<JoypadTypes>(std::move(*result));

    // Set a no-op rumble callback — without this, RetroArch's "welcome buzz" on
    // controller detect can crash the control stream (null rumble handler).
    std::visit(
        [](auto &pad) {
          pad.set_on_rumble([](int, int) { /* swallow rumble for hub joypads */ });
        },
        *device);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ControllerSlot slot;
    slot.slot_number = slot_num;
    slot.virtual_device = device;
    slot.target_session_id = lobby_ids[i];
    slot.physical_connected = false;

    std::visit(
        [&slot](auto &pad) {
          slot.udev_events = pad.get_udev_events();
          slot.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *device);

    PlugDeviceEvent plug_ev{.session_id = lobby_ids[i],
                            .udev_events = slot.udev_events,
                            .udev_hw_db_entries = slot.udev_hw_db_entries};

    if (i < static_cast<int>(device_queues.size()) && device_queues[i]) {
      device_queues[i]->push(immer::box<PlugDeviceEvent>(plug_ev));
    } else {
      event_bus->fire_event(immer::box<PlugDeviceEvent>(plug_ev));
    }

    logs::log(logs::info, "[HUB] Party Slot {} created → lobby {}", slot_num, lobby_ids[i]);
    hub->slots.push_back(std::move(slot));
  }

  logs::log(logs::info, "[HUB] Party Hub ready ({} slots)", num_slots);
  return hub;
}

bool pair_controller(ControllerHub &hub, int controller_number, int slot_number) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (slot_number < 1 || slot_number > (int)hub.slots.size()) {
    logs::log(logs::warning, "[HUB] Invalid slot {}", slot_number);
    return false;
  }

  auto &slot = hub.slots[slot_number - 1];

  // Unpair old mapping for this controller
  if (hub.pairing_table.count(controller_number)) {
    int old_slot = hub.pairing_table[controller_number];
    if (old_slot != slot_number) {
      auto &old = hub.slots[old_slot - 1];
      old.paired_controller_number = std::nullopt;
      old.physical_connected = false;
    }
  }

  // Unpair old controller from this slot
  if (slot.paired_controller_number.has_value()) {
    int old_ctrl = slot.paired_controller_number.value();
    hub.pairing_table.erase(old_ctrl);
    hub.unpaired_controllers.insert(old_ctrl);
  }

  slot.paired_controller_number = controller_number;
  slot.physical_connected = true;
  hub.pairing_table[controller_number] = slot_number;
  hub.unpaired_controllers.erase(controller_number);

  logs::log(logs::info, "[HUB] Paired controller {} → Slot {}", controller_number, slot_number);
  return true;
}

void unpair_slot(ControllerHub &hub, int slot_number) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (slot_number < 1 || slot_number > (int)hub.slots.size())
    return;

  auto &slot = hub.slots[slot_number - 1];
  if (slot.paired_controller_number.has_value()) {
    int ctrl = slot.paired_controller_number.value();
    hub.pairing_table.erase(ctrl);
    hub.unpaired_controllers.insert(ctrl);
    slot.paired_controller_number = std::nullopt;
    slot.physical_connected = false;
    logs::log(logs::info, "[HUB] Unpaired Slot {}", slot_number);
  }
}

bool route_slot(ControllerHub &hub,
                int slot_number,
                const std::string &target_session_id,
                const std::shared_ptr<EventBusType> &event_bus) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (slot_number < 1 || slot_number > (int)hub.slots.size())
    return false;

  auto &slot = hub.slots[slot_number - 1];
  std::string old_target = slot.target_session_id;

  // Unplug from old target
  event_bus->fire_event(immer::box<UnplugDeviceEvent>(
      UnplugDeviceEvent{.session_id = old_target,
                        .udev_events = slot.udev_events,
                        .udev_hw_db_entries = slot.udev_hw_db_entries}));

  // Plug into new target
  slot.target_session_id = target_session_id;
  event_bus->fire_event(immer::box<PlugDeviceEvent>(
      PlugDeviceEvent{.session_id = target_session_id,
                      .udev_events = slot.udev_events,
                      .udev_hw_db_entries = slot.udev_hw_db_entries}));

  logs::log(logs::info, "[HUB] Routed Slot {} → session {}", slot_number, target_session_id);
  return true;
}

std::shared_ptr<JoypadTypes> get_paired_device(ControllerHub &hub, int controller_number) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  auto it = hub.pairing_table.find(controller_number);
  if (it == hub.pairing_table.end())
    return nullptr;
  int idx = it->second - 1;
  if (idx < 0 || idx >= (int)hub.slots.size())
    return nullptr;
  return hub.slots[idx].virtual_device;
}

void hub_controller_arrival(ControllerHub &hub, int controller_number) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (hub.pairing_table.count(controller_number)) {
    // Existing pairing from party persistence — re-pair silently
    int slot_num = hub.pairing_table[controller_number];
    hub.slots[slot_num - 1].physical_connected = true;
    logs::log(logs::info, "[HUB] Controller {} reconnected → Slot {} (existing pairing)", controller_number, slot_num);
  } else {
    // New controller — add to unpaired, let notification system handle it
    hub.unpaired_controllers.insert(controller_number);
    logs::log(logs::info, "[HUB] Controller {} arrived — awaiting notification pairing", controller_number);
  }
}

void swap_slots(ControllerHub &hub, int slot_a, int slot_b) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (slot_a < 1 || slot_a > (int)hub.slots.size() ||
      slot_b < 1 || slot_b > (int)hub.slots.size() || slot_a == slot_b) {
    logs::log(logs::warning, "[HUB] Invalid swap: {} ↔ {}", slot_a, slot_b);
    return;
  }

  auto &a = hub.slots[slot_a - 1];
  auto &b = hub.slots[slot_b - 1];

  // Swap controller assignments
  std::swap(a.paired_controller_number, b.paired_controller_number);
  std::swap(a.physical_connected, b.physical_connected);

  // Update reverse lookup
  if (a.paired_controller_number.has_value())
    hub.pairing_table[a.paired_controller_number.value()] = slot_a;
  if (b.paired_controller_number.has_value())
    hub.pairing_table[b.paired_controller_number.value()] = slot_b;

  logs::log(logs::info, "[HUB] Swapped Slot {} ↔ Slot {}", slot_a, slot_b);
}

void hub_controller_departure(ControllerHub &hub, int controller_number) {
  std::lock_guard<std::mutex> lock(hub.mtx);

  if (hub.pairing_table.count(controller_number)) {
    int slot_num = hub.pairing_table[controller_number];
    hub.slots[slot_num - 1].physical_connected = false;
    logs::log(logs::info, "[HUB] Controller {} disconnected (Slot {} persists)", controller_number, slot_num);
  }
  hub.unpaired_controllers.erase(controller_number);
}

} // namespace control
