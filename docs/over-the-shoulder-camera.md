# Over-the-shoulder / first-person camera (experimental)

中文：[over-the-shoulder-camera.zh-CN.md](over-the-shoulder-camera.zh-CN.md) · Typeset PDF (Chinese): [over-the-shoulder-camera.zh-CN.pdf](over-the-shoulder-camera.zh-CN.pdf)

Turns the isometric top-down game into a shooter-style over-the-shoulder follow camera, inside the graphics translation layer. Off by default, switched on with an environment variable, Metal backend (iOS / Mac) only.

**In one sentence:** the game itself is not changed at all. On the picture side, in the layer that translates NVN to Metal, the "main camera" constant buffer the game uploads every frame is replaced with our own perspective matrices; on the input side, touch points and stick directions on the perspective picture are converted back to points and directions on the original orthographic picture before the game sees them. The game still believes it is top-down; the player sees an over-the-shoulder view.

## Trying it

| Environment variable | Effect | Default |
|---|---|---|
| `DM_CAM` | `1` over-the-shoulder (picture + input conversion); `2` first person (picture only, input not converted); unset = off | off |
| `DM_CAM_DIST` / `DM_CAM_H` | over-the-shoulder: camera distance behind / height above the character | 20 / 13 |
| `DM_CAM_LOOK` / `DM_CAM_LOOKH` | over-the-shoulder: look-at point distance ahead of / height above the character | 4 / 3 |
| `DM_CAM_FOV` | horizontal field of view (degrees) | 65 over-the-shoulder, 75 first person |
| `DM_CAM_EYE` / `DM_CAM_FWD` / `DM_CAM_PITCH` | first person: eye height / eye distance ahead of the character / downward pitch | 6 / 2.2 / 8 |
| `DM_CAM_TAU` | camera follow time constant (seconds) | 0.35 |
| `DM_CAM_NEAR` | near plane | 0.3 |

On a Mac, put `DM_CAM=1` in front of the launch command; on iOS add it under Xcode Scheme → Run → Environment Variables. The log shows `DM_CAM=1：镜头实验开` once and then `镜头实验：第 N 次改相机` every 600 camera replacements.

Android / HarmonyOS use the Vulkan backend, which is not wired up: `DM_CAM` changes nothing there (input conversion gets no matrices and passes input through unchanged).

## Where the code is

| Place | What it does |
|---|---|
| `dm/gfx/nvn/nvn_mtl.m`: `cam_mode` / `cam_build` / `cam_patch`, plus one line in `encode_args` | recognises the main camera and scene blocks, binds a modified copy (one per block per frame); the math is the shared `dm/gfx/vulkan/engine/xbg_cam.h` (follow, turns, wall pull, boom, reach, look-ahead), the same as on Android / HarmonyOS |
| `dm/gfx/vulkan/engine/xbg_cam.h`, `xbg_game.cpp` (`cam` namespace) | the same camera on the Vulkan side (Android / HarmonyOS); `tests/cam_test.cpp` tests the math |
| `dm/host/dm_cam.c` | state shared between the two threads (matrices, wanted heading); stick conversion and tap conversion |
| `dm/host/hle_hid.c`: one line at the controller poll | converts the sticks before the game gets them |
| `dm/host/dm_world.c`: `w_hit`, `w_watch_step` | converts a tap per target depth when tapping NPCs / objects / corpses; reverse lookup for the test tool |

With `DM_CAM` unset every one of these paths returns unchanged and modifies nothing.

## 1. Why it is possible

- **The original camera** is a fixed isometric orthographic projection: looking down 30°, turned 45°, parallel projection, no perspective foreshortening and no sky. The game's hit tests, taps and stick directions are all defined on this picture.
- **Where the translation layer sits**: the game's graphics calls (NVN) are translated one by one to Metal. Every constant buffer bound before a draw (matrices and other parameters) passes through this layer, so its contents can be swapped before the GPU reads them.
- **Key observation**: the 3D scene is real 3D geometry (terrain, walls and characters all have height); the orthographic camera just flattens it. Use a perspective camera and the world stands up; what remains is that the game does not know the camera changed.

## 2. Overall structure

```
                 the game (unchanged, only knows the orthographic picture)
   orthographic camera, every frame ↓        ↑ converted orthographic input
  ┌──────────────────────────────┐ matrices ┌──────────────────────────────┐
  │ render thread: NVN → Metal   │ ───────→ │ input thread: host           │
  │ 1 find the main camera (608) │          │ 1 stick: screen→world→orig.  │
  │ 2 perspective from position  │ ←─────── │ 2 tap: move to target depth  │
  │   + heading                  │ wanted   │ 3 record the wanted heading  │
  │ 3 write a copy, GPU reads it │ heading  │   for the camera follow      │
  └──────────────────────────────┘          └──────────────────────────────┘
                 ↓                                     ↑
                 screen (perspective picture) / the player's finger
```

The two halves exchange only two matrix sets (`dm_cam_publish`, under a lock) and one heading (`dm_cam_want_dir`, atomics).

## 3. Picture: replacing the orthographic camera with a perspective one

### 3.1 Recognising the main camera buffer

