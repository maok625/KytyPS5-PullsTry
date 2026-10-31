# Hadron-derived architecture scaffold for KytyPS5

## Status and intent

This document is an implementation scaffold, not a claim that native ARM64 execution or
translated guest memory is already integrated. It is based on the current
`exp/guest-memory-translation` experiment and a review of Hadron's public architecture,
macOS findings, FEX patches and KosmicKrisp patch queue.

The work is deliberately isolated from `rever4`. The current
`GuestMemoryMap` remains a small proof that guest addresses can be identifiers rather than
host pointers. It is not the production memory manager.

## Design rules

1. A PS5 guest virtual address is a number in the guest address space. It must not be assumed to
   be a valid macOS pointer.
2. Physical backing is owned separately from virtual mappings. Multiple guest virtual aliases
   can refer to the same backing object and offset.
3. macOS host page size and PS5 guest page size are separate properties. Query the host page size
   at runtime; obtain guest page granularity from Kyty's existing PS5 memory contract. Do not
   substitute the 2 MiB `PAGE_TABLE_GRANULARITY` accounting unit for either page size.
4. Fixed-address host mappings are an optional fast path, never the correctness foundation of
   native ARM64 mode.
5. CPU, kernel and GPU accesses must resolve through one memory authority. A CPU-only translation
   layer that leaves GPU direct dereferences unchanged is not a complete solution.
6. Every optimisation must preserve aliasing, protection, invalidation and GPU/CPU coherency
   semantics. Start with a correct slow path, then measure.
7. Do not copy Wine/FEX patches line-for-line. They solve Windows/Wine contracts; reuse the
   measured macOS behaviour and test ideas, then implement Kyty/PS5 semantics.

## What the current source tells us

- `src/kernel/memoryAddressSpace.inc` already has a separate shared backing object
  (`GuestBackingStore`) and tracks guest ranges with backing offsets. On macOS it creates a
  shared-memory file and a writable backing view, but its view-mapping path still attempts to
  map backing at the guest numeric address.
- `src/kernel/memory.cpp` owns the public virtual-range model, protection flags, memory types,
  CPU/GPU page accounting and the bridge to the renderer.
- `src/graphics/guest_gpu/graphicsRun.cpp` and
  `src/graphics/host_gpu/renderer/cache/bufferCache.cpp` are examples of the GPU-facing side
  that must continue to agree with kernel memory mappings and dirty-page tracking.
- `src/loader/runtimeLinker.cpp` still contains direct guest-address pointer use, including
  relocation writes. Its entry path also ultimately assumes guest code can be called as a
  native function. Address translation alone does not make x86-64 guest code executable on ARM64.
- `src/kernel/guestMemoryMap.{h,cpp}` is intentionally simpler: independent byte vectors,
  explicit Read/Write, no physical aliasing, permissions, GPU integration or guest execution.

## Proposed layers

```text
       PS5 guest CPU / loader / kernel APIs / GPU command processor
                              |
                   Guest access facade
             Read / Write / Fetch / Protect / Query
                              |
                    GuestAddressSpace
              guest VA -> backing ID + offset
                 + permissions + generation
                              |
               Physical Backing Store / Pages
               stable objects, offsets, aliasing
                              |
                macOS host-memory backend
           anonymous/shared backing, optional views
                              |
                    macOS virtual memory
```

### A. Host-memory backend

Responsibilities:
- allocate and release host-owned backing objects;
- expose backing reads/writes and, later, scoped host views where a consumer genuinely needs
  direct access;
- query host page size and supported mapping/protection capabilities;
- create optional aliases without requiring the alias address to equal a guest address;
- report allocation, mapping and protection failures with enough detail to diagnose them.

Non-goals:
- do not reserve the whole PS5 guest address range at its requested numeric address;
- do not assume that a successful address hint was honoured;
- do not silently replace an occupied mapping;
- do not silently emulate unsupported CPU/GPU permissions by granting broader host permissions.

### B. Physical backing store

Use stable opaque backing identifiers plus offsets. The same backing page can be mapped at more
than one guest virtual address. Direct memory, shared memory and ordinary committed memory must
be represented without making their host pointer their identity.

The existing `GuestBackingStore` is a useful starting point because it already owns shared
backing and tracks `backing_offset`. The refactor should separate its backing-object lifetime
from its optional host views and from the guest virtual-range metadata.

