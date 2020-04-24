#include <experimental/optional>

#include "tetrane/bochs_replayer/replayer/replayer.h"
#include "tetrane/bochs_replayer/tracer/tracer.h"
#include "tetrane/bochs_replayer/memhist_tracer/memhist_tracer.h"
#include "tetrane/bochs_replayer/icount/icount.h"
#include "tetrane/bochs_replayer/icount/fns.h"
#include "tetrane/bochs_replayer/util/log.h"

extern reven::replayer::Replayer replayer;
extern std::experimental::optional<reven::tracer::Tracer> tracer;
extern std::experimental::optional<reven::memhist_tracer::MemhistTracer> memhist_tracer;
extern reven::icount::ICount tick_counter;

namespace {
	std::experimental::optional<std::uint64_t> rip_repeat_iteration;

	class {
	public:
		void clear() {
			lin1_ = 0;
			lin2_ = 0;

			phys1_ = 0;
			phys2_ = 0;
		}

		bool register_new_access(bx_address lin, bx_phy_address phys) {
			if (lin1_ == 0) {
				lin1_ = lin;
				phys1_ = phys;
			} else if (lin2_ == 0) {
				lin2_ = lin;
				phys2_ = phys;
			} else {
				return false;
			}

			return true;
		}

		bx_address get_linear_address(bx_phy_address phys) {
			if (phys == phys1_) {
				return lin1_;
			} else if (phys == phys2_) {
				return lin2_;
			} else {
				return 0;
			}
		}

	private:
		bx_address lin1_{0};
		bx_address lin2_{0};

		bx_phy_address phys1_{0};
		bx_phy_address phys2_{0};
	} current_rmw_operation;

	void end_of_scenario(unsigned cpu, bool desync) {
		replayer.end_of_scenario(cpu, desync);
	}
}

std::uint64_t reven_icount(void) {
	return tick_counter.icount();
}

void bx_instr_init_env(void) {}
void bx_instr_exit_env(void) {}

void bx_instr_initialize(unsigned /* cpu */) {}
void bx_instr_exit(unsigned /* cpu */) {}

void bx_instr_reset(unsigned /* cpu */, unsigned /* type */) {}
void bx_instr_hlt(unsigned /* cpu */) {}
void bx_instr_mwait(unsigned /* cpu */, bx_phy_address /* addr */, unsigned /* len */, Bit32u /* flags */) {}

void bx_instr_debug_promt() {}
void bx_instr_debug_cmd(const char* /* cmd */) {}

void bx_instr_cnear_branch_taken(unsigned /* cpu */, bx_address /* branch_eip */, bx_address /* new_eip */) {}
void bx_instr_cnear_branch_not_taken(unsigned /* cpu */, bx_address /* branch_eip */) {}
void bx_instr_ucnear_branch(unsigned /* cpu */, unsigned /* what */, bx_address /* branch_eip */, bx_address /* new_eip */) {}
void bx_instr_far_branch(unsigned /* cpu */, unsigned /* what */, Bit16u /* prev_cs */, bx_address /* prev_eip */, Bit16u /* new_cs */, bx_address /* new_eip */) {}

void bx_instr_opcode(unsigned /* cpu */, bxInstruction_c* /* i */, const Bit8u* /* opcode */, unsigned /* len */, bx_bool /* is32 */, bx_bool /* is64 */) {}

void bx_instr_interrupt(unsigned cpu, unsigned vector) {
	tick_counter.break_and_start_new_instruction();

	rip_repeat_iteration = std::experimental::nullopt;

	if (tracer)
		tracer->interrupt(cpu, vector);
	replayer.interrupt(cpu, vector);
}

void bx_instr_exception(unsigned cpu, unsigned vector, unsigned error_code) {
	rip_repeat_iteration = std::experimental::nullopt;

	if (tracer)
		tracer->exception(cpu, vector, error_code, replayer);
	replayer.exception(cpu, vector, error_code);
}

void bx_instr_hwinterrupt(unsigned /* cpu */, unsigned /* vector */, Bit16u /* cs */, bx_address /* eip */) {}

void bx_instr_tlb_cntrl(unsigned /* cpu */, unsigned /* what */, bx_phy_address /* new_cr3 */) {}
void bx_instr_clflush(unsigned /* cpu */, bx_address /* laddr */, bx_phy_address /* paddr */) {}
void bx_instr_cache_cntrl(unsigned /* cpu */, unsigned /* what */) {}
void bx_instr_prefetch_hint(unsigned /* cpu */, unsigned /* what */, unsigned /* seg */, bx_address /* offset */) {}

