#include "trace_writer.h"

#include <fstream>
#include <memory>
#include <cstring>

#include "cpu_context.h"
#include "machine_description.h"

#include "util/log.h"

namespace reven {
namespace tracer {

namespace {

static uint32_t no_action_eflags_bits;
static CpuContext comparison_ctx;

template <typename T>
inline void write_reg(InitialRegistersSectionWriter& writer, x86Register reg, std::uint64_t size, const T& value)
{
	if (size > sizeof(T)) {
		LOG_ERROR("reg " << reg_name(reg) << "'s size " << std::dec << size << " is > of passed value type " << std::dec << sizeof(T))
	}
	writer.write(reg_id(reg), reinterpret_cast<const std::uint8_t*>(&value), size);
}

template <typename T>
inline void write_reg(EventsSectionWriter& writer, x86Register reg, std::uint64_t size, const T& value, T& comparison_value)
{
	if (size > sizeof(T)) {
		LOG_ERROR("reg " << reg_name(reg) << "'s size " << std::dec << size << " is > of passed value type " << std::dec << sizeof(T))
	}

	if (value == comparison_value)
		return;
	comparison_value = value;

	writer.write_register(reg_id(reg), reinterpret_cast<const std::uint8_t*>(&value), size);
}

template <>
inline void write_reg<uint80_t>(EventsSectionWriter& writer, x86Register reg, std::uint64_t size, const uint80_t& value, uint80_t& comparison_value)
{
	if (size > sizeof(uint80_t)) {
		LOG_ERROR("reg " << reg_name(reg) << "'s size " << std::dec << size << " is > of passed value type " << std::dec << sizeof(uint80_t))
	}

	if (std::memcmp(&value, &comparison_value, sizeof(uint80_t)) == 0)
		return;
	std::memcpy(&comparison_value, &value, sizeof(uint80_t));

	writer.write_register(reg_id(reg), reinterpret_cast<const std::uint8_t*>(&value), size);
}

template <>
inline void write_reg<ZmmRegister>(EventsSectionWriter& writer, x86Register reg, std::uint64_t size, const ZmmRegister& value, ZmmRegister& comparison_value)
{
	if (size > sizeof(ZmmRegister)) {
		LOG_ERROR("reg " << reg_name(reg) << "'s size " << std::dec << size << " is > of passed value type " << std::dec << sizeof(ZmmRegister))
	}

	if (std::memcmp(&value, &comparison_value, sizeof(ZmmRegister)) == 0)
		return;
	std::memcpy(&comparison_value, &value, sizeof(ZmmRegister));

	writer.write_register(reg_id(reg), reinterpret_cast<const std::uint8_t*>(&value), size);
}

}

BochsWriter::BochsWriter(const std::string& filename, const MachineDescription& desc,
                         const char* tool_name, const char* tool_version, const char* tool_info)
  : TraceWriter(
    	std::make_unique<std::ofstream>(filename, std::ios::binary), desc,
    	tool_name, tool_version, tool_info
    )
{
}

void save_initial_cpu_context(CpuContext* ctx, InitialRegistersSectionWriter& writer)
{
	comparison_ctx = *ctx;

	no_action_eflags_bits = 0xffffffff;
	for (auto bit : eflags_bits) {
		no_action_eflags_bits ^= 1u << bit;
	}

	#define REGISTER_ACTION(register, size, var) \
	write_reg(writer, r_##register, size, ctx->var);
	#define REGISTER_CTX(register, size, var) \
	write_reg(writer, r_##register, size, ctx->var);
	#define REGISTER_MSR(register, index) \
	write_reg(writer, r_##register, 8, ctx->msrs[msr_##register]);
	#include "registers.inc"
	#undef REGISTER_MSR
	#undef REGISTER_CTX
	#undef REGISTER_ACTION
}

void save_cpu_context(CpuContext* ctx, EventsSectionWriter& writer)
{
	if ((ctx->regs[REG_RIP] - comparison_ctx.regs[REG_RIP]) <= 15 and ctx->regs[REG_RIP] != comparison_ctx.regs[REG_RIP]) {
		writer.write_register_action(
		  register_action_ids.at(RegisterOperationRipPlus1 - 1 + (ctx->regs[REG_RIP] - comparison_ctx.regs[REG_RIP])));
	} else {
		write_reg(writer, r_rip, 8, ctx->regs[REG_RIP], comparison_ctx.regs[REG_RIP]);
	}
	comparison_ctx.regs[REG_RIP] = ctx->regs[REG_RIP];

	const long int rsp_diff = ctx->regs[REG_RSP] - comparison_ctx.regs[REG_RSP];
	switch(rsp_diff) {
		case 0: break;
		case 2: writer.write_register_action(register_action_ids.at(RegisterOperationRspPlus2)); break;
		case 4: writer.write_register_action(register_action_ids.at(RegisterOperationRspPlus4)); break;
		case 8: writer.write_register_action(register_action_ids.at(RegisterOperationRspPlus8)); break;
		case 16: writer.write_register_action(register_action_ids.at(RegisterOperationRspPlus16)); break;
		case -2: writer.write_register_action(register_action_ids.at(RegisterOperationRspMinus2)); break;
		case -4: writer.write_register_action(register_action_ids.at(RegisterOperationRspMinus4)); break;
		case -8: writer.write_register_action(register_action_ids.at(RegisterOperationRspMinus8)); break;
		case -16: writer.write_register_action(register_action_ids.at(RegisterOperationRspMinus16)); break;
		default:
			write_reg(writer, r_rsp, 8, ctx->regs[REG_RSP], comparison_ctx.regs[REG_RSP]);
	}
	comparison_ctx.regs[REG_RSP] = ctx->regs[REG_RSP];

	// Eflags needs special recomputation
	if (ctx->eflags != comparison_ctx.eflags) {
		if ((ctx->eflags & no_action_eflags_bits) != (comparison_ctx.eflags & no_action_eflags_bits)) {
			write_reg(writer, r_eflags, 4, ctx->eflags, comparison_ctx.eflags);
		} else {
			int counter = 0;
			for (std::size_t i = 0; i < eflags_bits.size(); ++i) {
				std::uint32_t mask = 1u << eflags_bits[i];
				if ((ctx->eflags & mask) != (comparison_ctx.eflags & mask))
					counter++;
			}
			if (counter < 5) {
				for (std::size_t i = 0; i < eflags_bits.size(); ++i) {
					std::uint32_t mask = 1u << eflags_bits[i];

					if ((ctx->eflags & mask) != (comparison_ctx.eflags & mask)) {
						std::uint8_t action_base = ctx->eflags & mask ? RegisterOperationFlagSetCf : RegisterOperationFlagUnsetCf;
						writer.write_register_action(register_action_ids.at(action_base + i));
					}
				}
			} else {
				write_reg(writer, r_eflags, 4, ctx->eflags, comparison_ctx.eflags);
			}
		}

		comparison_ctx.eflags = ctx->eflags;
	}

	#define REGISTER_ACTION(register, size, var)
	#define REGISTER_CTX(register, size, var) \
	write_reg(writer, r_##register, size, ctx->var, comparison_ctx.var);
	#define REGISTER_MSR(register, index) \
	write_reg(writer, r_##register, 8, ctx->msrs[msr_##register], comparison_ctx.msrs[msr_##register]);
	#include "registers.inc"
	#undef REGISTER_MSR
	#undef REGISTER_CTX
	#undef REGISTER_ACTION
}

}
}
