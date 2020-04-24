#pragma once

#include <chrono>
#include <string>

#include "bochs.h"
#include "cpu/cpu.h"

#include <rvncorevirtualbox/core_virtualbox.h>
#include <rvnsyncpoint/sync_file.h>
#include <rvnsyncpoint/hardware_file.h>

namespace reven {
namespace replayer {

class Replayer {
public:
	struct MemoryRange {
		uint64_t start_address;
		uint64_t size;

		uint8_t *memory;
	};

public:
	Replayer();
	~Replayer();

	bool load(const std::string& core_file, const std::string& analyze_dir);

	bool reset(unsigned cpu);
	void execute(unsigned cpu);

	void before_instruction(unsigned cpu, bxInstruction_c *i);
	void after_instruction(unsigned cpu, bxInstruction_c *i);

	void exception(unsigned cpu, unsigned vector, unsigned error_code);
	void interrupt(unsigned cpu, unsigned vector);

	void linear_access(unsigned cpu, std::uint64_t address, unsigned rw);

	void apply_sync_event(unsigned cpu, const reven::vmghost::sync_event& sync_event);
	void apply_hardware_access(unsigned cpu, uint64_t tsc);
	void end_of_scenario(unsigned cpu, bool desync);

	size_t get_memory_size() const;
	const std::vector<MemoryRange>& get_memory_ranges() const;

	void device_memory_read(bx_phy_address addr, unsigned len, uint8_t *data) const;
	void device_memory_write(bx_phy_address addr, unsigned len, uint8_t *data);

	bool get_desync() const { return desync_; };

private:
	bool is_final_int3(unsigned cpu, const bxInstruction_c *i) const;

private:
	reven::vmghost::core_virtualbox core_;
	reven::vmghost::sync_file sync_file_;
	reven::vmghost::hardware_file hardware_file_;

	std::chrono::steady_clock::time_point begin_time_;

	uint64_t current_rip_{0};
	BxExecutePtr_tR current_instruction_{nullptr};

	// Current matched event (if not current_event_.is_valid will be false)
	reven::vmghost::sync_event current_event_;

	// Current ctx before executing the instruction
	reven::vmghost::sync_event::context current_ctx_;

	// Used when we encounter a sync point with an interrupt to call at a later RIP
	reven::vmghost::sync_event saved_interrupt_event_;

	std::vector<MemoryRange> ranges_;

	bool desync_{false};

	std::uint64_t last_sync_point_{0};
};

}
}
