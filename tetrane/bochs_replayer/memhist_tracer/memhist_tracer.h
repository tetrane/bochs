#pragma once

#include <string>
#include <experimental/optional>

#include <rvnmemhistwriter/db_writer.h>

namespace reven {
namespace memhist_tracer {

class MemhistTracer {
public:
	MemhistTracer(const std::string& trace_file);

	void end();

	void linear_memory_access(std::uint64_t linear_address, std::uint64_t physical_address, std::size_t len, const std::uint8_t* data, bool read, bool write, bool execute);
	void physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool read, bool write, bool execute);
	void device_physical_memory_access(std::uint64_t address, std::size_t len, const std::uint8_t* data, bool read, bool write);

private:
	std::experimental::optional<reven::backend::memaccess::db::DbWriter> memory_history_writer_;
};

}
}
