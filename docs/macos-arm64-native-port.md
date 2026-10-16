# Native Apple Silicon (arm64) port

**Status: initial interpreter subset implemented — still not connected to the PS5 loader/runtime, and not yet capable of running games natively.**

## Goal

Build KytyPS5 itself as an arm64 macOS process and execute the PS5's x86-64 guest code without Rosetta 2. Keep Windows, Linux, and the existing x86-64/Rosetta macOS path unchanged while the native backend is developed.

## Important separation of responsibilities

- **Guest CPU translation:** required to execute PS5 x86-64 instructions on an arm64 host. This needs an x86-64 decoder, an AArch64 code emitter or interpreter, correct guest-register/flags/SIMD state, code-cache invalidation, guest memory fault handling, and explicit exits for syscalls and exceptions.
- **Host ABI and guest address space:** the current macOS x86-64 path relies on x86-64 guest-address-space linker layout and emits x86-64 trampolines/PLT stubs. These assumptions must be separated from the arm64 host path; changing only the build architecture is not enough.
- **Graphics:** [KosmicKrisp](https://www.lunarg.com/products/kosmickrisp/) is LunarG's Mesa Vulkan driver for Apple Silicon; it maps Vulkan calls to Metal 4 and is documented for macOS 26+. It is a possible **alternative host Vulkan ICD** to the current MoltenVK path, not a layer that translates CPU instructions. The [crueter-ci/KosmicKrisp repository](https://github.com/crueter-ci/KosmicKrisp) contains build/package scripts for related macOS graphics components. KosmicKrisp does **not** translate x86-64 CPU instructions to AArch64.
- **Rosetta 2:** must not be a runtime dependency of the native arm64 build. The existing x86-64 build remains available as a fallback during development.

## Initial code boundary

`src/native_arm64/guest_cpu_state.h` defines the guest architectural state. `src/native_arm64/translator_interface.h` defines guest memory and the execution-backend contract. `src/native_arm64/x64_interpreter.cpp` now implements a first, deliberately limited interpreter that executes as ordinary C++ on the host; it does not emit or execute x86-64 host machine code, so this backend itself does not need Rosetta 2 or JIT executable memory.

**Supported bring-up subset:** NOP; MOV register/register, register/immediate, register/memory, C6/C7 immediate and byte-width forms; MOVZX/MOVSX from 8/16-bit sources to 32/64-bit destinations; LEA; byte, 16-bit, and 32/64-bit ADD/OR/AND/SUB/XOR/CMP/TEST forms; 16-bit MOV and immediate ALU operations; memory and immediate forms for ADD/OR/AND/SUB/XOR/CMP; direct short/near and indirect register/memory JMP; short and near conditional branches; direct and indirect CALL; PUSH/POP of 64-bit general-purpose registers and memory operands; PUSH imm8/imm32; INC/DEC r/m8/r/m32/r/m64; TEST/NOT/NEG r/m8/r/m32/r/m64; LEAVE; RET/RET imm16; and explicit SYSCALL / UD2 exits. Legacy AH/CH/DH/BH and REX byte-register encodings are tested separately. The effective-address decoder handles ModRM/SIB, base/index/scale, signed displacements, RIP-relative addressing, and FS/GS segment bases. Writes to 32-bit registers zero-extend as on x86-64. A block stops at control-flow boundaries, syscalls, unsupported instructions, guest exceptions, or memory faults, and the reported guest RIP remains at an unsupported/faulting instruction.

**Important limitations:** this is still a small interpreter subset and is not connected to `RuntimeLinker` or Kyty's kernel syscall dispatcher. SYSCALL currently exits to the caller with RCX/R11 architectural effects; it does not perform a PS5 system call. It rejects unimplemented memory-width/prefix forms (including address-size override and most operand-size-prefixed instructions), most instructions, SSE/AVX, x87, atomic operations, and general exception recovery. Some x86 architectural edge cases are not modeled yet. The interpreter has no translated-code cache, and its `InvalidateCodeRange` method is intentionally a no-op. `RuntimeLinker::RunEntry` now refuses to call x86-64 guest entry bytes directly on an ARM64 host; this is a safety guard, not a functioning runtime bridge. Host-import calls, guest/host ABI marshalling, syscall dispatch, and the broader instruction set are still required before gameplay is possible.

The guest state model includes the 16 x86-64 general-purpose registers, RIP/RFLAGS, FS/GS bases, 16 256-bit YMM registers (AVX/AVX2), x87 register storage, and MXCSR/control state. It remains a starting contract: signal/context integration, x87/MMX aliasing, precise exceptions, and additional architectural state need explicit handling.

## Work plan and acceptance criteria

1. **Native build plumbing:** make the emulator compile as arm64 without changing the current x86-64 build. Inventory all x86-only assembly, code emitters, linker flags, guest-memory assumptions, and executable-memory requirements.
2. **Guest CPU backend:** expand the interpreter with complete memory addressing and conditional control flow, then build a tested AArch64 DBT path. Preserve x86-64 integer semantics and flags, RIP-relative addressing, SSE/AVX/AVX2 behavior needed by PS5 titles, and guest exception/fault semantics. The PS5 CPU is Zen 2 class; do not assume AVX-512 is available.
3. **Kernel/loader integration:** route guest code entry points, calls/returns, syscalls, signals, faults, and code writes through the backend. Invalidate translated blocks when executable guest memory changes.
4. **Native memory model:** separate PS5 guest virtual addresses from host pointers and remove the macOS x86-64-only `-segaddr` / `-image_base` strategy from the arm64 path.
5. **Graphics:** evaluate KosmicKrisp as an alternative Vulkan ICD to MoltenVK through the Vulkan loader. Compare the Vulkan extensions, formats, synchronization behavior, and limits Kyty actually requires; keep a proven fallback. This work must remain independent from the CPU translator.
6. **CI and regression testing:** build the native binary on an Apple Silicon runner and run state, instruction, memory, syscall, exception, and existing emulator tests. Keep a separate x86-64 macOS/Rosetta job until parity is demonstrated.

## Build and run the interpreter smoke test

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

The workflow has a fast interpreter-test job and a separate full-emulator ARM64 build job. The interpreter target verifies only this instruction subset; a successful full build would still not mean the emulator can boot games until the runtime-linker, host-import ABI bridge, and syscall dispatcher are connected.
