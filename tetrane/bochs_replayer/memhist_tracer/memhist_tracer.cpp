#include "memhist_tracer.h"

#include "tetrane/bochs_replayer/icount/fns.h"

#include "bxversion.h"

namespace reven {
namespace memhist_tracer {

MemhistTracer::MemhistTracer(const std::string& trace_file) {
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

	std::remove(trace_file.c_str());
	memory_history_writer_.emplace(trace_file.c_str(), tool_name, tool_version, tool_info);
}

void MemhistTracer::end() {
	if (memory_history_writer_) {
		memory_history_writer_->discard_after(reven_icount());
		memory_history_writer_ = std::experimental::nullopt;
	}
}

void MemhistTracer::linear_memory_access(std::uint64_t linear_address, std::uint64_t physical_address, std::size_t len, const std::uint8_t* /* data */, bool /* read */, bool write, bool execute) {
	if (execute) {
		return;
	}

	memory_history_writer_->push({reven_icount(), physical_address, linear_address, static_cast<std::uint32_t>(len), true, write ? reven::backend::memaccess::db::Operation::Write : reven::backend::memaccess::db::Operation::Read});
}

void MemhistTracer::physical_memory_access(std::uint64_t /* address */, std::size_t /* len */, const std::uint8_t* /* data */, bool /* read */, bool /* write */ , bool /* execute */) {
	// Physical access are mainly done by the MMU
	// We don't want to keep MMU accesses, because they have a huge impact on the database's size.
}

void MemhistTracer::device_physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* /* data */, bool /* read */, bool write) {
	memory_history_writer_->push({reven_icount(), address, 0, static_cast<std::uint32_t>(len), false, write ? reven::backend::memaccess::db::Operation::Write : reven::backend::memaccess::db::Operation::Read});
}

}
}
