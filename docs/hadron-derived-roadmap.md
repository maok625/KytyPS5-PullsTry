# Hadron-derived Kyty implementation roadmap

This roadmap turns the architecture scaffold into small, testable changes. Complete each gate
before starting the next one. Keep the work on `exp/hadron-architecture-scaffold` until the
M4 tests and source review justify a pull request.

## Phase 0 — Establish the exact baseline

**Implementation status: probe added; M4 execution still required.**

Added `tests/macosGuestAddressProbe.cpp` and the CMake target
`macos_guest_address_probe`. It records the process architecture, macOS kernel release and host
page size from both `sysctl(hw.pagesize)` and Mach. In a child process it queries the region at
`0x15b1400000`, then attempts a one-host-page `mach_vm_allocate` using `VM_FLAGS_FIXED`
without `VM_FLAGS_OVERWRITE`. If the allocation succeeds, it deallocates it before exiting.
A refusal is reported as a diagnostic result, not treated as a crash or a failed build.

This probe deliberately does not change Kyty's current allocator and does not prove that every
fixed mapping request is rejected. It checks one concrete address and size on the machine where
it is run.

Tasks still remaining:
- run the probe natively on the user's M4 and save its complete output;
- compare the reported sysctl and Mach host page sizes;
- record the exact Kyty build architecture and commit used for every test;
- add detailed logging around Kyty's existing backing allocation and fixed-view mapping after
  the standalone probe establishes the host behaviour.

Exit gate:
- the M4 output classifies the tested address as occupied, a free gap that can be reserved, or
  unavailable to fixed allocation;
- a successful probe is deallocated, and a refused probe leaves the process unchanged.

## Phase 1 — Complete the memory model outside the live emulator

**Implementation status: first reference model and tests added; execution still required.**

Added:
- `GuestMemoryModel`, implementing both backing-store and guest-address-space interfaces;
- opaque backing IDs with independent storage and offset-based reads/writes;
- reservations distinct from committed mappings;
- alias mappings that can share the same backing ID and offset;
- partial unmap and protect, permissions and mapping-generation tracking;
- explicit-copy CPU read/write and translation APIs;
- `tests/GuestMemoryAddressSpaceTests.cpp`, covering alias coherence, permissions, mapping
  splits, stale-generation detection and all-or-nothing cross-gap writes.

The test uses an explicit 4 KiB guest-page fixture only. This is not the final PS5 guest page-size
decision. Host page size remains a separate runtime property and is measured by Phase 0.

