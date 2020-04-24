#include "cache_writer.h"

#include "bochs.h"
#include "cpu/cpu.h"

#include "machine_description.h"
#include "cpu_context.h"
#include "trace_writer.h"

#include "util/log.h"

extern reven::replayer::Replayer replayer;

namespace {

void end_of_scenario(unsigned cpu, bool desync) {
	replayer.end_of_scenario(cpu, desync);
}

}

namespace reven {
namespace tracer {

BochsCacheWriter::BochsCacheWriter(const std::string& filename, const MachineDescription& desc,
                                   std::uint64_t cache_frequency,
                                   const char* tool_name, const char* tool_version, const char* tool_info)
  : CacheWriter(
    	std::make_unique<std::ofstream>(filename, std::ios::binary), TARGET_PAGE_SIZE, desc,
    	tool_name, tool_version, tool_info
    )
  , cache_points_writer_(start_cache_points_section())
  , last_dumped_context_id_(0)
  , cache_frequency_(cache_frequency)
  , memory_buffer_(header().page_size)
{

}

void BochsCacheWriter::mark_memory_dirty(std::uint64_t address, std::uint64_t size)
{
	bool found_region = false;
	for (const auto& region : machine().memory_regions) {
		if (address >= region.start and address + size <= region.start + region.size) {
			found_region = true;
			break;
		}
	}

	if (not found_region)
		return;

	size += address % header().page_size;
	address -= address % header().page_size;

	for(std::uint64_t sized_covered = 0; sized_covered < size; sized_covered += header().page_size) {
		dirty_pages_.insert(address + sized_covered);
	}
}

void BochsCacheWriter::new_context(CpuContext* ctx, std::uint64_t context_id, std::uint64_t trace_stream_pos, const replayer::Replayer& replayer)
{
	if (context_id - last_dumped_context_id_ < cache_frequency_)
		return;
	last_dumped_context_id_ = context_id;

	cache_points_writer_.start_cache_point(context_id, trace_stream_pos);

	#define REGISTER_ACTION(register, size, var) \
	cache_points_writer_.write_register(reg_id(r_##register), reinterpret_cast<const std::uint8_t*>(&ctx->var), size);
	#define REGISTER_CTX(register, size, var) \
	cache_points_writer_.write_register(reg_id(r_##register), reinterpret_cast<const std::uint8_t*>(&ctx->var), size);
	#define REGISTER_MSR(register, index) \
	cache_points_writer_.write_register(reg_id(r_##register), reinterpret_cast<const std::uint8_t*>(&ctx->msrs[msr_##register]), 8);
	#include "registers.inc"
	#undef REGISTER_MSR
	#undef REGISTER_CTX
	#undef REGISTER_ACTION

	for (auto page : dirty_pages_) {
		bool res = false;
		if (page < replayer.get_memory_size()) {
			res = BX_MEM(0)->dbg_fetch_mem(BX_CPU(0), page, header().page_size, memory_buffer_.data());
		} else {
			replayer.device_memory_read(page, header().page_size, memory_buffer_.data());
			res = true;
		}

		if (res) {
			cache_points_writer_.write_memory_page(page, memory_buffer_.data());
		} else {
			LOG_DESYNC(0, "Couldn't read physical memory " << std::showbase << std::hex << page)
			return;
		}
	}
	dirty_pages_.clear();

	cache_points_writer_.finish_cache_point();
}

void BochsCacheWriter::finalize()
{
	finish_cache_points_section(std::move(cache_points_writer_));
}

}
}
