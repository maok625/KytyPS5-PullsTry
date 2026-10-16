#ifndef KYTY_COMMON_ABI_H_
#define KYTY_COMMON_ABI_H_

#include "common/common.h"

// These calling-convention attributes describe x86-64 host/guest boundaries.
// They are meaningful on x86-64; on ARM64 use the native host ABI until an explicit
// guest-to-host marshalling bridge is wired in.
#if defined(__x86_64__) || defined(_M_X64)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI __attribute__((ms_abi))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_MS_ABI
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_SYSV_ABI
#endif

#endif /* KYTY_COMMON_ABI_H_ */