Build and run:
```sh
cmake --build _Build/macos --target guest_memory_address_space_tests macos_guest_address_probe --parallel
ctest --test-dir _Build/macos -R '^(guest_memory_address_space|macos_guest_address_probe)

## Phase 2 — Model host-page / guest-page differences

**Goal:** avoid the stale-data and neighbour-corruption bugs Hadron found on 16 KiB hosts.

Tasks:
- query host page size at runtime; do not hard-code 16 KiB;
- keep guest page size as a separate Kyty/PS5 property;
- test decommit/recommit when guest pages share one host page;
- test preservation of committed read-only, executable and no-access neighbours;
- test instruction-cache invalidation after writes through a backing alias;
- test writable shared image sections whose offsets are not host-page aligned.

Do not copy Wine patches 0034–0036 mechanically. Extract the invariants: clearing decommitted data,
preserving neighbouring permissions/content, respecting executable backing, and avoiding unsafe
whole-host-page remaps.

Exit gate:
- byte preservation and protection tests pass for partial and whole-page operations;
- the tests reproduce the old failure before the implementation change.

## Phase 3 — Integrate CPU and loader accesses

**Goal:** remove accidental dependence on host pointers before selecting the final translator.

Tasks:
- inventory every guest-address-to-pointer cast and classify it as loader, CPU, kernel or GPU;
- migrate relocation writes and image-loader access first;
- add guest instruction-fetch and data-access APIs;
- define fault/status propagation and code-page invalidation;
- keep x86-64 guest CPU context separate from the native ARM64 host context;
- put execution behind a backend interface.

Translator spike:
- evaluate FEXCore at a pinned revision from upstream, not Hadron's Wine-specific adapter;
- test one small x86-64 guest function with integer arithmetic, branches, loads/stores, calls,
  exceptions and code invalidation;
- compare it with Kyty's interpreter on the same instruction traces;
- document unsupported PS5 requirements before deciding whether to adopt it.

Exit gate:
- no direct native call of a PS5 x86-64 entry point on ARM64;
- memory faults and self-modifying code invalidation are observable by the execution backend;
- the chosen backend passes deterministic CPU tests before title boot is attempted.

## Phase 4 — Connect the GPU to the same memory authority

**Goal:** preserve PS5 CPU/GPU shared-memory semantics after host virtual addresses are abstracted.

Tasks:
- define how GPU virtual ranges resolve to backing ID + offset;
- retain existing GPU dirty-page tracking and buffer-cache invalidation initially;
- add alias tests in which CPU writes are visible to GPU uploads and GPU writes are visible after
  readback;
- validate command buffers and asynchronous readbacks across map/unmap/protect operations;
- only add direct host views or import/export optimisations after measuring the copy-based path.

Exit gate:
- CPU/GPU visibility tests pass for normal memory, direct memory and aliases;
- no stale GPU buffer-device-address entry survives an unmap or backing replacement.

## Phase 5 — Audit the KosmicKrisp patches

**Goal:** select driver fixes based on the actual Kyty shader failures and exact Mesa source base.

Order:
1. clip/cull-distance scalarisation (Hadron 0039);
2. 64-bit atomic lock lowering (Hadron 0079);
3. depth-bounds emulation for pre-family-10 Apple GPUs (Hadron 0085).

For each patch:
- compare Hadron's pinned Mesa commit with Kyty's checked-out Mesa/KosmicKrisp revision;
- inspect upstream changes since the patch's base;
- port only the minimal semantic change;
- add a small reproducer and the relevant Vulkan CTS tests;
- test performance and correctness on M4;
- keep the patch disabled or unadvertised if the driver does not fully implement the feature.

Exit gate:
- each patch has a dedicated test, before/after result and a clear supported-feature decision.

## Phase 6 — Full title validation

Only after phases 1–5:
- run the focused memory, CPU, graphics and Vulkan tests;
- run Kyty's existing CTest suite;
- boot GTA V and capture the first failure with guest address, access type, backing ID/offset,
  instruction address, thread and GPU submission context;
- compare native ARM64 results with the existing x86-64/Rosetta baseline where available.

Do not treat a later GTA V boot point as proof of correctness if memory, CPU or graphics tests regress.

## First practical milestone

The Phase 0 probe and Phase 1 reference implementation are now present on
`exp/hadron-architecture-scaffold`. The next action is to build and run the two focused tests
on the user's M4, then use the resulting host-page and fixed-address data to decide how the
existing `GuestBackingStore::MapView` path should be changed. The live memory allocator remains
untouched until that evidence is available.
 --output-on-failure
```

For a native ARM64 test, configure a separate build directory with
`-DCMAKE_OSX_ARCHITECTURES=arm64`; do not reuse an x86_64-configured build directory.

Exit gate:
- the focused tests compile and pass with sanitizers on a development host and natively on the
  user's M4;
- no test requires a guest virtual address to be a valid host pointer;
- a translation's generation is checked before a consumer retains or reuses it.

## Phase 2 — Model host-page / guest-page differences

**Goal:** avoid the stale-data and neighbour-corruption bugs Hadron found on 16 KiB hosts.

