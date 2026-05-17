# Vulkan headers (vendored)

This directory holds the C headers required to build tyrquake's Vulkan
renderer backend (`common/backend_vulkan.c`).  They are vendored
verbatim from upstream so that the libretro core has **no build-time
dependency** on a system Vulkan SDK or any external tooling.  Anyone
checking out the repository can build the Vulkan backend with nothing
beyond a C toolchain.

## Required files

When the Vulkan backend is enabled (`make RHI_BACKEND_VULKAN=1`), the
build expects the following layout under `deps/vulkan/`:

```
deps/vulkan/
  include/
    vulkan/
      vulkan_core.h          (from Khronos)
      vk_platform.h          (from Khronos)
    libretro_vulkan.h        (from libretro-common upstream)
  README.md                  (this file)
  LICENSE.Khronos            (Apache-2.0 -- vulkan_core.h, vk_platform.h)
  LICENSE.libretro           (zlib       -- libretro_vulkan.h)
```

`vulkan_core.h` includes `vk_platform.h` via `#include "vk_platform.h"`,
so both must be present in the same `vulkan/` subdirectory.

## Sources

### Khronos headers (vulkan_core.h, vk_platform.h)

Canonical upstream:
[KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers/)

Pinned tag for this vendor drop: **`v1.3.280`** (chosen for broad
driver coverage -- shipping in Mesa 24+, NVIDIA 555+, AMD Adrenalin
24.x).  The tyrquake Vulkan backend only uses Vulkan 1.0 core
features for portability, so the header version is effectively a
ceiling, not a floor; any 1.3.x or 1.2.x release of vulkan_core.h
would work just as well.

To re-vendor or update:

```
git clone --depth 1 --branch v1.3.280 \
    https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vkh
cp /tmp/vkh/include/vulkan/vulkan_core.h  deps/vulkan/include/vulkan/
cp /tmp/vkh/include/vulkan/vk_platform.h  deps/vulkan/include/vulkan/
cp /tmp/vkh/LICENSE.md                    deps/vulkan/LICENSE.Khronos
```

License: Apache-2.0 (see `LICENSE.Khronos`).

### libretro Vulkan interface (libretro_vulkan.h)

Canonical upstream:
[libretro-common](https://github.com/libretro/libretro-common/blob/master/include/libretro_vulkan.h)

This header defines the `retro_hw_render_interface_vulkan` struct
that the frontend passes back to the core via
`RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE`.  The interface gives the
core a handle to the frontend's `VkInstance`, `VkPhysicalDevice`,
`VkDevice`, queue, and a `vkGetInstanceProcAddr` function pointer
for loading every other Vulkan symbol.

To re-vendor:

```
git clone --depth 1 https://github.com/libretro/libretro-common.git /tmp/lrc
cp /tmp/lrc/include/libretro_vulkan.h     deps/vulkan/include/
cp /tmp/lrc/LICENSES/zlib                 deps/vulkan/LICENSE.libretro
```

License: zlib (see `LICENSE.libretro`).

## What we do NOT vendor

* **vulkan.h** -- The outer umbrella that pulls in platform-specific
  headers (Win32 / Android / Wayland / etc.).  We only use the
  platform-independent `vulkan_core.h` plus the libretro HW interface;
  the frontend handles the platform glue.
* **Volk, VMA, vulkan_hpp, glslang, SPIRV-Cross, etc.** -- C++ helpers
  and toolchain dependencies.  tyrquake's Vulkan backend is pure C and
  loads function pointers manually from `vkGetInstanceProcAddr`.

## How the backend uses them

`common/backend_vulkan.c` is the single-TU implementation.  When
`RHI_BACKEND_VULKAN` is defined at compile time, it:

1. Includes `vulkan/vulkan_core.h` for the API types / enums / structs
   / function-pointer typedefs.
2. Includes `libretro_vulkan.h` for the libretro HW interface struct.
3. Caches `vkGetInstanceProcAddr` from the frontend's interface and
   loads every Vulkan entry point it needs into a file-static table.
4. Renders into the per-frame `retro_vulkan_image` the frontend
   negotiates, signalling completion via the frontend-provided
   semaphore.

The backend builds without linking `libvulkan` or any other library;
all Vulkan symbols are resolved at runtime through the loader the
frontend hands us.

## Why we don't pre-compile or bundle a Vulkan loader

A libretro core runs inside a frontend that already has its own
Vulkan loader linked in for its own UI rendering.  The frontend passes
us its `vkGetInstanceProcAddr` so we use the same loader; bringing
our own would conflict and waste memory.  This is the standard
libretro Vulkan model -- see `paraLLEl-RDP`, `beetle-psx-libretro`,
and other RetroArch cores for the same pattern.
