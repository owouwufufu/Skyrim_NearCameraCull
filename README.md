# CameraCull

SKSE plugin for Skyrim AE (1.6.1170) that fades or hard-culls static objects,
trees, and flora that are too close to the 3rd-person camera, preventing them
from clipping through the view.

## Features

- **Hard cull** (instant AppCulled) for objects inside the inner radius
- **Alpha fade** for objects in the outer ring
- Both modes configurable per INI, including toggle per object type
- Fully restores object visibility when the camera moves away or on load

## Installation

Drop `SKSE/Plugins/CameraCull.dll` and `CameraCull.ini` into your Skyrim
`Data/` folder (e.g. via MO2). Requires SKSE64 for AE.

## Configuration

Edit `Data/SKSE/Plugins/CameraCull.ini`. All options are documented inside
the file. Key settings:

| Key | Default | Description |
|-----|---------|-------------|
| `fCullRadius` | `128.0` | Skyrim units from camera where culling begins |
| `sMode` | `both` | `hard`, `fade`, or `both` |
| `fHardRatio` | `0.5` | Inner/outer radius split for `both` mode |
| `bOnly3rdPerson` | `true` | Only cull in 3rd person |
| `iScanInterval` | `3` | Frame interval for the proximity scan |

## Building

Requires Visual Studio 2022 and vcpkg.

```
git clone https://github.com/YOU/CameraCull
cd CameraCull
cmake --preset release
cmake --build --preset release
```

Or just push a tag (`git tag v1.0.0 && git push --tags`) and GitHub Actions
will build and attach the zip to a release automatically.

## Notes on grass

Skyrim's grass geometry is procedurally generated and not addressable as
`TESObjectREFR` instances, so it's out of scope for this plugin. The fade
approach works well for all placed references (statics, trees, flora).
