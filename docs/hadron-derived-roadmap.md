# Hadron-derived Kyty implementation roadmap

This roadmap turns the architecture scaffold into small, testable changes. Complete each gate
before starting the next one. Keep the work on `exp/hadron-architecture-scaffold` until the
M4 tests and source review justify a pull request.

## Phase 0 — Establish the exact baseline

**Goal:** understand the M4 host and the current Kyty assumptions before changing them.

Tasks:
- record macOS version, `uname -m`, `sysctl -n hw.pagesize` and Kyty build architecture;
- add diagnostic logging around the existing macOS backing allocation and fixed-view mapping;
- log requested guest address, backing offset, size, returned host address and native error;
- reproduce the `0x15b1400000` reservation failure in an isolated process, without overwriting
  existing mappings;
- capture the exact `rever4`/experiment commit and build configuration used for every test.

Exit gate:
- the same operation is classified as success, relocation, or a specific host mapping failure;
- a failed probe leaves no mappings behind and does not alter normal Kyty execution.

## Phase 1 — Complete the memory model outside the live emulator

**Goal:** prove that guest virtual memory can be represented independently of host virtual
addresses, including aliases.

Implement behind the contract in
`src/kernel/guest_memory/guestMemoryAddressSpace.h`:

- backing objects with opaque IDs and offset-based read/write;
- guest reservations distinct from committed mappings;
- map, unmap, protect and translation;
- two guest virtual aliases that point to the same backing bytes;
- map generation/invalidation for consumers holding translations;
- a slow explicit-copy path as the reference implementation.

Required tests:
- map/read/write/unmap and zero-fill;
- alias coherence: write through alias A, read the changed bytes through alias B;
- offsets and ranges crossing mapping boundaries;
- all-or-nothing failed writes;
- overlapping reservations and mappings;
- permission rejection for CPU read/write/execute and GPU access;
- unmap/protect while a translation exists (generation must make stale translations detectable);
- allocation failure and integer-overflow handling.

Exit gate:
- tests pass with sanitizers on a development host and on the user's M4;
- no test requires a guest virtual address to be a valid host pointer.

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