void bx_instr_before_execution(unsigned cpu, bxInstruction_c *i) {
	// Dump the current instruction in the trace only if we aren't in repeat iteration
	if (!rip_repeat_iteration or rip_repeat_iteration != BX_CPU(cpu)->prev_rip) {
		tick_counter.before_instruction();

		if (tracer) {
			tracer->execute_instruction(cpu, replayer);
		}
	}

	replayer.before_instruction(cpu, i);
	rip_repeat_iteration = std::experimental::nullopt;
}

void bx_instr_after_execution(unsigned cpu, bxInstruction_c *i) {
	replayer.after_instruction(cpu, i);
	current_rmw_operation.clear();
}

void bx_instr_repeat_iteration(unsigned cpu , bxInstruction_c* /* i */) {
	rip_repeat_iteration.emplace(BX_CPU(cpu)->prev_rip);
}

void bx_instr_inp(Bit16u /* addr */, unsigned /* len */) {}
void bx_instr_inp2(Bit16u /* addr */, unsigned /* len */, unsigned /* val */) {}
void bx_instr_outp(Bit16u /* addr */, unsigned /* len */, unsigned /* val */) {}

void bx_instr_lin_access(unsigned cpu, bx_address lin, bx_address phy, unsigned len, unsigned /* memtype */, unsigned rw, Bit8u* data) {
	replayer.linear_access(cpu, lin, rw);

	if (rw == BX_RW) {
		if (len == 16) {
			// To handle read_RMW_linear_dqword_aligned_64 followed by write_RMW_linear_dqword
			if (!current_rmw_operation.register_new_access(lin, phy)) {
				LOG_DESYNC(cpu, "More than two accesses in a RMW operation");
			}

			if (!current_rmw_operation.register_new_access(lin + 8, phy + 8)) {
				LOG_DESYNC(cpu, "More than two accesses in a RMW operation");
			}
		} else if (!current_rmw_operation.register_new_access(lin, phy)) {
			LOG_DESYNC(cpu, "More than two accesses in a RMW operation");
		}

		// If we are in the middle of an rmw operation we want to tell the tracer that is a read because we didn't do the write yet
		if (tracer)
			tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), true, false, false);

		if (memhist_tracer)
			memhist_tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), true, false, false);
	} else {
		if (tracer)
			tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE, rw == BX_EXECUTE);

		if (memhist_tracer)
			memhist_tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE, rw == BX_EXECUTE);
	}
}

void bx_instr_phy_access(unsigned cpu, bx_address phy, unsigned len, unsigned /* memtype */, unsigned rw, Bit8u* data) {
	if (rw == BX_RW) {
		bx_address lin = current_rmw_operation.get_linear_address(phy);

		if (lin == 0) {
			LOG_DESYNC(cpu, "Physical access in Read/Write without matching linear access at " << std::hex << phy << " (" << std::dec << len << " bytes" << ")")
		}

		// We don't have to call replayer.linear_access because it should already have launch a pagefault if necessary
		if (tracer)
			tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), false, true, false);

		if (memhist_tracer)
			memhist_tracer->linear_memory_access(lin, phy, len, reinterpret_cast<const std::uint8_t*>(data), false, true, false);
	} else {
		if (tracer)
			tracer->physical_memory_access(phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE, rw == BX_EXECUTE);

		if (memhist_tracer)
			memhist_tracer->physical_memory_access(phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE, rw == BX_EXECUTE);
	}
}

void bx_instr_dev_phy_access(bx_address phy, unsigned len, unsigned rw, Bit8u* data) {
	if (rw == BX_RW) {
		LOG_DESYNC(0, "Physical access in Read/Write from a device at " << std::hex << phy << " (" << std::dec << len << " bytes" << ")")
	}

	if (tracer)
		tracer->device_physical_memory_access(phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE);

	if (memhist_tracer)
		memhist_tracer->device_physical_memory_access(phy, len, reinterpret_cast<const std::uint8_t*>(data), rw == BX_READ, rw == BX_WRITE);
}

void bx_instr_wrmsr(unsigned /* cpu */, unsigned /* addr */, Bit64u /* value */) {}

void bx_instr_vmexit(unsigned /* cpu */, Bit32u /* reason */, Bit64u /* qualification */) {}
