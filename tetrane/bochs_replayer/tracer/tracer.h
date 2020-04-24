#pragma once

#include <string>
#include <experimental/optional>

#include "cache_writer.h"
#include "cpu_context.h"
#include "machine_description.h"
#include "trace_writer.h"

#include <replayer/replayer.h>

namespace reven {
namespace tracer {

class Tracer {
public:
	Tracer(const std::string& trace_dir);

	void init(unsigned cpu, const replayer::Replayer& replayer);

	void start(unsigned cpu, const replayer::Replayer& replayer);
	void end();

	void execute_instruction(unsigned cpu, const replayer::Replayer& replayer);
	void linear_memory_access(std::uint64_t linear_address, std::uint64_t physical_address, std::size_t len, const std::uint8_t* data, bool read, bool write, bool execute);
	void physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool read, bool write, bool execute);
	void device_physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool read, bool write);

	void interrupt(unsigned cpu, unsigned vector);
	void exception(unsigned cpu, unsigned vector, unsigned error_code, const replayer::Replayer& replayer);

private:
	std::string trace_dir_;

	bool started_{false};
	bool in_exception_{false}; // Are we executing an exception? (will be reset to false after the next instruction)

	std::experimental::optional<BochsWriter> trace_writer_;
	std::experimental::optional<EventsSectionWriter> packet_writer_;
	std::experimental::optional<BochsCacheWriter> cache_writer_;

	MachineDescription machine_;
};

}
}
