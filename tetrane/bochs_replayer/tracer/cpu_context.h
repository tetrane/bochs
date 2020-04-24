#pragma once

#include <cstdint>

#include "machine_description.h"

namespace reven {
namespace tracer {

enum Reg64 {
	REG_RAX = 0,
	REG_RCX,
	REG_RDX,
	REG_RBX,
	REG_RSP,
	REG_RBP,
	REG_RSI,
	REG_RDI,
	REG_R8,
	REG_R9,
	REG_R10,
	REG_R11,
	REG_R12,
	REG_R13,
	REG_R14,
	REG_R15,
	REG_RIP,
	REG_COUNT
};

enum SegReg {
	SEG_REG_ES = 0,
	SEG_REG_CS = 1,
	SEG_REG_SS = 2,
	SEG_REG_DS = 3,
	SEG_REG_FS = 4,
	SEG_REG_GS = 5,
	SEG_REG_COUNT
};

typedef struct uint80 {
	std::uint8_t value[10];
} uint80_t;
static_assert(sizeof(uint80_t) == 10, "uint80_t must be of size 10");

typedef union ZmmRegister {
   std::uint64_t  zmm_u64[8];
} ZmmRegister;

struct CpuContext {
	std::uint64_t regs[REG_COUNT];

	std::uint32_t eflags;

	std::uint16_t seg_regs[SEG_REG_COUNT];
	std::uint64_t seg_regs_shadow[SEG_REG_COUNT];

	std::uint32_t pkru;

	struct {
		std::uint64_t base;
		std::uint16_t limit;
	} gdtr;

	struct {
		std::uint64_t base;
		std::uint32_t limit;
	} ldtr;

	struct {
		std::uint64_t base;
		std::uint32_t limit;
	} idtr;

	struct {
		std::uint64_t base;
		std::uint32_t limit;
	} tr;

	std::uint64_t cr[5]; // 1 isn't used
	std::uint64_t cr8;

	std::uint64_t dr[8]; // 4 and 5 aren't used

	struct {
		uint80_t fpregs[8];

		std::uint64_t fip;
		std::uint64_t fdp;

		std::uint16_t foo;
		std::uint16_t swd;
		std::uint16_t cwd;
		std::uint16_t twd;
	} i387;

	ZmmRegister vmm[32];

	std::uint32_t mxcsr;

	std::uint64_t msrs[msr_enum_count];
};

}
}
