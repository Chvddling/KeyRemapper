
<img width="743" height="738" alt="image" src="https://github.com/user-attachments/assets/423fba92-96d3-46c3-ad85-d0c8d738b07c" />

# Key Remapper

Per-game key remapping and display modes for Windows 10/11. Remaps only while the selected game is the
foreground window, pauses automatically when you Alt+Tab out, resumes when you come back.

## Use

1. Run `KeyRemapper.exe` (it asks for admin rights so it can remap games that run as admin).
2. **Add game...** and pick the game's `.exe`.
3. **Add...** a mapping: choose (or **Detect**) the key you press and what the game should get.
   Targets can be any key, a mouse button, or "Disabled" to block a key.
   Already set up another game? Use **Copy from...** to bring its bindings over (replace this
   game's bindings or add to them). When you add a new game, Key Remapper offers this right away.
4. Click **Hide to tray** (or close the window). It keeps running in the background.

Example: `Up -> W`, `Z -> Num 4`.

- **Ctrl+Shift+F12** pauses/resumes all remapping (also in the tray menu).
- **Start with Windows** creates a logon task in Task Scheduler so it starts in the tray with
  admin rights and no UAC prompt.
- **Scancode-only output**: try it if a game ignores the remaps (some DirectInput games only read
  scancodes).
- Settings are stored in `KeyRemapper.ini` next to the exe (portable).

## Display modes (per game, in the Display panel)

Set the game itself to **windowed** mode in its own options first.

- **Borderless fullscreen**: the game fills the screen with black bars where needed, keeping its
  aspect ratio (a 4:3 game is never stretched). Two ways to get the picture big (**Upscaling**):
  - **Zoom the picture (any game, no lag)** - the default. The game keeps drawing at its own
    resolution and Windows' built-in full-screen zoom (the one Magnifier uses) scales the picture
    while it composes the screen, so no frame is added. Works with games that draw at a fixed size
    (the "small picture in the top-left corner" problem). Uses your main monitor. The mouse is kept
    on the game while it's in front so clicks line up with the zoomed picture.
  - **Resize the window (game redraws)** - for games that render at whatever size their window is
    (most modern games). Gives native-resolution sharpness.
- **Pixels**: *Smooth* (filtered) or *Sharp pixels* (blocky, ideal with Whole multiples). Applies
  to the zoom.
- **Resizable window (keeps aspect)**: the game window gets a resize border. Drag it to any size;
  when you let go it snaps to the correct aspect ratio. The scale and position are remembered and
  restored the next time the game starts. The game has to redraw at the new size for this one.
- **Aspect ratio** (resize methods): "Game's own" uses the game's original resolution (detected the
  first time it runs). You can force 4:3, 16:9, 16:10 or 5:4.
- **Size**: "Best fit" uses all the space; "Whole multiples" uses exact 2x/3x/4x... of the
  original resolution for crisp, even pixels.
- **Sharp on high-DPI screens**: on a screen scaled above 100%, Windows stretches older games and
  makes them blurry. This sets Windows' own "High DPI scaling override: Application" for the game
  exe. Takes effect the next time the game starts. Turned on automatically the first time you pick
  a display mode.
- **Forget size**: re-detects the game's original resolution (use it after changing the game's
  resolution if something looks off).

Hotkeys while a library game is in front:

- **Ctrl+Alt+Enter**: switch between borderless fullscreen and window
- **Ctrl+Alt+PgUp / PgDn**: make the window bigger / smaller (remembered)

There is no capture or re-rendering in either method, so no input lag is added. When Key
Remapper exits (or you Alt+Tab away) the zoom is switched off, and on exit game windows are put
back as they were.

A Linux version is in the `linux` folder - see `linux/README.md`.

## How it keeps latency low

- Keyboard hook lives on its own time-critical thread and is only installed while a library
  game is in the foreground. Outside games nothing intercepts your input.
- Hot path: one 512-entry table lookup by physical key (scancode + extended bit), then one
  `SendInput`. No allocations, locks, or string handling.
- Foreground changes are event driven (`SetWinEventHook`), with a 250 ms safety re-check.
- Windows power throttling (EcoQoS) is turned off for the process and the input thread, and the
  process runs at high priority, so Windows doesn't slow it down while it's in the background.
- Held keys are released automatically when the game loses focus (no stuck keys).

## Limits

- Games with kernel anti-cheat may ignore or flag injected input. Check the game's rules before
  using any remapper online.
- The game is matched by full exe path, or by file name if the path differs (e.g. game moved).
  If a launcher starts a different exe, add the actual game exe.

## Build

Pure C + Win32 (GDI+ for the glossy anti-aliased drawing), no C runtime. Built with clang, lld-link, llvm-rc and the mingw-w64 headers
(`build.sh`). With MSVC instead:

```
rc /fo app.res src\app.rc
cl /O2 /GS- /DUNICODE src\main.c src\theme.c src\winmode.c app.res /link /entry:WinMainCRTStartup /nodefaultlib /subsystem:windows ^
   kernel32.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib ^
   gdiplus.lib uxtheme.lib dwmapi.lib advapi32.lib
```
