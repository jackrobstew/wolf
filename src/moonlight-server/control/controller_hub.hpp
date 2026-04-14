#pragma once

#include <chrono>
#include <events/events.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wolf::core::events {
// Define the forward-declared ControllerHubState from events.hpp

struct ControllerSlot {
  int slot_number; // 1-based (Slot 1, Slot 2, etc.)
  std::shared_ptr<JoypadTypes> virtual_device;

  // Which moonlight controller is currently paired to this slot
  std::optional<int> paired_controller_number;

  // Which session's container should receive input from this slot
  std::string target_session_id;

  // Is a physical controller currently paired and connected?
  bool physical_connected = false;

  // Cached udev metadata for re-firing PlugDeviceEvents to new targets
  std::vector<std::map<std::string, std::string>> udev_events;
  std::vector<std::pair<std::string, std::vector<std::string>>> udev_hw_db_entries;
};

/**
 * ControllerHubState — the actual type behind StreamSession::controller_hub.
 * Forward-declared in events.hpp, defined here.
 *
 * Manages virtual controller slots that sit between Moonlight's physical controllers
 * and game containers. Provides stable virtual devices that never appear/disappear
 * from the game's perspective.
 *
 * Thread safety: all reads/writes to mutable state go through the mutex.
 * The control thread (high frequency input) and API thread (rare pairing changes)
 * both access this struct.
 */
struct ControllerHubState {
  mutable std::mutex mtx;
  std::vector<ControllerSlot> slots;

  // Reverse lookup: moonlight controller_number → slot_number (1-based)
  std::map<int, int> pairing_table;

  // Set of moonlight controller numbers that are connected but not paired
  std::set<int> unpaired_controllers;

  // Last input activity time per controller — for UI "which controller is this?" indicator
  std::map<int, std::chrono::steady_clock::time_point> last_activity;

  // Per-controller previous button state for rising-edge detection (protected by mtx)
  std::map<int, std::uint16_t> last_buttons_;
  // Per-controller previous stick digital state for overlay navigation (protected by mtx)
  std::map<int, std::uint16_t> last_stick_digital_;
};

} // namespace wolf::core::events

namespace control {

using ControllerHub = wolf::core::events::ControllerHubState;

/**
 * Create a new Controller Hub with N virtual Xbox controller slots.
 * Slots are permanent uinput devices that never get destroyed.
 * Returns shared_ptr (not optional) because ControllerHub has a mutex (non-movable).
 *
 * @param device_queue If provided, PlugDeviceEvents are pushed directly to this queue
 *                     (bypassing the event bus). This is needed when targeting a lobby's
 *                     container, since the event bus handler won't match lobby IDs.
 */
std::shared_ptr<ControllerHub> create_hub(int num_slots,
                                           const std::string &default_target_session_id,
                                           const std::shared_ptr<wolf::core::events::EventBusType> &event_bus,
                                           std::shared_ptr<wolf::core::events::devices_atom_queue> device_queue = nullptr);

/**
 * Create a party-mode hub: one slot per lobby, each slot's virtual joypad
 * plugged into its respective lobby container via that lobby's device queue.
 */
std::shared_ptr<ControllerHub> create_party_hub(
    const std::vector<std::string> &lobby_ids,
    const std::vector<std::shared_ptr<wolf::core::events::devices_atom_queue>> &device_queues,
    const std::shared_ptr<wolf::core::events::EventBusType> &event_bus);

/**
 * Pair a physical Moonlight controller to a virtual slot.
 * Thread-safe: acquires hub mutex.
 */
bool pair_controller(ControllerHub &hub, int controller_number, int slot_number);

/**
 * Unpair a slot, removing its physical controller mapping.
 * The virtual device stays — just stops receiving input.
 * Thread-safe: acquires hub mutex.
 */
void unpair_slot(ControllerHub &hub, int slot_number);

/**
 * Route a slot to a different target session.
 * Re-fires PlugDeviceEvent with the new session_id.
 * Thread-safe: acquires hub mutex.
 */
bool route_slot(ControllerHub &hub,
                int slot_number,
                const std::string &target_session_id,
                const std::shared_ptr<wolf::core::events::EventBusType> &event_bus);

/**
 * Get the virtual device for a given Moonlight controller number.
 * Returns nullptr if the controller is not paired.
 * Thread-safe: acquires hub mutex.
 */
std::shared_ptr<wolf::core::events::JoypadTypes> get_paired_device(ControllerHub &hub, int controller_number);

/**
 * Handle a controller arrival in hub mode.
 * Thread-safe: acquires hub mutex.
 */
void hub_controller_arrival(ControllerHub &hub, int controller_number);

/**
 * Handle a controller departure in hub mode.
 * Thread-safe: acquires hub mutex.
 */
void hub_controller_departure(ControllerHub &hub, int controller_number);

/**
 * Atomically swap two slots' controller pairings.
 * Virtual devices stay where they are — only the pairing table changes.
 * Thread-safe: acquires hub mutex.
 */
void swap_slots(ControllerHub &hub, int slot_a, int slot_b);

} // namespace control
