---
version: alpha
colors:
  background: "#0d0d0f"
  text: "#e9d9ba"
  statusScrim: "#0d0d0fe6"
typography:
  status:
    fontFamily: "Android system sans-serif, Noto Sans CJK"
    fontSize: "18px"
spacing:
  statusHorizontal: "28px"
  statusVertical: "18px"
omitted:
  - section: rounded
    reason: "Touch controls reuse the upstream circular targets, not card geometry."
---

## Overview
An Android host for the original offline 3D game. The original game's rendered
interface owns the visual identity. The shell's single job is to host that image
and give truthful preparation or failure feedback. Never replace game frames
with mock artwork or test geometry. Existing iOS gameplay is the sibling flow.

## Colors
GameActivity is the canonical owner of shell colors. The near-black background
keeps the unrendered Surface unobtrusive; warm parchment text follows the game's
interface materials. Status disappears only after actual successful presentation.

## Typography
Use Android's locale-capable native font and scaled text. Chinese is the shell's
current language. Original game language follows Activity.Configuration.

## Layout
Landscape full-screen SurfaceView; preparation and failure occupy a stable
overlay. Gameplay controls reuse the upstream C layout and gesture state machine.
Status padding is
in density-independent units and text sizing follows Android font scale.

## Elevation & Depth
The status scrim overlays rather than resizes the game surface. No shadows.

## Shapes
The original game owns game UI shape. The shell has a rectangular full-screen
surface and status scrim.

## Components
GameActivity owns shell state: prepare files, attach surface, start once, report
native progress, reveal real presentation, retain actionable failures. Surface
loss suspends graphics and drains before releasing its acquired native window.
Native text status supplies an accessibility live region. No network permission.

## Android touch adapter
The unchanged `dm/ios/Sources/dm_touch_controls.c` owns layout, hit testing,
movement axes, skill aiming, skill-set timing and sampled-input pulse behavior.
`TouchControlsView` renders in the iOS overlay's visual language
(`dm/ios/Sources/DmTouchOverlay.m`): dark disc #16110c, bronze ring #997845,
dark inner ring #1f170d, sparkle-knob bronze #c8a460, parchment #d9c9a6.
Action buttons (skills, main, set switch) fade to 60% while idle
(DM_SKILL_IDLE_ALPHA) and go solid while held; belt/menu/map cells draw a
fainter ring (belt cells numbered 1–4) so the game's own HUD artwork stays readable; the joystick is a
faint ring whose sparkle knob follows the output axes, with a ±30° aim arc
while deflected. Its native Canvas painter consumes these canonical Java
constants. Coordinates use dp, with original safe-inset and tall-screen
scaling. Buttons remain anchored while pressed. Labels describe controller
slots until native skill identity is audited. Virtual accessibility buttons
expose clicks and directional movement without dragging.

Menu and combat modes are explicitly selected because this Android build's
91738 scene observer is not mapped yet. The mode switch cancels all held input.
Unclaimed combat touches continue through original HID touchscreen states.
Portal, difficulty mutation, multi-row belt actions and inventory mouse/drag
bridges stay absent until the current game's native identities are established.
Never copy 94012 guest addresses into this Android build.

## Do's and Don'ts
`ShellTextInputDialog` owns all Android SDK text-entry modals. It uses native
EditText/IME and AlertDialog focus, Back and accessibility behavior; the panel
uses opaque ink derived from TouchControlsView.INK and shared parchment TEXT.
The SDK supplies title, description, initial text and UTF-16 length bounds.
Confirm returns actual entered text once; Cancel, Back, backgrounding and
Activity destruction return the SDK cancellation result and release the wait.
Invalid length retains the text with a correction message. Input never creates
or submits a game character automatically; the original game validates names
and owns its create action. The game surface remains behind the modal.

Keep engine paths, shader numbers and compatibility terminology in diagnostics.
Never claim game completion from a frame counter: real 3D scene evidence is a
separate device verification gate. Preserve original game resources and saves.
