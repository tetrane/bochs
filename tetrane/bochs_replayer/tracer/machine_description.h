#pragma once

#include <cstdint>

#include "bochs.h"
#include "cpu/cpu.h"

#include <rvnbintrace/trace_sections.h>

using namespace reven::backend::plugins::file::libbintrace;

namespace reven {
namespace tracer {

#define REGISTER_ACTION(register, size, ctx) r_##register,
#define REGISTER_CTX(register, size, ctx) r_##register,
#define REGISTER_MSR(register, index) r_##register,
enum x86Register {
	#include "registers.inc"
	register_enum_count
};
#undef REGISTER_MSR
#undef REGISTER_CTX
#undef REGISTER_ACTION

#define REGISTER_ACTION(register, size, ctx)
#define REGISTER_CTX(register, size, ctx)
#define REGISTER_MSR(register, index) msr_##register,
enum x86MSR {
	#include "registers.inc"
	msr_enum_count
};
#undef REGISTER_MSR
#undef REGISTER_CTX
#undef REGISTER_ACTION

const std::string& exception_event_description(int32_t vector, uint32_t error_code);

void initialize_register_maps();
RegisterId reg_id(x86Register reg_name);
std::uint16_t reg_size(x86Register reg_name);
const std::string& reg_name(x86Register reg_name);

enum RegisterOperationId {
	RegisterOperationRipPlus1,
	RegisterOperationRipPlus2,
	RegisterOperationRipPlus3,
	RegisterOperationRipPlus4,
	RegisterOperationRipPlus5,
	RegisterOperationRipPlus6,
	RegisterOperationRipPlus7,
	RegisterOperationRipPlus8,
	RegisterOperationRipPlus9,
	RegisterOperationRipPlus10,
	RegisterOperationRipPlus11,
	RegisterOperationRipPlus12,
	RegisterOperationRipPlus13,
	RegisterOperationRipPlus14,
	RegisterOperationRipPlus15,

	RegisterOperationFlagSetCf,
	RegisterOperationFlagSetPf,
	RegisterOperationFlagSetAf,
	RegisterOperationFlagSetZf,
	RegisterOperationFlagSetSf,
	RegisterOperationFlagSetTf,
	RegisterOperationFlagSetIf,
	RegisterOperationFlagSetDf,
	RegisterOperationFlagSetOf,

	RegisterOperationFlagUnsetCf,
	RegisterOperationFlagUnsetPf,
	RegisterOperationFlagUnsetAf,
	RegisterOperationFlagUnsetZf,
	RegisterOperationFlagUnsetSf,
	RegisterOperationFlagUnsetTf,
	RegisterOperationFlagUnsetIf,
	RegisterOperationFlagUnsetDf,
	RegisterOperationFlagUnsetOf,

	RegisterOperationRspPlus2,
	RegisterOperationRspPlus4,
	RegisterOperationRspPlus8,
	RegisterOperationRspPlus16,
	RegisterOperationRspMinus2,
	RegisterOperationRspMinus4,
	RegisterOperationRspMinus8,
	RegisterOperationRspMinus16,

	RegisterOperationIdLast
};

const std::vector<std::uint8_t> eflags_bits = { 0, 2, 4, 6, 7, 8, 9, 10, 11 };
extern std::vector<RegisterId> register_action_ids;

}
}
