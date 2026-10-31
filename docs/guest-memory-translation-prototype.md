# Guest memory translation prototype

## Purpose

This is an isolated experiment for separating a PS5 guest virtual address from the address where its bytes are stored by the host operating system.

For example, a PS5 title may request guest address `0x15B1400000`. The prototype records that number as a guest address and allocates the actual bytes separately using ordinary C++ host memory. It does not ask macOS to map anything at `0x15B1400000`.

## What is included

`Libs::LibKernel::Memory::GuestMemoryMap` currently supports:

- mapping and unmapping page-aligned guest ranges;
- zero-initialized host backing;
- reading and writing by guest address;
- accesses that cross 16 KiB page boundaries;
- accesses that cross two adjacent guest mappings;
- rejection of overlaps, invalid ranges and unmapped accesses;
- all-or-nothing validation for writes, so a write that reaches an unmapped gap does not partially modify an earlier mapped range.

The guest address is used only as a key in a map. The implementation does not convert it into a host pointer.

## Build and run the focused test

From a configured Kyty build directory:

```sh
cmake --build _Build/macos --target guest_memory_map_tests --parallel
ctest --test-dir _Build/macos -R '^guest_memory_map$' --output-on-failure
```

Use the name of your existing build directory if it differs from `_Build/macos`.

The test uses the GTA V-related guest address `0x15B1400000` as a simulated address. It does not load GTA V or access game files.

## Deliberate limitations

This is **not yet a replacement** for Kyty's existing `GuestAddressSpace` or direct-memory implementation.

It does not currently model:

- PS5 physical direct memory or multiple virtual aliases of the same physical pages;
- page permissions, guard pages, guest page faults or protection changes;
- memory-pool reservations, flexible memory or automatic memory accounting;
- GPU page tracking, Vulkan buffer addresses, cache invalidation or GPU readback;
- code execution directly from guest memory.

Reads and writes go through explicit `Read` and `Write` calls. That interface is suitable for an interpreter prototype, but existing Kyty code often dereferences guest addresses as ordinary pointers. Those call sites cannot be made compatible just by adding this class.

## Next engineering steps

1. Validate this isolated test on the user's Mac.
2. Extend the design to represent physical backing separately from guest virtual mappings, so aliases can share the same bytes.
3. Define how CPU instruction fetches, guest reads/writes and kernel operations use the same address-translation service.
4. Design a GPU-facing memory interface that preserves Kyty's existing tracking and cache-coherency rules.
5. Only then plan integration with the emulator's live memory system.
