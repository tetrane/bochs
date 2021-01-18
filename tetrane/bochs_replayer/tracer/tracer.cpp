#include "tracer.h"

#include <cstring>
#include <cstdio>
#include <stdexcept>

#include "bochs.h"
#include "cpu/cpu.h"
#include "iodev/iodev.h"
#include "bxversion.h"

#include "util/log.h"
#include "tetrane/bochs_replayer/icount/fns.h"

extern reven::replayer::Replayer replayer;

namespace {

void end_of_scenario(unsigned cpu, bool desync) {
	replayer.end_of_scenario(cpu, desync);
}

}

namespace reven {
namespace tracer {

std::vector<RegisterId> register_action_ids;

namespace {

template <typename T>
static std::vector<std::uint8_t> value_to_buffer(const T& value)
{
	const auto buffer = reinterpret_cast<const std::uint8_t*>(&value);
	return {buffer, buffer + sizeof(T)};
}

static MachineDescription x64_machine_description(unsigned cpu, const replayer::Replayer& replayer)
{
	MachineDescription desc;

	desc.architecture = MachineDescription::Archi::x64_1;
	desc.physical_address_size = 6;

	RegisterId next_id = 0;

	for (std::size_t i = 0; i < register_enum_count; ++i) {
		auto reg = static_cast<x86Register>(i);
		desc.registers.insert(std::make_pair(next_id++, MachineDescription::Register({reg_size(reg), reg_name(reg)})));
	}

	for (std::uint64_t i = 0; i < 15; ++i) {
		register_action_ids.push_back(next_id);

		desc.register_operations.insert(std::make_pair(
			next_id++,
			MachineDescription::RegisterOperation{
				reg_id(r_rip),
				MachineDescription::RegisterOperator::Add,
				value_to_buffer<std::uint64_t>(i + 1)
			}
		));
	}

	// Eflag bit operations
	for (auto bit : eflags_bits) {
		register_action_ids.push_back(next_id);

		desc.register_operations.insert(std::make_pair(
			next_id++,
			MachineDescription::RegisterOperation{
				reg_id(r_eflags),
				MachineDescription::RegisterOperator::Or,
				value_to_buffer<std::uint32_t>(1u << bit)
			}
		));
	}
	for (auto bit : eflags_bits) {
		register_action_ids.push_back(next_id);

		desc.register_operations.insert(std::make_pair(
			next_id++,
			MachineDescription::RegisterOperation{
				reg_id(r_eflags),
				MachineDescription::RegisterOperator::And,
				value_to_buffer<std::uint32_t>(0xffffffffu ^ (1u << bit))
			}
		));
	}

	// RSP mouvement
	for (std::uint64_t i = 2; i <= 16; i = i * 2) {
		register_action_ids.push_back(next_id);

		desc.register_operations.insert(std::make_pair(
			next_id++,
			MachineDescription::RegisterOperation{
				reg_id(r_rsp),
				MachineDescription::RegisterOperator::Add,
				value_to_buffer<std::uint64_t>(i)
			}
		));
	}
	for (std::uint64_t i = 2; i <= 16; i = i * 2) {
		register_action_ids.push_back(next_id);

		desc.register_operations.insert(std::make_pair(
			next_id++,
			MachineDescription::RegisterOperation{
				reg_id(r_rsp),
				MachineDescription::RegisterOperator::Add,
				value_to_buffer<std::uint64_t>(-i)
			}
		));
	}

	desc.static_registers["cpuid_pat"] = value_to_buffer<std::uint8_t>(BX_CPU(cpu)->is_cpu_extension_supported(BX_ISA_PAT));
	desc.static_registers["cpuid_pse36"] = value_to_buffer<std::uint8_t>(BX_CPU(cpu)->is_cpu_extension_supported(BX_ISA_PSE36));
	desc.static_registers["cpuid_1gb_pages"] = value_to_buffer<std::uint8_t>(BX_CPU(cpu)->is_cpu_extension_supported(BX_ISA_1G_PAGES));

	// Bochs doesn't provide those informations, calculate them from the raw value of the CPUID leaf 0x80000008 of the i7-2600K
	desc.static_registers["cpuid_max_phy_addr"] = value_to_buffer<std::uint8_t>(0x3024 & 0xFF);
	desc.static_registers["cpuid_max_lin_addr"] = value_to_buffer<std::uint8_t>((0x3024 >> 8) & 0xFF);

	desc.memory_regions.push_back({ 0, replayer.get_memory_size() });

	for (const auto& memory_range : replayer.get_memory_ranges()) {
		desc.memory_regions.push_back({memory_range.start_address, memory_range.size});
	}

	return desc;
}

template <std::uint32_t MsrIndex>
static std::uint64_t read_msr(unsigned cpu) {
	switch(MsrIndex) {
		case BX_MSR_APICBASE:
			return BX_CPU(cpu)->msr.apicbase;

		case BX_MSR_SYSENTER_CS:
			return BX_CPU(cpu)->msr.sysenter_cs_msr;
		case BX_MSR_SYSENTER_ESP:
			return BX_CPU(cpu)->msr.sysenter_esp_msr;
		case BX_MSR_SYSENTER_EIP:
			return BX_CPU(cpu)->msr.sysenter_eip_msr;

		case BX_MSR_TSC_DEADLINE:
			return BX_CPU(cpu)->lapic.get_tsc_deadline();

		case BX_MSR_EFER:
			return BX_CPU(cpu)->efer.get32();
		case BX_MSR_STAR:
			return BX_CPU(cpu)->msr.star;
		case BX_MSR_LSTAR:
			return BX_CPU(cpu)->msr.lstar;
		case BX_MSR_CSTAR:
			return BX_CPU(cpu)->msr.cstar;
		case BX_MSR_FMASK:
			return BX_CPU(cpu)->msr.fmask;
		case BX_MSR_FSBASE:
			return BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.base;
		case BX_MSR_GSBASE:
			return BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.base;
		case BX_MSR_KERNELGSBASE:
			return BX_CPU(cpu)->msr.kernelgsbase;
		case BX_MSR_TSC_AUX:
			return BX_CPU(cpu)->msr.tsc_aux;

		case BX_MSR_MTRRCAP:
			return BX_CONST64(0x0000000000000500) | BX_NUM_VARIABLE_RANGE_MTRRS;
		case BX_MSR_MTRRPHYSBASE0:
		case BX_MSR_MTRRPHYSBASE1:
		case BX_MSR_MTRRPHYSBASE2:
		case BX_MSR_MTRRPHYSBASE3:
		case BX_MSR_MTRRPHYSBASE4:
		case BX_MSR_MTRRPHYSBASE5:
		case BX_MSR_MTRRPHYSBASE6:
		case BX_MSR_MTRRPHYSBASE7:
		case BX_MSR_MTRRPHYSMASK0:
		case BX_MSR_MTRRPHYSMASK1:
		case BX_MSR_MTRRPHYSMASK2:
		case BX_MSR_MTRRPHYSMASK3:
		case BX_MSR_MTRRPHYSMASK4:
		case BX_MSR_MTRRPHYSMASK5:
		case BX_MSR_MTRRPHYSMASK6:
		case BX_MSR_MTRRPHYSMASK7:
			return BX_CPU(cpu)->msr.mtrrphys[MsrIndex - BX_MSR_MTRRPHYSBASE0];
		case BX_MSR_MTRRFIX64K_00000:
			return BX_CPU(cpu)->msr.mtrrfix64k.u64;
		case BX_MSR_MTRRFIX16K_80000:
		case BX_MSR_MTRRFIX16K_A0000:
			return BX_CPU(cpu)->msr.mtrrfix16k[MsrIndex - BX_MSR_MTRRFIX16K_80000].u64;
		case BX_MSR_MTRRFIX4K_C0000:
		case BX_MSR_MTRRFIX4K_C8000:
		case BX_MSR_MTRRFIX4K_D0000:
		case BX_MSR_MTRRFIX4K_D8000:
		case BX_MSR_MTRRFIX4K_E0000:
		case BX_MSR_MTRRFIX4K_E8000:
		case BX_MSR_MTRRFIX4K_F0000:
		case BX_MSR_MTRRFIX4K_F8000:
			return BX_CPU(cpu)->msr.mtrrfix4k[MsrIndex - BX_MSR_MTRRFIX4K_C0000].u64;
		case BX_MSR_PAT:
			return BX_CPU(cpu)->msr.ia32_xss;
		case BX_MSR_MTRR_DEFTYPE:
			return BX_CPU(cpu)->msr.pat.u64;
		case BX_MSR_XSS:
			return BX_CPU(cpu)->msr.ia32_xss;
		default:
			break;
	}

	throw std::logic_error("read_msr: Unknown MSR: " + std::to_string(MsrIndex));
	return 0;
}

static reven::tracer::CpuContext create_cpu_context(unsigned cpu) {
	reven::tracer::CpuContext ctx;

	std::memcpy(&ctx.regs, BX_CPU(cpu)->gen_reg, sizeof(ctx.regs));
	ctx.regs[BX_64BIT_REG_RIP] = BX_CPU(cpu)->prev_rip;

	ctx.eflags = BX_CPU(cpu)->read_eflags();

	for (unsigned i = 0; i < reven::tracer::SEG_REG_COUNT; ++i) {
		ctx.seg_regs[i] = BX_CPU(cpu)->sregs[i].selector.value;
	}

	for (unsigned i = 0; i < reven::tracer::SEG_REG_COUNT; ++i) {
		ctx.seg_regs_shadow[i] = BX_CPU(cpu)->sregs[i].selector.value;

		// TODO: Construct it from the cache
		if (BX_CPU(cpu)->sregs[i].selector.ti)
			bx_dbg_read_linear(cpu, BX_CPU(dbg_cpu)->ldtr.cache.u.segment.base + BX_CPU(cpu)->sregs[i].selector.index * 8,8, (Bit8u*)&ctx.seg_regs_shadow[i]);
		else
			bx_dbg_read_linear(cpu, BX_CPU(dbg_cpu)->gdtr.base + BX_CPU(cpu)->sregs[i].selector.index * 8,8, (Bit8u*)&ctx.seg_regs_shadow[i]);
	}

	#if BX_SUPPORT_PKEYS
		ctx.pkru = BX_CPU(cpu)->pkru;
	#else
		#error Must support protection key to work
	#endif

	ctx.gdtr.base = BX_CPU(cpu)->gdtr.base;
	ctx.gdtr.limit = BX_CPU(cpu)->gdtr.limit;

	ctx.ldtr.base = BX_CPU(cpu)->ldtr.cache.u.segment.base;
	ctx.ldtr.limit = BX_CPU(cpu)->ldtr.cache.u.segment.limit_scaled;

	ctx.idtr.base = BX_CPU(cpu)->idtr.base;
	ctx.idtr.limit = BX_CPU(cpu)->idtr.limit;

	ctx.tr.base = BX_CPU(cpu)->tr.cache.u.segment.base;
	ctx.tr.limit = BX_CPU(cpu)->tr.cache.u.segment.limit_scaled;

	ctx.cr[0] = BX_CPU(cpu)->cr0.get32();
	ctx.cr[2] = BX_CPU(cpu)->cr2;
	ctx.cr[3] = BX_CPU(cpu)->cr3;
	ctx.cr[4] = BX_CPU(cpu)->cr4.get32();
	ctx.cr8 = BX_CPU(cpu)->get_cr8();

	for (unsigned i = 0; i < 4; ++i) {
		ctx.dr[i] = BX_CPU(cpu)->dr[i];
	}
	ctx.dr[6] = BX_CPU(cpu)->dr6.get32();
	ctx.dr[7] = BX_CPU(cpu)->dr7.get32();

	for (unsigned i = 0; i < 8; ++i) {
		std::memcpy(&ctx.i387.fpregs[i].value, &BX_CPU(cpu)->the_i387.st_space[i].fraction, sizeof(BX_CPU(cpu)->the_i387.st_space[i].fraction));
		std::memcpy(&ctx.i387.fpregs[i].value[sizeof(BX_CPU(cpu)->the_i387.st_space[i].fraction)], &BX_CPU(cpu)->the_i387.st_space[i].fraction, sizeof(BX_CPU(cpu)->the_i387.st_space[i].exp));
	}

	ctx.i387.fip = BX_CPU(cpu)->the_i387.fip;
	ctx.i387.fdp = BX_CPU(cpu)->the_i387.fdp;
	ctx.i387.foo = BX_CPU(cpu)->the_i387.foo;
	ctx.i387.swd = BX_CPU(cpu)->the_i387.get_status_word();
	ctx.i387.cwd = BX_CPU(cpu)->the_i387.get_control_word();
	ctx.i387.twd = BX_CPU(cpu)->the_i387.get_tag_word();

	#if BX_SUPPORT_EVEX
		std::memcpy(&ctx.vmm, BX_CPU(cpu)->vmm, sizeof(ctx.vmm));
	#else
		#error Must support EVEX to work
	#endif

	ctx.mxcsr = BX_CPU(cpu)->mxcsr.mxcsr;

	#define REGISTER_ACTION(register, size, ctx)
	#define REGISTER_CTX(register, size, ctx)
	#define REGISTER_MSR(register, index) ctx.msrs[msr_##register] = read_msr<(index)>(cpu);
	#include "registers.inc"
	#undef REGISTER_MSR
	#undef REGISTER_CTX
	#undef REGISTER_ACTION

	return ctx;
}

}

Tracer::Tracer(const std::string& trace_dir) : trace_dir_(trace_dir) {}

void Tracer::init(unsigned cpu, const replayer::Replayer& replayer) {
	machine_ = x64_machine_description(cpu, replayer);

	const char* tool_name = "bochs_replayer";
	const char* tool_version = "1.2.0";
	const char* tool_info =
#ifdef __DATE__
	#ifdef __TIME__
			"bochs_replayer version " GIT_VERSION " - " REL_STRING " - Compiled on " __DATE__ " at " __TIME__;
	#else
			"bochs_replayer version " GIT_VERSION " - " REL_STRING " - Compiled on " __DATE__;
	#endif
#else
		"bochs_replayer version " GIT_VERSION " - " REL_STRING;
#endif

	trace_writer_.emplace(trace_dir_ + "/trace.bin", machine_, tool_name, tool_version, tool_info);

	cache_writer_.emplace(trace_dir_ + "/trace.cache", machine_, 1000000, tool_name, tool_version, tool_info);
}

void Tracer::start(unsigned cpu, const replayer::Replayer& replayer) {
	auto memory_writer = trace_writer_->start_initial_memory_section();

	uint8_t mem_buf[TARGET_PAGE_SIZE];
	uint8_t zero_buf[TARGET_PAGE_SIZE];
	std::memset(zero_buf, 0, TARGET_PAGE_SIZE);

	for (size_t addr = 0; addr < BX_MEM(0)->get_memory_len(); addr += TARGET_PAGE_SIZE) {
		auto size = std::min<std::size_t>(TARGET_PAGE_SIZE, BX_MEM(0)->get_memory_len() - addr);
		auto res = BX_MEM(0)->dbg_fetch_mem(BX_CPU(cpu), addr, size, mem_buf);

		if (!res) { // I/O. Just fill page with zeroes.
			memory_writer.write(zero_buf, size);
		} else {
			memory_writer.write(mem_buf, size);
		}
	}

	for (const auto& region : replayer.get_memory_ranges()) {
		for (size_t addr = 0; addr < region.size; addr += TARGET_PAGE_SIZE) {
			auto size = std::min<std::size_t>(TARGET_PAGE_SIZE, region.size - addr);
			memory_writer.write(region.memory + addr, size);
		}
	}

	auto cpu_writer = trace_writer_->start_initial_registers_section(std::move(memory_writer));

	auto ctx = create_cpu_context(cpu);
	save_initial_cpu_context(&ctx, cpu_writer);

	packet_writer_.emplace(trace_writer_->start_events_section(std::move(cpu_writer)));

	started_ = true;
}

void Tracer::end() {
	if (trace_writer_ and packet_writer_) {
		if (packet_writer_->is_event_started())
			packet_writer_->finish_event();

		trace_writer_->finish_events_section(std::move(packet_writer_).value());
		trace_writer_ = std::experimental::nullopt;

		cache_writer_->finalize();
		cache_writer_ = std::experimental::nullopt;
	}
}

void Tracer::execute_instruction(unsigned cpu, const replayer::Replayer& replayer) {
	in_exception_ = false;

	if (!started_) {
		start(cpu, replayer);
		return;
	}

	if (not packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	auto ctx = create_cpu_context(cpu);
	save_cpu_context(&ctx, *packet_writer_);
	packet_writer_->finish_event();

	if (packet_writer_->event_count() != reven_icount()) {
		LOG_DESYNC(cpu, "Inconsistency detected between event count and reven icount. "
		                << std::dec << packet_writer_->event_count() << " != " << reven_icount())
		return;
	}

	cache_writer_->new_context(&ctx, packet_writer_->event_count(), packet_writer_->stream_pos(), replayer);
}

void Tracer::linear_memory_access(std::uint64_t /* linear_address */, std::uint64_t physical_address, std::size_t len, const std::uint8_t* data, bool /* read */, bool write, bool /* execute */) {
	if (!write) {
		return;
	}

	if (not packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	packet_writer_->write_memory(physical_address, data, len);
	cache_writer_->mark_memory_dirty(physical_address, len);
}

void Tracer::physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool /* read */, bool write, bool /* execute */) {
	if (!write) {
		return;
	}

	if (not packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	packet_writer_->write_memory(address, data, len);
	cache_writer_->mark_memory_dirty(address, len);

	// Physical access are mainly done by the MMU
	// We don't want to keep MMU accesses, because they have a huge impact on the database's size.
}

void Tracer::device_physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool /* read */, bool write) {
	if (!write) {
		return;
	}

	if (not packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	packet_writer_->write_memory(address, data, len);
	cache_writer_->mark_memory_dirty(address, len);
}

void Tracer::interrupt(unsigned cpu, unsigned vector) {
	// If we are in an exception, we are not in an interrupt
	if (in_exception_) {
		return;
	}

	if (!packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	auto ctx = create_cpu_context(cpu);
	save_cpu_context(&ctx, *packet_writer_);

	packet_writer_->finish_event();

	packet_writer_->start_event_other(std::string("interrupt ") + std::to_string(vector));
}

void Tracer::exception(unsigned cpu, unsigned vector, unsigned error_code, const replayer::Replayer& replayer) {
	if (!started_) {
		start(cpu, replayer);
	}

	in_exception_ = true;

	if (!packet_writer_->is_event_started()) {
		packet_writer_->start_event_instruction();
	}

	auto ctx = create_cpu_context(cpu);
	save_cpu_context(&ctx, *packet_writer_);

	packet_writer_->finish_event();

	packet_writer_->start_event_other(exception_event_description(vector, error_code));
}

}
}
