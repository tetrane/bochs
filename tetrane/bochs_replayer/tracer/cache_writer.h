#pragma once

#include <set>

#include <rvnbintrace/cache_writer.h>
#include <replayer/replayer.h>

using namespace reven::backend::plugins::file::libbintrace;

#define TARGET_PAGE_SIZE 4096

namespace reven {
namespace tracer {

struct CpuContext;

class BochsCacheWriter : public CacheWriter {
public:
	BochsCacheWriter(const std::string& filename, const MachineDescription& desc, std::uint64_t cache_frequency,
	                 const char* tool_name, const char* tool_version, const char* tool_info);

	void mark_memory_dirty(std::uint64_t address, std::uint64_t size);
	void new_context(CpuContext* ctx, std::uint64_t context_id, std::uint64_t trace_stream_pos, const replayer::Replayer& replayer);

	void finalize();

private:
	std::set<std::uint64_t> dirty_pages_;
	CachePointsSectionWriter cache_points_writer_;
	std::uint64_t last_dumped_context_id_;
	const std::uint64_t cache_frequency_;
	std::vector<std::uint8_t> memory_buffer_;
};

}
}
