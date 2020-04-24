#include "log.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>

#include <rvnsyncpoint/hardware_access.h>
#include <rvnsyncpoint/sync_event.h>

#include "bochs.h"
#include "cpu/cpu.h"

#include "tetrane/bochs_replayer/icount/fns.h"

namespace reven {
namespace util {

void display_hardware_access(unsigned /* cpu */, const reven::vmghost::hardware_access& access) {
	std::cerr << "Hardware access at TSC=" << std::showbase << std::hex << access.tsc << std::endl;

	std::cerr << "  Type:";

	if (access.is_write())
		std::cerr << " WRITE";
	else
		std::cerr << " READ";

	if (access.is_pci())
		std::cerr << " PCI";

	if (access.is_mmio())
		std::cerr << " MMIO";

	if (access.is_port())
		std::cerr << " PORT";

	std::cerr << std::endl;

	std::cerr << "  Device: ID=" << std::showbase << std::hex << access.device_id << " INSTANCE=" << std::showbase << std::hex << access.device_instance << std::endl;
	std::cerr << "  Physical Address: " << std::showbase << std::hex << access.physical_address << " (" << std::dec << access.length() << " bytes)" << std::endl;
}

void display_sync_event(unsigned cpu, const reven::vmghost::sync_event& sync_event) {
	const unsigned name_column_size = sync_event.has_interrupt ? 14 : 5;
	const unsigned data_column_size = 20;

	std::cerr << std::setw(name_column_size) << std::setfill(' ') << ""
	          << " | "
	          << "Sync event " << std::setw(data_column_size - 11) << std::setfill(' ') << ("$" + std::to_string(sync_event.position))
	          << " | "
	          << "CPU " << std::dec << cpu << " Context"
	          << std::right << std::endl;

	std::cerr << std::setw(name_column_size) << std::setfill('-') << ""
	          << " | "
	          << std::setw(data_column_size) << std::setfill('-') << ""
	          << " | "
	          << std::left << std::setw(data_column_size) << std::setfill('-') << ""
	          << std::right << std::endl;

	#define DISPLAY_LINE(name, value_sync_event, value_cpu) \
	    do { \
		    std::stringstream sstream_sync_event; \
		    sstream_sync_event << value_sync_event; \
 			\
		    std::stringstream sstream_cpu; \
		    sstream_cpu << value_cpu; \
 			\
			std::cerr << std::setw(name_column_size) << std::setfill(' ') << (name) \
			          << " | " \
			          << std::setw(data_column_size) << std::setfill(' ') << sstream_sync_event.str() \
			          << " | " \
			          << std::left << std::setw(data_column_size) << std::setfill(' ') << sstream_cpu.str() \
			          << std::right << std::endl; \
		} while(0);

	DISPLAY_LINE("RIP",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_rip,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx)

	DISPLAY_LINE("RAX",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rax,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[0].rrx)
	DISPLAY_LINE("RBX",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rbx,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[3].rrx)
	DISPLAY_LINE("RCX",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rcx,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[1].rrx)
	DISPLAY_LINE("RDX",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rdx,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[2].rrx)

	DISPLAY_LINE("RSI",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rsi,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[6].rrx)
	DISPLAY_LINE("RDI",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rdi,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[7].rrx)
	DISPLAY_LINE("RBP",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rbp,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[5].rrx)
	DISPLAY_LINE("RSP",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.rsp,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[4].rrx)

	DISPLAY_LINE("R8",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r8,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[8].rrx)
	DISPLAY_LINE("R9",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r9,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[9].rrx)
	DISPLAY_LINE("R10",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r10,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[10].rrx)
	DISPLAY_LINE("R11",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r11,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[11].rrx)
	DISPLAY_LINE("R12",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r12,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[12].rrx)
	DISPLAY_LINE("R13",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r13,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[13].rrx)
	DISPLAY_LINE("R14",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r14,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[14].rrx)
	DISPLAY_LINE("R15",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.r15,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->gen_reg[15].rrx)

	DISPLAY_LINE("CR0",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.cr0,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->cr0.get32())
	DISPLAY_LINE("CR2",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.cr2,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->cr2)
	DISPLAY_LINE("CR3",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.cr3,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->cr3)
	DISPLAY_LINE("CR4",
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.start_context.cr4,
	             "0x" << std::setw(16) << std::setfill('0') << std::hex << BX_CPU(cpu)->cr4.get32())

	DISPLAY_LINE("FSW",
	             "0x" << std::setw(4) << std::setfill('0') << std::hex << sync_event.start_context.fpu_sw,
	             "0x" << std::setw(4) << std::setfill('0') << std::hex << BX_CPU(cpu)->the_i387.swd)
	DISPLAY_LINE("FCW",
	             "0x" << std::setw(4) << std::setfill('0') << std::hex << sync_event.start_context.fpu_cw,
	             "0x" << std::setw(4) << std::setfill('0') << std::hex << BX_CPU(cpu)->the_i387.cwd)
	DISPLAY_LINE("FTAGS",
	             "0x" << std::setw(2) << std::setfill('0') << std::hex << static_cast<std::uint32_t>(sync_event.start_context.fpu_tags),
	             "0x" << std::setw(2) << std::setfill('0') << std::hex << static_cast<std::uint32_t>(BX_CPU(cpu)->pack_FPU_TW(BX_CPU(cpu)->the_i387.twd)))

	if (sync_event.has_interrupt) {
		DISPLAY_LINE("Interrupt",
		             "0x" << std::setw(2) << std::setfill('0') << std::hex << static_cast<std::uint32_t>(sync_event.interrupt_vector) << " "
		             << "(0x" << std::setw(8) << std::setfill('0') << std::hex << sync_event.fault_error_code << ")",
		             std::setw(data_column_size) << std::setfill('X') << "")
		DISPLAY_LINE("Interrupt RIP",
		             "0x" << std::setw(16) << std::setfill('0') << std::hex << sync_event.interrupt_rip,
		             std::setw(data_column_size) << std::setfill('X') << "")
	}

	BX_CPU(cpu)->debug_disasm_instruction(BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx);
}

void display_progress(unsigned cpu, const reven::vmghost::sync_event& sync_event,
                      std::uint64_t nb_sync_point, const std::chrono::steady_clock::time_point& begin_time)
{
	static unsigned next_step = 0;
	double pourcentage = ((double)sync_event.position / (double)nb_sync_point) * 100.0;

	if (std::round(pourcentage * 10.0) < next_step) {
		return;
	}

	next_step = std::round(pourcentage * 10.0) + 1;

	std::chrono::duration<double> elapsed_seconds = std::chrono::steady_clock::now() - begin_time;

	std::cerr << "Progress: "
	          << std::dec << std::setw(9) << std::setfill(' ') << sync_event.position
	          << " (" << std::fixed << std::setprecision(1) << std::setw(5) << std::setfill(' ') << pourcentage << "%) sync points"
	          << " - "
	          << std::dec << std::setw(12) << std::setfill(' ') << BX_CPU(cpu)->icount << " instrs"
	          << " (at " << std::dec << std::setw(7) << std::setfill(' ')
	          << static_cast<uint64_t>(BX_CPU(cpu)->icount / elapsed_seconds.count()) << " Hz)"
	          << " - "
	          << std::setw(7) << std::setfill(' ') << std::fixed << std::setprecision(2) << elapsed_seconds.count() << " seconds"
	          << std::endl;
}

}
}
