# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

J Launcher users who create and operate local Minecraft servers from the desktop launcher. The Server Manager supports both quick daily actions and deeper administration without requiring a separate server tool.

## Product Purpose

J Launcher provides one desktop application for launching Minecraft and managing local Minecraft servers. The Server Manager lets users create, start, stop, monitor, configure, update, back up, and maintain servers while keeping the main launcher available.

## Positioning

Server administration is integrated directly into the Minecraft launcher and shares its instances, accounts, content discovery, files, runtime configuration, and visual themes.

## Operating Context

The Server Manager is a modeless desktop window opened from the launcher's Servers action. Users move between server status, console, mods or plugins, players, files, backups, maintenance tools, and settings. Server processes continue running when the Server Manager window closes.

## Capabilities and Constraints

- Preserve all existing server-management behavior and data.
- Preserve the current Home, Console, Mods, Players, Files, Backups, Tools, and Settings capabilities.
- Preserve keyboard access, native Qt interaction behavior, modeless window behavior, and the existing server process lifecycle.
- Support the launcher's light and dark application themes rather than imposing a fixed appearance.
- Keep the implementation in the existing Qt/C++ widget system.

## Brand Commitments

- Product name: J Launcher.
- The existing J Launcher logo and blue identity are binding.
- The Server Manager should feel modern and premium while remaining recognizably part of the launcher.
- The blue icon theme is the default for first-run users; other saved user theme choices remain supported.

## Evidence on Hand

- J Launcher logo and application icons under `program_info/`.
- Built-in blue icon theme under `launcher/resources/pe_blue/`.
- Existing launcher themes and palette behavior under `launcher/ui/themes/`.
- Existing Server Manager implementation under `launcher/ui/pages/server/` and `launcher/ui/dialogs/CreateServerDialog.*`.
- No external customer claims, benchmarks, or marketing evidence are required for this operational surface.

## Product Principles

- Make the server's current state and safest next action immediately clear.
- Keep routine controls fast while making advanced tools easy to find.
- Preserve user control and communicate destructive or process-changing actions clearly.
- Feel native to J Launcher across light and dark themes.
- Prefer durable, accessible desktop interaction over decorative novelty.

## Accessibility & Inclusion

Retain keyboard navigation, visible focus, readable contrast, non-color status cues, scalable native text, and clear disabled, loading, empty, warning, and error states.
