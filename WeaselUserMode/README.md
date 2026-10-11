# Weasel User Mode — screenshot orange theme and visual preedit

This Windows user-mode frontend operates without installing a TSF IME and without administrator permissions. It intercepts keys only for ordinary keyboard layouts and commits text through Rime IPC/SendInput.

## Keyboard shortcuts

- **Ctrl+Space** toggles Rime's `ascii_mode` directly from the frontend, regardless of the current schema's `key_binder` entries.
- **Ctrl+Alt+F11** enables/disables the entire user-mode keyboard hook (not the Chinese/English mode).
- **Ctrl+Alt+F12** exits User Mode.

The tray icon indicates Chinese/English/paused states. Windows or application shortcuts may override some combinations if the frontend is paused or another IME is active.

## Preedit overlay (no TSF)

The unfinished composition is painted in a small, topmost, click-through window. It is **visual only**: it does not modify the application's text, undo stack, or caret position. A selected candidate is committed to the target application by SendInput.

Right-click the User Mode tray icon -> **Preedit 顯示模式**:

| Mode | Registry value (`HKCU\Software\Rime\Weasel\UserMode\OverlayMode`) | Behavior |
|---|---:|---|
| Off | 0 | Use the built-in Weasel candidate panel with preedit (fallback) |
| Auto (default) | 1 | Near-opaque white or charcoal background + contrasting text, sampled beside caret |
| Transparent | 2 | Color-key transparent background + contrast-aware foreground and underline |
| Solid | 3 | Fixed white background, dark text |

Auto/transparent mode samples pixels immediately outside the overlay and applies a brightness hysteresis to avoid rapid color flipping. If UI Automation and Win32 caret reporting both fail, the overlay uses the same approximate target-input anchor as the candidate window. It is not true inline text composition. Some protected windows, elevated programs, full-screen content, or mixed/colorful backgrounds may yield imperfect appearance.

If the overlay window cannot be created, User Mode disables overlay and keeps the native candidate-panel preedit visible.

## Screenshot-inspired candidate theme

The packaged `output/data/weasel.yaml` defaults to `screenshot_orange`:

- White candidate background with `#FF8C00` border and selected row
- Selected text white, normal text black
- **Noto Sans TC** for Chinese/candidate/emoji fallback
- **Consolas** for candidate selection labels, such as `1QY`, `2AH`
- Square edges, thin orange border, **18pt candidate text and 16pt
  monospaced labels** with compact row padding (rather than a screen-filling
  26–28pt panel). Eight candidates should fit comfortably on a 768px desktop.

Install Noto Sans TC and Consolas for the intended appearance; Windows substitutes available fonts if they are missing. The monospace font affects the Rime-provided label, not candidate text. Source labels and number of candidates depend on the installed Rime schema.

Users can override these defaults in `%APPDATA%\WeaselUserMode\weasel.custom.yaml`. A pre-existing user customization can supersede the packaged scheme. Re-deploy Rime from the tray after changing any configuration.

## Validation

GitHub Actions' **User-mode frontend** compiles the frontend with the overlay and tray UI. **User-mode installer** builds the complete runtime and packaged installer, validates the theme, runs Rime IPC conversion and explicit ASCII-mode toggle smoke tests, and checks instance/layout guards. The IPC smoke test does not simulate a real keyboard hook or validate visual positioning, which still need interactive Windows desktop testing.
