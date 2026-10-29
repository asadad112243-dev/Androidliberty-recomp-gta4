# Mali / MediaTek devices (Infinix Hot 60 Pro / Pro+ and similar)

The port was developed on a Snapdragon 865 / Adreno 650. Phones with a Mali GPU use a different
profile. Target of this document: **Infinix Hot 60 Pro and Pro+**, which share the MediaTek
Helio G200 (2x Cortex-A76 at 2.2 GHz, 6x Cortex-A55 at 2.0 GHz), Mali-G57 MC2, up to 8 GB RAM and a
144 Hz 1224x2720 AMOLED panel.

> Status: written without access to the device. The code compiles in isolation but the profile is
> untested on hardware. Treat the numbers as a starting point.

## What changes automatically on a device with no Adreno GPU

| Area | Behaviour |
| --- | --- |
| Driver | `DriverBridge` already skips the Turnip bundle and uses the system Vulkan driver. |
| `TU_DEBUG=sysmem` | Only set on Adreno (it is a Turnip flag). |
| First-launch `args.txt` | Created from `assets/default_args_non_adreno.txt` (see below). |
| Thread pinning | Cores are ranked by `cpu_capacity` first, then max frequency, so the two A76 cores are found even though their clock is close to the A55s. |
| Thermal guard | Without Adreno's KGSL temperature, falls back to a `/sys/class/thermal` zone (GPU, then SoC/CPU) when the thermal HAL reports no headroom. |

## The 144 Hz panel

Guest vblanks follow the display's vsync (Choreographer). The console runs at 60 Hz, so the
display has to be 60 or 120 Hz for every second (or fourth) callback to be a vblank. On a 144 Hz
(or 90 Hz) panel the old rule passed every second callback, giving the game about 72 (or 45)
vblanks per second, which shows up as uneven frame pacing and wrong game speed. Two fixes:

1. `--android_display_hz=60` (new, in the profile) asks Android for the 60 Hz mode at launch.
   The log line `display: was 144.0 Hz, asked for 60.0 Hz` in logcat (tag `LibertyRecomp`)
   shows what happened. The system or the phone's game mode can still override it.
2. If the panel stays on a fractional ratio anyway, the vblank code uses a phase accumulator that
   averages exactly 60 Hz (a simulation of 144, 90 and 165 Hz panels gives 60.0 Hz; the spacing
   alternates between two values, which is why 60 Hz is still better). Whole-number ratios (60,
   120, 180 Hz) use the original rule unchanged.

## Phone settings that help

These are set on the phone, not by the app:

- Turn on **bypass charging** if the phone offers it (it was listed for this device): it powers
  the phone from the charger and stops the battery from heating while you play.
- Put the phone's game mode or performance mode on for the app, if it has one, and keep the
  refresh rate setting on Auto or 60 Hz.
- Keep 10+ GB free and close other apps; 8 GB of RAM leaves little room once the game is loaded.

## The profile (`default_args_non_adreno.txt`)

- 540p output, FXAA/SMAA off, motion blur off: the Mali-G57 MC2 is the limiting factor.
- Traffic/ped density 0.3 (default 0.4): the render thread replays every draw.
- `--gta4_fps_guard=2`, 30 FPS limit: a locked 30 is steadier than an uneven 40-60.
- `--gta4_thread_pinning=2`: render and recorder threads on the two A76 cores.
- `--android_thermal_guard=true`: lowers draw distance before throttling.
- `--android_display_hz=60`: asks for the 60 Hz panel mode (see above).

`args.txt` is only created on the first launch. After updating the APK, delete `args.txt` in the
app's data folder (or clear app data) to pick up this profile.

## Requirement: Vulkan 1.2

The renderer needs Vulkan 1.2. If the phone's Mali driver only offers 1.1 the game will not start;
`last_launch.txt` in the data folder reports the version found. Nothing in this profile can fix that;
a Mali driver cannot be swapped like Turnip.

## Tuning order if it still stutters

1. Lower `--gta4_output_height_cap` (480).
2. `--gta4_thread_pinning=3` or `0`.
3. Traffic/ped density 0.2.
4. `--gta4_draw_distance_scale=0.8` (do not set `--gta4_shadow_cascade_count`).
5. Keep the shader cache folder: the first minutes stutter while shaders compile.
