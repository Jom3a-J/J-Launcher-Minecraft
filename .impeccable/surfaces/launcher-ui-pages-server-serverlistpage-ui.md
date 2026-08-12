---
version: 1
slug: "launcher-ui-pages-server-serverlistpage-ui"
primary_target: "launcher/ui/pages/server/ServerListPage.ui"
related_targets: ["launcher/ui/pages/server/ServerListPage.cpp","launcher/ui/MainWindow.cpp"]
---

# Server Manager Surface

- Scope: the modeless Server Manager window, including its shell and Home, Console, Mods or Plugins, Players, Files, Backups, Tools, and Settings workspaces.
- Visitor mode: Operate.
- Audience and job: J Launcher users creating and administering local Minecraft servers; understand state, take the safe next action, and reach advanced tools without losing server context.
- Primary actions: select, create, start, stop, restart, monitor, configure, update, back up, and maintain servers.
- Constraints: preserve all current behavior and data; remain native Qt; support launcher light and dark palettes, keyboard access, modeless operation, blue theme icons, and saved user themes.
- Direction: Server Operations Workspace with a searchable server rail, persistent selected-server command bar, vertical tool navigation, and focused content canvas.
- Memorable moment: server state and the safest available process action remain visible while the user moves across every administration tool.
- Unresolved decisions: none.