Tasks:
- query host page size at runtime; do not hard-code 16 KiB;
- keep guest page size as a separate Kyty/PS5 property;
- test decommit/recommit when guest pages share one host page;
- test preservation of committed read-only, executable and no-access neighbours;
- test instruction-cache invalidation after writes through a backing alias;
- test writable shared image sections whose offsets are not host-page aligned.

Do not copy Wine patches 0034–0036 mechanically. Extract the invariants: clearing decommitted data,
preserving neighbouring permissions/content, respecting executable backing, and avoiding unsafe
whole-host-page remaps.

Exit gate:
- byte preservation and protection tests pass for partial and whole-page operations;
- the tests reproduce the old failure before the implementation change.

## Phase 3 — Integrate CPU and loader accesses

**Goal:** remove accidental dependence on host pointers before selecting the final translator.

Tasks:
- inventory every guest-address-to-pointer cast and classify it as loader, CPU, kernel or GPU;
- migrate relocation writes and image-loader access first;
- add guest instruction-fetch and data-access APIs;
- define fault/status propagation and code-page invalidation;
- keep x86-64 guest CPU context separate from the native ARM64 host context;
- put execution behind a backend interface.

Translator spike:
- evaluate FEXCore at a pinned revision from upstream, not Hadron's Wine-specific adapter;
- test one small x86-64 guest function with integer arithmetic, branches, loads/stores, calls,
  exceptions and code invalidation;
- compare it with Kyty's interpreter on the same instruction traces;
- document unsupported PS5 requirements before deciding whether to adopt it.

Exit gate:
- no direct native call of a PS5 x86-64 entry point on ARM64;
- memory faults and self-modifying code invalidation are observable by the execution backend;
- the chosen backend passes deterministic CPU tests before title boot is attempted.

## Phase 4 — Connect the GPU to the same memory authority

**Goal:** preserve PS5 CPU/GPU shared-memory semantics after host virtual addresses are abstracted.

Tasks:
- define how GPU virtual ranges resolve to backing ID + offset;
- retain existing GPU dirty-page tracking and buffer-cache invalidation initially;
- add alias tests in which CPU writes are visible to GPU uploads and GPU writes are visible after
  readback;
- validate command buffers and asynchronous readbacks across map/unmap/protect operations;
- only add direct host views or import/export optimisations after measuring the copy-based path.

Exit gate:
- CPU/GPU visibility tests pass for normal memory, direct memory and aliases;
- no stale GPU buffer-device-address entry survives an unmap or backing replacement.

## Phase 5 — Audit the KosmicKrisp patches

**Goal:** select driver fixes based on the actual Kyty shader failures and exact Mesa source base.

Order:
1. clip/cull-distance scalarisation (Hadron 0039);
2. 64-bit atomic lock lowering (Hadron 0079);
3. depth-bounds emulation for pre-family-10 Apple GPUs (Hadron 0085).

For each patch:
- compare Hadron's pinned Mesa commit with Kyty's checked-out Mesa/KosmicKrisp revision;
- inspect upstream changes since the patch's base;
- port only the minimal semantic change;
- add a small reproducer and the relevant Vulkan CTS tests;
- test performance and correctness on M4;
- keep the patch disabled or unadvertised if the driver does not fully implement the feature.

Exit gate:
- each patch has a dedicated test, before/after result and a clear supported-feature decision.

## Phase 6 — Full title validation

Only after phases 1–5:
- run the focused memory, CPU, graphics and Vulkan tests;
- run Kyty's existing CTest suite;
- boot GTA V and capture the first failure with guest address, access type, backing ID/offset,
  instruction address, thread and GPU submission context;
- compare native ARM64 results with the existing x86-64/Rosetta baseline where available.

Do not treat a later GTA V boot point as proof of correctness if memory, CPU or graphics tests regress.

## First practical milestone

The next implementation change should be **Phase 0 diagnostics plus Phase 1 alias tests**. It is
the smallest step that tells us whether the current memory abstraction is sufficient and gives us
a reliable base for later CPU and GPU integration.
