#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kyty::NativeArm64 {

// Indexes into GuestCpuState::gpr. Keep the order explicit; translator backends
// must not assume that an x86 register index equals an AArch64 register index.
enum class GuestGpr : std::size_t {
	Rax = 0,
	Rcx,
	Rdx,
	Rbx,
	Rsp,
	Rbp,
	Rsi,
	Rdi,
	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15,
};

inline constexpr std::size_t kGuestGprCount = 16;
inline constexpr std::size_t kGuestVectorRegisterCount = 16;
inline constexpr std::size_t kGuestX87RegisterCount = 8;

// Storage for an x86 YMM register (low 128 bits are the corresponding XMM
// register; high 128 bits are AVX state). The representation is byte/state
// storage, not a promise that the host has AVX or AVX2 instructions.
struct alignas(32) GuestYmmRegister {
	std::array<std::uint64_t, 4> qword{};
};

// x87 values are 80-bit extended precision. Keep 16 bytes per register so the
// guest representation can preserve the payload and reserved/padding bytes
// without relying on the host's long-double format.
struct alignas(16) GuestX87Register {
	std::array<std::byte, 16> bytes{};
};

// Architecture-neutral state passed between the emulator and a future
// x86-64-to-AArch64 translator. This is only a starting point: exception,
// debug, segment-selector, and other architectural state will need to be
// modelled where instruction semantics or guest ABI require it.
struct alignas(32) GuestCpuState {
	// Conventional x86-64 GPR order, indexed by GuestGpr.
	std::array<std::uint64_t, kGuestGprCount> gpr{};

	std::uint64_t rip = 0;
	std::uint64_t rflags = 1ULL << 1; // Reserved bit 1 is set.

	// FS/GS bases are required for x86-64 thread-local storage and FSGSBASE
	// semantics. Segment selectors and privilege state are handled separately.
	std::uint64_t fs_base = 0;
	std::uint64_t gs_base = 0;

	std::array<GuestYmmRegister, kGuestVectorRegisterCount> ymm{};
	std::array<GuestX87Register, kGuestX87RegisterCount> x87{};

	std::uint16_t x87_control_word = 0x037f;
	std::uint16_t x87_status_word = 0;
	std::uint16_t x87_tag_word = 0xffff;
	std::uint16_t reserved = 0;

	std::uint32_t mxcsr = 0x1f80;
	std::uint32_t mxcsr_mask = 0xffff;
};

static_assert(sizeof(GuestYmmRegister) == 32);
static_assert(alignof(GuestYmmRegister) == 32);
static_assert(sizeof(GuestX87Register) == 16);
static_assert(std::is_standard_layout_v<GuestCpuState>);
static_assert(alignof(GuestCpuState) == 32);

} // namespace Kyty::NativeArm64
