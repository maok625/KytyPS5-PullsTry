# Native Apple Silicon (arm64) port

**Status: early scaffolding only — this branch is not yet capable of running PS5 games natively.**

## Goal

Build KytyPS5 itself as an arm64 macOS process and execute the PS5's x86-64 guest code without Rosetta 2. Keep Windows, Linux, and the existing x86-64/Rosetta macOS path unchanged while the native backend is developed.

## Important separation of responsibilities

- **Guest CPU translation:** required to execute PS5 x86-64 instructions on an arm64 host. This needs an x86-64 decoder, an AArch64 code emitter or interpreter, correct guest-register/flags/SIMD state, code-cache invalidation, guest memory fault handling, and explicit exits for syscalls and exceptions.
- **Host ABI and guest address space:** the current macOS x86-64 path relies on x86-64 guest-address-space linker layout and emits x86-64 trampolines/PLT stubs. These assumptions must be separated from the arm64 host path; changing only the build architecture is not enough.
- **Graphics:** [KosmicKrisp](https://github.com/crueter-ci/KosmicKrisp) is a collection/build workflow for Mesa, MoltenVK, and the Vulkan loader on macOS. It can be investigated for the native Vulkan stack, but it does **not** translate x86-64 CPU instructions to AArch64.
- **Rosetta 2:** must not be a runtime dependency of the native arm64 build. The existing x86-64 build remains available as a fallback during development.

## Initial code boundary

`src/native_arm64/guest_cpu_state.h` defines the guest architectural state needed at the translator boundary. `src/native_arm64/translator_interface.h` defines the future block-execution contract. These files are deliberately not wired into the emulator yet: there is no implementation behind the interface, and routing guest execution through it before it is implemented would be unsafe.

The initial state model covers the 16 x86-64 general-purpose registers, RIP/RFLAGS, FS/GS bases, 16 256-bit YMM registers (AVX/AVX2), x87 register storage, and MXCSR/control state. The model is a starting contract, not a complete CPU implementation; exception details, signal/context integration, x87/MMX aliasing, and additional architectural state still need explicit handling.

## Work plan and acceptance criteria

1. **Native build plumbing:** make the emulator compile as arm64 without changing the current x86-64 build. Inventory all x86-only assembly, code emitters, linker flags, guest-memory assumptions, and executable-memory requirements.
2. **Guest CPU backend:** implement and test an AArch64 DBT/interpreter backend. Preserve x86-64 integer semantics and flags, RIP-relative addressing, SSE/AVX/AVX2 behavior needed by PS5 titles, and guest exception/fault semantics. The PS5 CPU is Zen 2 class; do not assume AVX-512 is available.
3. **Kernel/loader integration:** route guest code entry points, calls/returns, syscalls, signals, faults, and code writes through the backend. Invalidate translated blocks when executable guest memory changes.
4. **Native memory model:** separate PS5 guest virtual addresses from host pointers and remove the macOS x86-64-only `-segaddr` / `-image_base` strategy from the arm64 path.
5. **Graphics:** validate an arm64 Vulkan loader/MoltenVK path first; evaluate KosmicKrisp as an optional way to build/package graphics dependencies. This work must remain independent from the CPU translator.
6. **CI and regression testing:** build the native binary on an Apple Silicon runner and run state, instruction, memory, syscall, exception, and existing emulator tests. Keep a separate x86-64 macOS/Rosetta job until parity is demonstrated.

## Build the state-contract smoke test

On macOS with Xcode Command Line Tools, CMake, Ninja, and the project dependencies installed:

```sh
git submodule update --init --recursive
cmake -S . -B _Build/macos-arm64-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/macos-arm64-native --target native_arm64_state_tests --parallel
ctest --test-dir _Build/macos-arm64-native -R native_arm64_state --output-on-failure
```

The standalone GitHub Actions workflow compiles the same test for the arm64 target. Neither the CMake target nor the workflow builds the complete emulator or validates guest instruction execution.

## Current validation

The accompanying `macos-arm64-scaffold.yml` workflow only checks that the new guest-state boundary can be compiled for the arm64 target. It does **not** mean that the complete KytyPS5 application builds or runs natively yet.
