# wolf — party mode fork

> **This is a messy, AI-assisted fork of [games-on-whales/wolf](https://github.com/games-on-whales/wolf).** It adds splitscreen party mode on top of Wolf `stable` (commit `f492ab6`). It works, it's been used with real people, but the code quality reflects "get it working" not "get it right." Uploaded for reference so the Wolf maintainers can see the approach.

[![Discord](https://img.shields.io/discord/856434175455133727.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2)](https://discord.gg/kRGUDHNHt2)
[![GitHub license](https://img.shields.io/github/license/games-on-whales/wolf)](https://github.com/games-on-whales/wolf/blob/main/LICENSE)

---

## What this fork adds

Everything lives on the `party-mode` branch. 14 changed files, ~4,000 lines added. Three features:

### 1. Session Splitscreen

Multiple Wolf sessions (each running their own game in their own container) get composited into one video stream. One TV, 2-4 games side by side.

- GStreamer `cudacompositor` tiles 2-4 session video feeds into a single 1080p output
- Layouts: 2-player horizontal, 2-player vertical, 3-player, 4-player grid
- Audio from all sessions mixed together with spatial panning (left player's audio comes from the left)
- "Adaptive" mode fills the screen based on layout, "retro" mode does 4:3 tiles

**How it works:** Each Wolf session already outputs video to a GStreamer interpipesink. The compositor reads from those interpipesinks, scales each to a tile, positions them in a grid, and outputs to a new interpipesink. The Moonlight client's encoder switches from the single-session source to the compositor output. When the party ends, it switches back.

**Files:** `party_compositor.cpp/hpp`

### 2. Controller Hub

Virtual controller slots that sit between physical controllers and game sessions. Solves the problem of "who controls what" when you have 4 controllers and 4 game sessions.

- Persistent virtual slots — games see stable controllers that never disconnect
- Pair any physical controller to any session via the overlay or API
- Swap controllers between sessions without unplugging
- Session-scoped numbering (session A gets controllers 0-3, session B gets 100-103) so they don't collide

**How it works:** Physical controllers from Moonlight get an offset added to their ID based on which session they came from. The hub maintains a pairing table mapping physical controller IDs to virtual slot devices. Input goes: Moonlight controller -> offset -> hub lookup -> virtual device -> game container.

**Files:** `controller_hub.cpp/hpp`, changes to `input_handler.cpp`

### 3. In-Stream Overlay

A menu system rendered directly into the video stream so you can manage the party without leaving the game.

- Open with HOME+LB+RB (controller) or F1 (keyboard)
- Per-player menu: swap controller, adjust volume, reset game
- Party menu: switch global/private mode, end party
- Controller pairing notifications when new controllers connect
- System health warnings (GPU temp, encoder load)

**How it works:** Cairo renders menu frames at 15fps on CPU into BGRA buffers. These get pushed into the GStreamer compositor pipeline via `appsrc`, uploaded to GPU, and alpha-blended on top of the game tiles at 60fps.

**Files:** `session_overlay.cpp/hpp`

### Other changes

- **Session lifecycle** (`moonlight.cpp`): New connections during an active party get routed through an interpipe bridge so they receive compositor frames immediately. Moonlight "pause" events get upgraded to full disconnects during party mode to force clean reconnection through the bridge path.
- **API endpoints** (`endpoints.cpp`, `unix_socket_server.cpp`, `api.hpp`): REST endpoints for party spawn/stop, controller hub create/pair/unpair/swap, layout selection, party status.
- **Dockerfile** (`wolf.Dockerfile`): Added `libcairo2-dev` for the overlay. Bumped Rust to 1.92.0 and unpinned cargo-c.
- **Config** (`config.include.toml`, `config.v6.toml`): Minor interpipe buffer settings.

## What doesn't work

- **Steam.** Steam sessions always crash with a "steam web helper" error. Probably related to the resolution stuff. RetroArch works great.
- **Zero-copy GPU compositing.** The compositor downloads from CUDA to CPU and back because GStreamer's interpipe doesn't transfer CUDA memory context across pipeline boundaries. Adds ~5-10ms latency. I couldn't get it working — the CUDA contexts from different pipelines don't share properly.
- **Overlay latency.** The Cairo overlay adds compositing overhead. A proper implementation built into Wolf-UI would be way better.
- **Reset Game** only stops the lobby, doesn't restart it.
- **DualSense over Bluetooth** keeps re-pairing because the container can't create PS5 virtual joypads (uinput permissions).

## Hardware

Tested on: i7-14700K, RTX 3060 12GB, 64GB DDR4, Unraid. 4-player splitscreen runs at ~18% GPU utilization.

---

*Everything below is from the original Wolf README.*

---

> An intelligent wolf is better than a foolish lion.
>
> &mdash; <cite>Matshona Dhliwayo.</cite>

Wolf is a streaming server for [Moonlight](https://moonlight-stream.org/) that allows you to share a single server with
multiple remote clients in order to play videogames!

![Wolf basic flow chart](https://github.com/games-on-whales/wolf/blob/stable/docs/modules/ROOT/images/wolf-introduction.svg?raw=true)

It's made from the ground up with the following primary goals:

- Allow multiple users to stream different content by sharing a single remote host hardware
- On demand creation of virtual desktops with full support for any resolution/FPS without the need for a monitor or a
  dummy plug.
- Allow multiple GPUs to be used simultaneously for different jobs
    - Example: stream encoding on iGPU whilst gaming on GPU
- Provide low latency video and audio stream with full support for gamepads
- Linux and Docker first: run your games with low privileges in containers (based
  on [Games On Whales](https://github.com/games-on-whales/gow))
- Mostly hackable, just edit the config file to modify encoding pipelines, GPU settings or Docker/Podman low level
  details

It's a specific tool for a specific need, are you looking for a general purpose streaming solution?
Try out [Sunshine](https://github.com/LizardByte/Sunshine)!

Want to give it a spin? [Checkout our docs](https://games-on-whales.github.io/wolf/stable/)!

[![Youtube video preview](https://github.com/games-on-whales/wolf/blob/stable/docs/modules/ROOT/images/introduction-video.png?raw=true)](https://www.youtube.com/watch?v=z5jzLIUH6rA)

## Acknowledgements

- [@Drakulix](https://github.com/Drakulix) for the incredible help given in developing Wolf
- [@zb140](https://github.com/zb140) for the constant help and support in [GOW](https://github.com/games-on-whales/gow)
- [@loki-47-6F-64](https://github.com/loki-47-6F-64) for creating and
  sharing [Sunshine](https://github.com/loki-47-6F-64/sunshine)
- [@ReenigneArcher](https://github.com/ReenigneArcher) for being the first stargazer of the project and taking care of
  keeping [Sunshine alive](https://github.com/LizardByte/Sunshine)
- All the guys at the [Moonlight](https://moonlight-stream.org/) Discord channel, for the tireless help they provide to
  anyone
