#include "machine_description.h"

#include <cstdint>

#include "cpu_context.h"

namespace reven {
namespace tracer {

namespace {

static std::vector<RegisterId> register_ids;
static std::vector<std::uint16_t> register_sizes;
static std::vector<std::string> register_names;

}

RegisterId reg_id(x86Register reg_name)
{
	return register_ids.at(reg_name);
}

std::uint16_t reg_size(x86Register reg_name)
{
	return register_sizes.at(reg_name);
}

const std::string& reg_name(x86Register reg_name)
{
	return register_names.at(reg_name);
}

void initialize_register_maps()
{
	RegisterId next_id = 0;

	register_ids.resize(register_enum_count);
	register_sizes.resize(register_enum_count);
	register_names.resize(register_enum_count);

	char msr_name[255];

	#define REGISTER_ACTION(register, size, ctx)  \
	register_sizes[r_##register] = size;        \
	register_ids[r_##register] = next_id++;     \
	register_names[r_##register] = #register;

	#define REGISTER_CTX(register, size, ctx)   \
	register_sizes[r_##register] = size;        \
	register_ids[r_##register] = next_id++;     \
	register_names[r_##register] = #register;

	#define REGISTER_MSR(register, index)		\
	register_sizes[r_##register] = 8;        	\
	register_ids[r_##register] = next_id++;     \
	sprintf(msr_name, "msr_%.8x", (index));		\
	register_names[r_##register] = std::string(msr_name);

	#include "registers.inc"

	#undef REGISTER_MSR
	#undef REGISTER_CTX
	#undef REGISTER_ACTION

	next_id += RegisterOperationIdLast;
}

const std::string& exception_event_description(int32_t vector, uint32_t error_code)
{
	static const std::string string_divide("divide error");
	static const std::string string_debug("debug");
	static const std::string string_nmi("nmi interrupt");
	static const std::string string_breakpoint("breakpoint");
	static const std::string string_overflow("overflow");
	static const std::string string_bound("bound range exceeded");
	static const std::string string_invalid_opcode("invalid opcode");
	static const std::string string_device("device not available");
	static const std::string string_double("double fault with error code " + std::to_string(error_code));
	static const std::string string_coprocessor("coprocessor segment overrun");
	static const std::string string_invalid_tss("invalid tss with error code " + std::to_string(error_code));
	static const std::string string_segment("segment not present with error code " + std::to_string(error_code));
	static const std::string string_stack("stack segment fault with error code " + std::to_string(error_code));
	static const std::string string_general("general protection with error code " + std::to_string(error_code));
	static const std::string string_page("page fault with error code " + std::to_string(error_code));
	static const std::string string_floating("floating-point error");
	static const std::string string_alignment("alignment check with error code " + std::to_string(error_code));
	static const std::string string_machine("machine check");
	static const std::string string_unknown("unknown exception " + std::to_string(vector));

	switch (vector) {
		case 0x0:
			return string_divide;
		case 0x1:
			return string_debug;
		case 0x2:
			return string_nmi;
		case 0x3:
			return string_breakpoint;
		case 0x4:
			return string_overflow;
		case 0x5:
			return string_bound;
		case 0x6:
			return string_invalid_opcode;
		case 0x7:
			return string_device;
		case 0x8:
			return string_double;
		case 0x9:
			return string_coprocessor;
		case 0xA:
			return string_invalid_tss;
		case 0xB:
			return string_segment;
		case 0xC:
			return string_stack;
		case 0xD:
			return string_general;
		case 0xE:
			return string_page;
		case 0x10:
			return string_floating;
		case 0x11:
			return string_alignment;
		case 0x12:
			return string_machine;
	}

	return string_unknown;
}

}
}
