# J Launcher Integrations

**Status:** Phase 9 release-candidate inventory, reviewed against source on
14 August 2026. The exact tagged package must be checked again before stable
publication.

For the data involved in these connections, see [PRIVACY.md](PRIVACY.md).

## Identity and game services

| Service | Purpose | Candidate state |
| --- | --- | --- |
| [Microsoft identity platform](https://learn.microsoft.com/entra/identity-platform/) | Microsoft account sign-in | Enabled with J Launcher's application registration |
| [Xbox Live](https://www.xbox.com/legal/privacy-policy) | Xbox authentication exchange | Enabled |
| [Minecraft services](https://www.minecraft.net/terms) | Ownership, profile, skins, capes, game metadata and downloads | Enabled |
| J Launcher GitHub Pages metadata | Minecraft, library, Java, and loader metadata | Enabled and J Launcher-owned |
| J Launcher GitHub Pages login completion | Presentation after local authentication | Enabled; receives no authentication data |
| Prism legacy FML library host | Forge 1.5.2 and older libraries | Retained compatibility dependency |

## Content providers

| Service | Purpose | Candidate state |
| --- | --- | --- |
| [Modrinth](https://modrinth.com/legal/privacy) | Mods, modpacks, resource packs, and shaders | Enabled |
| [CurseForge](https://www.curseforge.com/legal/privacy) | Mods and modpacks | Hidden unless a key is configured |
| [ATLauncher](https://atlauncher.com/privacy) | Modpack browsing and installation | Enabled |
| [Feed The Beast](https://www.feed-the-beast.com/privacy) | Modpack browsing, installation, and import | Enabled |
| [Technic](https://www.technicpack.net/privacy) | Modpack browsing and installation | Enabled |

A release build may receive J Launcher's reviewed CurseForge key only through
protected build configuration. The key must never be committed, copied into
evidence, or printed in logs. Without a key the CurseForge UI stays unavailable.
A user-entered key is stored in Windows Credential Manager.

## Server software

Server creation and updates contact only the provider selected by the user:

- Mojang for Vanilla
- PaperMC for Paper
- PurpurMC for Purpur
- FabricMC for Fabric
- MinecraftForge for Forge
- NeoForged for NeoForge

## Optional user actions

| Service | Purpose | State |
| --- | --- | --- |
| `mclo.gs` and alternative configured paste hosts | Log upload | Only after the user presses **Upload** |
| Prism translation host/Weblate | Translation list and files | Manual refresh, or automatic only after opt-in |
| Imgur | Screenshot upload | Disabled; no client ID configured |

## Deliberately absent

J Launcher integrates no advertising, analytics, telemetry, automatic crash
reporting, news feed, screenshot host, or automatic-update service. None may be
added without separate technical, security, privacy, legal, and release review.

Every service above is operated by a third party except J Launcher's public
GitHub repository and GitHub Pages sites. Their published policies and terms
govern requests they receive. Report integration security issues privately
under [SECURITY.md](SECURITY.md).
