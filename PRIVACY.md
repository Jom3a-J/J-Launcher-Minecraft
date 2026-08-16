# J Launcher Privacy Policy

**Status:** Phase 9 release-candidate draft, reviewed against the source on
14 August 2026. The exact tagged binary and its runtime logs must be audited
again before stable publication.

## In short

J Launcher runs locally. It operates no account service, analytics service,
advertising network, telemetry collector, or automatic crash reporter. It
contacts third-party services to sign in, download Minecraft and Java, browse
content, create servers, and perform actions the user requests.

Every network connection reveals the user's IP address and basic connection
metadata to the service receiving it. J Launcher identifies normal requests
with a `JLauncher/<version>` user agent.

## Data stored on the computer

| Data | Typical location | Notes |
| --- | --- | --- |
| Microsoft/Minecraft tokens and account profile | `accounts.json` | Tokens are encrypted on Windows as described below |
| Launcher settings | `jlauncher.cfg` | Local configuration |
| Instances, worlds, saves, screenshots, and content | `instances/` | User-owned game data |
| Local servers and backups | `servers/` | User-owned server data |
| Launcher logs | `logs/` | Retained locally for diagnostics |
| Game assets, libraries, Java runtimes, and metadata | launcher data directories | Download cache and runtime data |
| A user-entered CurseForge key | Windows Credential Manager | Not written to the settings file |

Portable mode keeps this data under the portable directory. An installed build
uses the current Windows user's application-data location.

### Account-token protection on Windows

Microsoft and Minecraft tokens are encrypted before `accounts.json` is saved,
using authenticated XChaCha20-Poly1305 encryption. The encryption key is stored
separately in Windows Credential Manager for the current Windows user. If the
credential entry is unavailable or malformed, J Launcher preserves existing
encrypted data and does not silently replace it with plaintext.

This protects a copied launcher folder, backup, or drive from exposing usable
tokens without the Windows credential. It does not protect against malicious
software already running as the same Windows user, which can access that user's
credential store.

Phase 9 releases Windows x64 only. The inherited non-Windows storage path is not
qualified or distributed by this release plan.

## Automatic network requests

Normal instance creation or launch may request Minecraft/version/loader
metadata from J Launcher's GitHub Pages metadata service, game files from
Mojang/Microsoft hosts, and Java runtimes from vendor URLs supplied by that
metadata. These are download requests; J Launcher does not attach analytics,
instance contents, or usage statistics.

Fresh installations use bundled or cached translations. The launcher contacts
Prism Launcher's translation host only after the user requests a language-list
refresh or explicitly enables automatic translation checks.

## Microsoft and Minecraft sign-in

When the user adds or refreshes an account, J Launcher contacts Microsoft
identity, Xbox Live, and Minecraft services. The Microsoft password is entered
in Microsoft's browser/device-code flow and is never received by J Launcher.

J Launcher uses its own Microsoft application registration. Authentication is
completed by a local loopback handler. The browser then opens J Launcher's
GitHub Pages completion page, which receives no authorization code, token,
account identifier, or other sign-in data. Authentication remains completed if
that presentation-only page cannot load.

## User-triggered network requests

- Browsing or downloading content contacts the selected provider: Modrinth,
  CurseForge when configured, ATLauncher, Feed The Beast, or Technic.
- Creating or updating a local server may contact Mojang, PaperMC, PurpurMC,
  FabricMC, MinecraftForge, or NeoForged.
- Forge 1.5.2 and older may download retained legacy libraries from
  `files.prismlauncher.org`.
- Pressing **Upload** on a log publishes it to the selected paste service. Logs
  can contain a Minecraft name, personal filesystem paths, installed content,
  and server addresses; review and redact them first.

J Launcher does not upload a log by itself.

## Features deliberately absent

- No advertising, analytics, or telemetry.
- No automatic crash reporting.
- No automatic launcher updates until their trust and recovery gates receive a
  separate approval.
- No news service.
- No screenshot upload integration.

## Third parties and changes

Third-party services apply their own terms and privacy policies to requests
they receive. [INTEGRATIONS.md](INTEGRATIONS.md) lists the enabled and optional
services. Material changes to what leaves the computer must be reviewed here
and called out in the applicable release notes.

Privacy questions may be opened in the public issue tracker. Potential privacy
or security vulnerabilities must be reported privately under
[SECURITY.md](SECURITY.md).
