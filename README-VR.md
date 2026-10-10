# Arx Libertatis VR

A virtual reality version of [Arx Libertatis](https://arx-libertatis.org/), the open source port of Arx Fatalis,
built on OpenXR. You see the world through your headset, your body follows the headset and controllers, you swing
your weapon to fight, hold up your shield to block, draw the runes in the air with your hand, and read the book and
notes in your hands.

This is an early release (0.1.0). Expect rough edges, and please report problems.

## Requirements

- Windows 10 or 11, 64-bit.
- A PC VR headset with an **OpenXR runtime that supports OpenGL**:
  - **SteamVR** (any headset it supports, including Quest through Virtual Desktop, Steam Link or ALVR), or
  - the **Meta Quest PC app** (Quest Link with a cable or Air Link).
  - Not supported: Windows Mixed Reality's own runtime and Virtual Desktop's VDXR runtime (no OpenGL). With Virtual
    Desktop the game uses SteamVR instead, as long as SteamVR is installed.
- The game data of **Arx Fatalis** (Steam, GOG or CD), or the free demo. The data is not included.
- Controllers: Meta Touch, Valve Index, HTC Vive and Windows Mixed Reality controllers have bindings.

## Installation

1. Extract the zip into a new folder, for example `C:\Games\ArxLibertatisVR`.
2. Make the game data available, either way:
   - If [Arx Libertatis](https://arx-libertatis.org/) is already installed with the game data, nothing is needed: the
     game finds the data through the registry.
   - Otherwise copy the `.pak` files of Arx Fatalis (`data.pak`, `data2.pak`, `loc.pak`, `sfx.pak`, `speech.pak` and
     the `*_default.pak` files if your copy has them) into the folder of `arx.exe`.
     See <https://arx.vg/data> for details and the demo.
3. Start your VR runtime (SteamVR, or Quest Link), then start `arx.exe`.

VR is on by default. Without a headset or runtime the game starts in the normal flat mode. Run `arx.exe --no-vr` to
play flat on purpose.

Saved games and settings are in `%USERPROFILE%\Saved Games\Arx Libertatis`, shared with a normal Arx Libertatis
install. Starting the normal Arx Libertatis rewrites `cfg.ini` without the `[vr]` section, which resets the VR
settings to their defaults.

## Controls

The right hand is the main hand (Options → VR settings → Main hand switches it).

| Control | Action |
|---|---|
| Left stick | Move (towards your head, or the hand: VR settings) |
| Right stick | Turn (snap or smooth: VR settings) |
| Right stick click | Crouch (or just crouch down for real) |
| Left stick click | Sneak on/off |
| A | Jump; also changes level when the stairs icon shows on your left wrist |
| B | Book (hold it to open the spell pages) |
| X | Inventory (above your left wrist) |
| Y / Menu | Game menu (pauses the game) |
| Right trigger | Click what you point at; in the world, tap it to enter magic mode |
| Right grip | Activate what you point at: doors, levers, talking, looting dead bodies |
| Left grip | Activate what the left hand points at; right click on panels |
| Grip on an object | Take it (short press), or hold to grab, carry and throw it; far objects fly to your hand when you flick it |
| Trigger while holding an object | Put it into the inventory |
| Left trigger | Potion wheel: move the hand towards a potion and let go to drink it |
| Grip and trigger | Make a fist to punch |

**Weapons.** Reach over a shoulder and press the grip to draw your weapon into that hand; bring it back over the
shoulder, blade pointing back, to put it away. Swing to strike: faster, wider swings hit harder. A shield held up
towards an attacker blocks part of the hit (the Defense skill helps; VR settings → Physical shield block).

**Bow.** Draw the bow over a shoulder like a weapon. Bring the other hand near the bow to nock an arrow, press its
trigger to take the string, pull back and let go.

**Magic.** Tap the right trigger to enter magic mode (the hand glows). Hold the trigger and draw a rune, release,
draw the next one, then tap the trigger to cast. Hold the right grip while drawing to precast. The runes drawn show
on your right wrist. VR settings → Improved rune drawing (experimental) steadies the strokes.

**Torch.** A lit torch is held in the off hand. Bring it to your hip to hang it on the belt, grab it there to take it back.

**Book and notes** are held in your hands; point with the other hand and use the trigger. Naming a saved game shows a
keyboard next to the menu.

## VR settings

Options → VR settings: turning, snap angle or smooth speed, comfort vignette, physical shield block, improved rune
drawing, movement direction, main hand, height, throw strength, and the desktop view (full screen, window or off;
applied after a restart).

More settings are in the `[vr]` section of `cfg.ini`. The OpenXR runtime is chosen with `runtime=`:

- `auto` (default): the system default runtime, or SteamVR, or the Meta runtime, the first one with OpenGL support.
- `steamvr` or `meta`: that runtime.
- the full path to a runtime's `.json` manifest.

## Known issues

- An object held directly in the hand when saving floats in the air where it was after loading.
- The low polygon player body is close to your eyes; some armor may clip into the view.
- Only English tutorial texts name the VR controls.

## Troubleshooting

The log is `%USERPROFILE%\Saved Games\Arx Libertatis\arx.log` (written when the game exits). Lines starting with
`OpenXR` show which runtime was used and why VR could not start.

- **The game starts flat:** start the VR runtime first. With Virtual Desktop, install SteamVR (or set
  `runtime=steamvr`).
- **No sound in the headset:** select the headset as the Windows audio output, or in Options → Audio.

## License

Arx Libertatis VR is free software under the GNU General Public License version 3 or later, with the additional
terms of Arx Libertatis (see `COPYING.txt` and `LICENSE.txt`). The licenses of the libraries are in the `license`
folder. Arx Fatalis is a trademark of its owners; this project is not affiliated with Arkane Studios,
ZeniMax or Bethesda.
