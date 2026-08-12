---
name: "J Launcher Fleet Operations Workspace"
description: "A native Qt server-operations system with palette-adaptive surfaces, blue icon identity, and compact command context."
colors:
  status-running: "#43a047"
  status-transition: "#f9a825"
  status-error: "#e53935"
  status-stopped: "#607d8b"
  control-disabled-text: "#7d7d7d"
  danger-text-dark: "#ff8a80"
  danger-text-light: "#b3261e"
typography:
  display:
    fontFamily: "Qt application font"
    fontSize: "18pt"
    fontWeight: 700
  headline:
    fontFamily: "Qt application font"
    fontSize: "14pt"
    fontWeight: 700
  title:
    fontFamily: "Qt application font"
    fontSize: "12pt"
    fontWeight: 700
  body:
    fontFamily: "Qt application font"
    fontWeight: 400
  label:
    fontFamily: "Qt application font"
    fontWeight: 600
  console:
    fontFamily: "Courier New"
    fontSize: "9pt"
    fontWeight: 400
rounded:
  control: "8px"
  card: "10px"
  panel: "12px"
spacing:
  compact: "8px"
  control: "10px"
  panel: "12px"
  section: "14px"
  canvas: "18px"
  header: "24px"
---

# Design System: J Launcher Fleet Operations Workspace

## Overview

**Creative North Star: "Fleet Operations Workspace"**

The approved direction (seed aa8e1623) treats server management as a focused native operations surface: fleet state stays scannable at the left, the selected server and safest process action remain visible above the workspace, and administration tools switch without erasing that command context. It is compact, direct, and premium through precision rather than ornament.

The system belongs to J Launcher through native Qt palette behavior and the launcher's blue icon identity. Flat tonal layers, restrained separators, compact system typography, and small rounded controls keep both light and dark appearances readable without imposing a fixed theme.

**Key Characteristics:**

- Native Qt palette semantics across launcher themes.
- Launcher-palette primary actions and active navigation.
- Blue launcher icons as the persistent identity signal.
- Flat tonal layering with restrained one-pixel separators.
- Compact operational typography and dense, keyboard-accessible controls.
- Persistent selected-server context and text-backed status communication.

## Colors

The palette is semantic first: launcher and operating-system roles provide surfaces, text, borders, selection, and primary emphasis; fixed colors are reserved for operational state and danger text.

### Primary

- **Launcher Highlight:** `QPalette::Highlight` fills the current primary action and active tool destination; `QPalette::HighlightedText` supplies its readable foreground. The active launcher palette, not this surface, owns the actual hue.

### Secondary

- **Running Green:** Marks a running server and normal live-resource state, always beside a visible state label.
- **Transition Amber:** Marks starting, stopping, downloading, and approaching-threshold states, always beside explanatory text.
- **Error Red:** Marks server errors, warnings, and failed health states, always with a written diagnosis or warning.
- **Stopped Blue Gray:** Marks stopped or unavailable states, always paired with the state name.
- **Adaptive Danger Text:** Destructive button text uses the dedicated light-appearance or dark-appearance token while retaining the native button surface.

### Neutral

- **Window Canvas:** `QPalette::Window` is the page and detail-canvas layer.
- **Base Surface:** `QPalette::Base` is the header, command bar, workspace, fields, tables, and hover or selected-card surface.
- **Alternate Rail:** `QPalette::AlternateBase` separates the server and tool rails without adding elevation.
- **Primary Text:** `QPalette::Text`, `QPalette::WindowText`, and `QPalette::ButtonText` preserve native foreground contrast for their control contexts.
- **Muted Text:** Secondary copy is calculated by blending two parts foreground with one part parent background, so it remains theme-aware.
- **Quiet Divider:** `QPalette::Midlight` supplies one-pixel borders and separators; `QPalette::Mid` supplies disabled navigation and unavailable content.
- **Disabled Control Text:** A fixed neutral gray is used only for disabled button labels.

### Named Rules

**The Palette Owns the Theme Rule.** Resolve surfaces, ordinary text, dividers, selections, and button fills through Qt palette roles so saved launcher themes remain authoritative.

**The Action Blue Is Semantic Rule.** Use the launcher highlight role for the current primary action and selected navigation; never introduce a competing fixed accent.

**The Status Is Never Color Alone Rule.** Every status dot, metric color, or warning color must be paired with explicit text naming the state or condition.

## Typography

**Display Font:** Qt application font (platform and launcher fallback)
**Body Font:** Qt application font (platform and launcher fallback)
**Label/Mono Font:** Qt application font for labels; Courier New for console output

**Character:** Typography is compact and operational. Weight and a short size ladder establish hierarchy while native font resolution keeps the interface familiar and scalable in both launcher appearances.

### Hierarchy

- **Display** (700, 18pt): The Server Manager window title.
- **Headline** (700, 14pt): High-value metric readings and comparable operational values.
- **Title** (700, 12pt): Rail headings, selected-server titles, and section anchors.
- **Body** (400, native Qt size): Descriptions, metadata, form copy, table content, and server details.
- **Label** (600, native Qt size): Primary actions, group-box titles, and table headers.
- **Console** (400, 9pt): Dense server output in Courier New; commands and surrounding controls remain in the native application font.

### Named Rules