The game uploads many constant buffers per frame, and the shadow-map ones carry camera matrices too (from the light's point of view). Only the main camera is recognised, by content signature:

| Condition | Meaning |
|---|---|
| size ≥ 608 bytes | layout length of the main camera block |
| view row 1 at +80 ≈ (0.7071, −0.3536, 0.6124), row 2 ≈ (0, 0.866, 0.5) | the original fixed isometric orientation |
| projection at +208 has third column 0, bottom-right 1 | orthographic projection |

On a match the original buffer is left alone; a copy in our own ring buffer (4 MB) is modified and the binding points the GPU at the copy. The same address is modified once per frame.

### 3.2 What gets rewritten

| Offset | Contents | Treatment |
|---|---|---|
| +80 / +144 | view / inverse view | recomputed for the new camera |
| +208 / +272 | projection / inverse projection | perspective (reversed Z, see 3.3) |
| +336 / +400 | view-projection / inverse | recomputed |
| +464 | previous frame's view-projection | **our own modified previous one**, otherwise motion vectors / temporal AA smear |
| +592 | depth reconstruction (far, near, ?, orthographic flag) | flag 0 selects the shaders' perspective branch, xy = (B, −A): depth d → distance = B / (d + A) |
| fragment slot 10 (1440-byte scene constants) | +16 camera position, +32 view direction, +848 linear depth origin | changed to match; the origin is 550 in the original (the orthographic camera sits far above the ground), 0 for perspective |

Two things are **deliberately left alone**:

- the shadow-map cameras: light's view, the signature does not match them anyway;
- a compute shader that culls against the orthographic frustum (`CAM_CULL_CS`, excluded by shader fingerprint). Changing it makes whole stretches of ground and walls vanish; left alone, the game still picks what to draw from the original picture's extent.

While the camera is on, runtime extrapolation (a power-saving frame interpolation that assumes the original camera) is switched off.

### 3.3 Projection matrix (reversed Z)

The game uses reversed-Z depth (near = 1, far = 0) and the perspective projection follows that convention. Row-vector convention (clip = [x y z 1] × M):

```
sx = 1 / tan(hfov / 2)            sy = sx × the original projection's aspect (pr[5] / pr[0])
A  = n / (F − n)                  B  = n·F / (F − n)        n = 0.3, F = 3000
P  = | sx  0   0   0 |
     | 0   sy  0   0 |
     | 0   0   A  −1 |
     | 0   0   B   0 |            view z = −n → depth 1, z = −F → depth 0
```

The view matrix is a standard look-at: back = normalize(eye − target), right = up(0,1,0) × back, up = back × right. Inverses use 4×4 Gauss-Jordan in double precision.

### 3.4 Where the camera goes

**The character position** is not read from game memory: the original orthographic camera always aims at the character, so intersecting its sight line (camera position and view direction from the original inverse view) with the ground y = 0 gives the character's feet.

| Parameter | Over-the-shoulder | First person |
|---|---|---|
| camera position | 20 behind the character, 13 high (raised further by the pitch limit) | 2.2 ahead, 6 high (eye level) |
| looks at | 4 ahead of the character, 3 high | straight ahead, 8° down |
| horizontal FOV | 65° | 75° |

### 3.5 Camera follow

- **Wanted heading**, by priority: aim-drag direction > move-stick direction (both converted to world directions by the input side) > the character's per-frame displacement; with none of them the heading is kept.
- **How it turns**: a wall-clock first-order lag on the angle, `angle += diff × (1 − e^(−dt/τ))`, τ = 0.35 s, the shorter way round. Wall clock rather than frames, so the turn rate is the same at 30 / 40 / 60 fps.
- **Reset**: on the first frame, or when the character moves more than 20 in one frame (area change, teleport, doors), the heading resets to the horizontal direction of the original sight line (−0.707, −0.707), so a new area starts out facing the same way as the original.

### 3.6 Pitch limit: keep the horizon out of the picture

The original is a top-down game and **has no sky at all**. As soon as a perspective camera looks up, everything above the horizon is black. The top edge of the picture is kept at least 3° below the horizon; the vertical half-angle follows this frame's aspect ratio (an iPad is squarer than an iPhone, so the same horizontal FOV gives a larger vertical one), and the camera is raised when it is too low:

```
tan(vertical half-angle) = tan(hfov / 2) × height / width
minimum camera height    = look-at height + (distance + look-ahead) × tan(vertical half-angle + 3°)
```

## 4. Input: the game only understands the original picture

The picture is perspective, but the game's taps, hit tests and sticks are all computed on the original orthographic picture. The game is not touched; input is converted once before it is handed over. Every frame the render thread publishes **the original orthographic view-projection (and inverse), the perspective view-projection (and inverse), and the character's ground point**; with no update for 0.5 s the camera counts as off and input passes through unchanged.

### 4.1 Basic conversion: perspective picture point → original picture point

1. Screen point → clip coordinates; unproject at a near and a far depth with the perspective inverse to get a ray.
2. Intersect the ray with some plane to get a world position.
3. Project the world position with the original orthographic view-projection to get the same thing on the original picture.

### 4.2 Sticks: camera-relative

In the original, a stick direction corresponds to the screen direction (x, y/2) (isometric 2:1). Over-the-shoulder wants "push up = walk where the camera looks":

1. stick → screen direction (x, y/2), normalized;
2. take the camera's horizontal heading f (xz of the screen-center ray) and rotate into a world direction with up = f, right = (−f.z, f.x);
3. take two points from the character's feet along that world direction, project both with the original orthographic matrix to get the original screen direction, then undo 2:1 back to a stick value of the same magnitude.

Do not derive the direction from "the character's point on the perspective picture": it drifts by a few degrees depending on where the character is on screen (measured 6° off pushing straight up). Rotating by the camera heading is exact.

> **Pitfall: holding right runs in circles.** The stick is camera-relative while the camera chases the stick's world direction; they chase each other and holding right circles.
> **Fix: lock the reference camera per push.** The camera at the moment the stick goes from 0 to non-zero is the reference until release. Holding right then runs straight towards the right of the picture as it was when pressed, and the camera swings behind the character in about 1.5 s; the next push uses the new camera. Each stick has its own lock.

### 4.3 Tapping a target: move the tap to the target's depth

Intersecting the tap ray with the ground does not work: characters and objects have height, so a ray through a body lands on the ground behind it. The conversion is done **per candidate target**:

1. bring the target's feet point on the original picture back to the world (orthographic sight lines are parallel: intersect the line through two depths with y = 0);
2. intersect the tap ray with the vertical plane through the feet whose normal is the ray's horizontal direction; targets whose hit lies behind the camera (t ≤ 0) are skipped;
3. project the hit point back onto the original picture and test it with the existing body box / footprint diamond / selection box; with several hits take the nearest.

Not one of the original hit rules changes; only "where the tap is" does. The test tool ("tap a unit" in scripts) gets the reverse conversion: original feet / head points → the middle of the body on the perspective picture.

## 5. Pitfalls

| Symptom | Looked like | Actually | Fix |
|---|---|---|---|
| a large black area in the distance | culling | widening the culling extent 4× and 16× barely changed the black share (40.7% → 40.2%). The black is the sky: a top-down game has none | pitch limit, horizon kept out of the picture |
| pushing up walks backwards | stick conversion bug | default heading sign flipped: the original camera looks towards −x −z, +0.707 is exactly 180° off | default / reset heading (−0.707, −0.707) |
| holding right runs in circles | follow too fast | camera-relative stick + camera chasing the stick = positive feedback | lock the reference camera per push |
| whole stretches of ground and walls vanish | — | the compute shader culling against the orthographic frustum got the new matrices too | excluded by shader fingerprint |
| depth-based effects misplaced | — | shaders pick the depth reconstruction formula by the "orthographic flag" | flag 0, parameters from the perspective projection; camera position, view direction and linear depth origin in the scene constants updated too |

## 6. Measurements (original implementation, iOS App running on a Mac)

| Test | Expected | Result |
|---|---|---|
| push the stick up | walk where the camera looks | walks forward |
| hold the stick right | run straight, camera swings behind | runs straight, camera behind in about 1.5 s |
| tap a chest / an NPC | hit, panel opens | hit, panel opens |
| drag a skill button towards a boss | target locked | locked inside the cone, 0° off |
| switch off | identical to the code without this feature | pause / statistics pages 0 pixels different, 0 camera log lines |

## 7. Current limits and good next steps

- **Names, health bars and item labels are still drawn at orthographic positions**: their positions come from the game's world → screen projection (`main+0x90d110`, also used by `w_project` in `dm_world.c`). The proper fix is to return perspective coordinates at that function's exit.
- **Dungeons have missing walls and look dark**: the scenes are designed for a top-down view and that shows in perspective.
- **Tap-to-walk on the ground** does not exist (the original touch controls do not have it either).
- **First person** is picture only; input is not converted.
- **Android / HarmonyOS** (Vulkan backend) are not wired up: the main camera signature and offsets are game data, independent of the backend, and the math in `cam_build` carries over as is; what is needed is the one place in the Vulkan backend where constant buffers are bound.

## 8. Doing the same in another game, in this order

1. **Find the injection point**: you must be able to change constant buffers before the GPU reads them (a graphics API translation layer, a driver hook, RenderDoc-style interception).
2. **Find the main camera buffer**: scan all constant buffers for a view matrix with the original fixed orientation (a constant in isometric games, so the easiest to spot) and note the layout: view, projection, their inverses, previous view-projection, depth reconstruction parameters.
3. **Swap only the matrices first**, camera pinned behind the character, and see whether the world stands up; then fix one by one: depth reconstruction, culling (find the frustum-culling pass and exclude it), the previous-frame matrix, camera positions duplicated in scene constants.
4. **Add follow and the pitch limit**: wall-clock lag, reset on area change, horizon kept out of the picture.
5. **Convert input last**: sticks rotated by the camera heading, taps moved to "the target's depth layer" and converted back to the original picture; lock the reference camera per gesture.
6. **Make it a switch**, off by default, and compare pixel by pixel with the original while off.