### C. Guest address space

Own reservations, mappings, aliases, permissions and map generations. A mapping records at least:

- guest virtual base and size;
- backing identifier and backing offset (or an explicit unbacked/reserved state);
- guest-visible memory type;
- CPU read/write/execute and GPU read/write permissions;
- mapping generation or equivalent invalidation token.

A translation returns the first contiguous backing span only. Callers loop when a request crosses
a mapping boundary. Aliasing is represented by mapping the same backing identifier/offset at
multiple guest virtual addresses.

The interfaces and initial reference implementation are in
`src/kernel/guest_memory/guestMemoryAddressSpace.h` and
`src/kernel/guest_memory/guestMemoryAddressSpace.cpp`. `GuestMemoryModel` stores bytes in
host-owned vectors and uses guest addresses only as map keys. It supports shared backing aliases,
partial unmap/protect, permissions and map generations. It is deliberately a correctness-first
test model, not a performant physical-memory backend or a drop-in replacement for
`GuestBackingStore`. The guest page size is supplied explicitly by the caller; it is not
automatically set to the host page size.

### D. Guest access facade and consumers

Migrate consumers in this order, with compatibility wrappers where useful:

1. loader relocation and ELF/image writes;
2. CPU data reads/writes and instruction fetches;
3. kernel memory APIs, protection, reserve/commit/decommit and direct-memory aliases;
4. GPU command and buffer-cache resolution;
5. asynchronous readback, dirty tracking and invalidation.

Do not switch a consumer to translated memory until its boundary, permissions, aliasing and
failure behaviour have tests. Preserve the existing GPU page-table and dirty-page model until a
replacement is proven equivalent.

### E. Native ARM64 CPU execution

Keep CPU execution behind a backend boundary. The backend must consume an explicit guest CPU
context and the guest-memory access facade; it must not depend on the host process executing an
x86-64 function pointer.

Evaluate FEXCore as a candidate translation engine, not as an already-integrated dependency.
Hadron's FEX changes adapt FEX to Wine's ARM64EC/WoW64 DLL and UnixLib interfaces, including
macOS MAP_JIT support. Those Wine adapters are not reusable as Kyty's PS5 CPU interface.
Before choosing FEXCore, prove that its public/core API can support Kyty's required PS5 ABI,
exception/interrupt boundaries, guest memory callbacks, self-modifying code invalidation and
JIT cache lifetime. Keep the small interpreter as a reference/test backend, not as an assumption
that its current opcode subset can boot a commercial game.

### F. KosmicKrisp and Vulkan

Keep the Vulkan driver boundary separate from guest address translation. The renderer should
receive validated backing/range information and retain its existing cache-coherency rules.
Driver fixes should be audited against the exact Mesa/KosmicKrisp source revision in the Kyty
branch before being ported.

The first Hadron patches to investigate are tracked in
`docs/kosmickrisp-hadron-patch-audit.md`.

## macOS-specific lessons to validate on the M4

Hadron measured macOS 27 behaviour on an M2 Pro. Its report says that some fixed-address mappings
between 4 GiB and `0x7000000000` are refused or relocated, and that 16 KiB host pages complicate
software expecting smaller guest pages. Our GTA-related test address `0x15b1400000` lies in
that numeric range. This is strong supporting evidence for the direction of our investigation,
not proof that every M4/macOS build behaves identically.

`tests/macosGuestAddressProbe.cpp` is the first diagnostic probe. It records:
- process architecture and macOS kernel release;
- host page size from sysctl and Mach;
- the region state around `0x15b1400000`;
- the result of a fixed Mach VM allocation at that address from a child process.

The allocation uses `VM_FLAGS_FIXED` without `VM_FLAGS_OVERWRITE`; XNU documents the latter as
the flag that permits replacing existing mappings. Successful allocations are deallocated before
the child exits. The probe does not use `MAP_FIXED` and does not alter Kyty's allocator.

It only classifies one exact mapping request. Run additional probes only when needed, keep them
in child processes, and deallocate every successful mapping.

## Explicitly out of scope for this scaffold

- replacing Kyty's live memory allocator;
- changing the GTA V execution path;
- importing Hadron's Wine/FEX patch series wholesale;
- enabling new Vulkan features without matching device/format/feature tests;
- changing Windows or Linux behaviour as part of the native macOS experiment.