**The Operational Ladder Rule.** Use bold weight and the established 18pt, 14pt, and 12pt steps for hierarchy; keep routine controls and metadata at the native application size.

**The Console Is the Only Mono Rule.** Reserve Courier New for server output; do not turn navigation, metadata, or settings into terminal-styled text.

## Layout

The workspace uses a zero-margin outer shell with an 84px minimum header and a horizontal split beneath it. The searchable server rail is constrained to 264–328px, while the selected-server workspace takes the remaining width. Within that workspace, a 78px minimum command bar stays above a 142px tool rail and focused content canvas.

Spacing is compact and nested: 8px for adjacent actions, 10–14px within control groups, 18px around working canvases, and 24px at the global header. Server cards use 82px rows; tool destinations use 44px rows, keeping targets comfortable without relaxing operational density. The empty-fleet state expands the server area and removes the redundant detail pane.

**The Context Before Tools Rule.** Keep the selected server, its status summary, and the safest available start or stop action visible while users move among Home, Console, Mods or Plugins, Players, Files, Backups, Tools, and Settings.

## Elevation & Depth

This system uses no shadows. Depth comes from flat tonal layering: Window for the canvas, Base for active work surfaces, AlternateBase for rails, and Midlight one-pixel separators for boundaries. Hover and selection shift tone or border color rather than lifting components.

### Named Rules

**The Flat Tonal Layers Rule.** Separate regions with palette tone and restrained one-pixel borders; do not add drop shadows, glow, blur, or floating-card elevation.

## Shapes

The rounded vocabulary has three durable levels: gently rounded controls and navigation rows (8px), server cards and grouped content (10px), and major command or workspace panels (12px). Borders stay one pixel and silhouettes remain rectangular; circular status dots are a small semantic indicator, not a competing shape language.

**The Three-Radius Rule.** Use 8px for controls, 10px for contained cards or groups, and 12px for major workspace surfaces.

## Components

### Buttons

- **Shape:** Compact native controls with gently rounded corners (8px), a 30px minimum styled height, and 12px horizontal padding; key header and process actions use 38px minimum height.
- **Primary:** Launcher Highlight fill, HighlightedText foreground, matching highlight border, and semibold text. Only the safest currently available process action receives this role.
- **Hover / Focus:** Hover changes the border to Launcher Highlight. Keyboard focus uses a two-pixel highlight border while compensating padding to prevent layout shift.
- **Secondary:** Native Button fill and ButtonText foreground with a quiet Midlight border.
- **Danger:** Native button fill is preserved; only the text changes to the appearance-specific danger token.
- **Disabled:** AlternateBase fill, disabled text, and Midlight border preserve the control's location while clearly removing availability.

### Cards / Containers

- **Server Cards:** Transparent at rest, Base on hover or selection, with a 10px radius. Selection adds a Launcher Highlight border.
- **Major Panels:** Base surfaces with one-pixel Midlight borders and a 12px radius.
- **Grouped Content:** Base surfaces with one-pixel Midlight borders, a 10px radius, and 12px internal padding.
- **Shadow Strategy:** None; use the flat tonal layering rules above.

### Inputs / Fields

- **Style:** Base fill, native text, one-pixel Midlight border, 8px radius, 32px styled minimum height, and compact horizontal padding.
- **Focus:** Two-pixel Launcher Highlight border with compensated padding; selection uses Highlight and HighlightedText palette roles.
- **Disabled:** Retain native Qt disabled behavior and readable field context.

### Navigation

- **Style:** A fixed 142px vertical tool rail on AlternateBase with blue launcher theme icons, native labels, and 44px destination rows.
- **Hover:** Base tonal fill without elevation.
- **Active:** Launcher Highlight fill, HighlightedText foreground, and an 8px radius.
- **Disabled:** Mid palette text and non-selectable behavior preserve location without implying access.

### Server Command Context

The command bar is the signature component: selected-server name and metadata stay on the left, while Start or Stop, Restart, and Delete remain aligned on the right. Runtime state changes which process action is primary, so the visually strongest action is also the safest available next step.

### Status Indicators

Server cards pair an 8px colored dot with the written state. Home metrics repeat the written state and may color the value. Health bars and warnings reuse the same running, transition, error, and unavailable vocabulary with explanatory labels.

## Do's and Don'ts

### Do:

- **Do** resolve ordinary surfaces, text, dividers, selection, and primary emphasis through Qt palette roles.
- **Do** preserve valid saved user icon themes through `QIcon::fromTheme`, then fall back to the built-in blue launcher set before using a native platform icon.
- **Do** keep the selected server and safest process action visible across administration tools.
- **Do** pair every status color with a written state, warning, or diagnosis.
- **Do** verify both approved light and dark appearances whenever a fixed semantic color is introduced.
- **Do** preserve native keyboard focus, disabled states, and command-entry behavior.

### Don't:

- **Don't** replace palette-adaptive surfaces with fixed light or dark hex backgrounds.
- **Don't** add shadows, glass effects, decorative gradients, or floating-card depth.
- **Don't** introduce a second accent that competes with the launcher highlight and blue icon identity.
- **Don't** hide destructive meaning in an icon or color alone; keep the Delete label and appearance-aware danger text.
- **Don't** collapse server selection, selected-server context, and tool navigation into an undifferentiated row of tabs or buttons.
- **Don't** restyle the command console as a system-wide visual language.
