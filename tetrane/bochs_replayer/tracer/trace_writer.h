#pragma once

#include <rvnbintrace/trace_writer.h>

using namespace reven::backend::plugins::file::libbintrace;

namespace reven {
namespace tracer {

struct CpuContext;

class BochsWriter : public TraceWriter {
public:
	BochsWriter(const std::string& filename, const MachineDescription& desc,
	            const char* tool_name, const char* tool_version, const char* tool_info);
};

void save_initial_cpu_context(CpuContext* ctx, InitialRegistersSectionWriter& writer);
void save_cpu_context(CpuContext* ctx, EventsSectionWriter& writer);

}
}
