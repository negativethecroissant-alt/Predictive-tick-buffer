# Tick Buffer Auto-Correct (240 TPS)

Automatically buffers early clicks by **1 to 16 ticks** on **240 TPS** until a tick where the click passes and your player survives the next **480 ticks** (2.00 seconds at 240 TPS).

## Features
- **1-16 Tick Auto-Buffer**: Choose any maximum early-click buffer window from `1` to `16` ticks (`4.17 ms` to `66.67 ms` at 240 TPS).
- **480-Tick Lookahead Survival Check**: Evaluates candidate click ticks at `dt = 1/240s` across the next `480` ticks against hazards, spikes, solid blocks, slopes, orbs, pads, and portals.
- **In-Game Mobile UI**: Open the **Pause Menu** during any level and tap the **`1-16t`** button to pick from `1` to `16` ticks directly or toggle the live HUD overlay.
- **Short-Tap Release Protection**: If you tap and release your finger before the delayed tick arrives, the mod queues your release so the buffered click still holds for a clean jump.
