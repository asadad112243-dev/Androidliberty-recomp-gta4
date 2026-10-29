# Mali / MediaTek devices (Infinix Hot 60 Pro+ and similar)

The port was developed on a Snapdragon 865 / Adreno 650. Phones with a Mali GPU use a different
profile. Target of this document: **Infinix Hot 60 Pro+**, MediaTek Helio G200 (2x Cortex-A76 at
2.2 GHz, 6x Cortex-A55 at 2.0 GHz), Mali-G57 MC2, 6-8 GB RAM.

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

## The profile (`default_args_non_adreno.txt`)

- 540p output, FXAA/SMAA off, motion blur off: the Mali-G57 MC2 is the limiting factor.
- Traffic/ped density 0.3 (default 0.4): the render thread replays every draw.
- `--gta4_fps_guard=2`, 30 FPS limit: a locked 30 is steadier than an uneven 40-60.
- `--gta4_thread_pinning=2`: render and recorder threads on the two A76 cores.
- `--android_thermal_guard=true`: lowers draw distance before throttling.

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
