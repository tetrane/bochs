#include "replayer.h"

#include <iostream>
#include <algorithm>

#include "iodev/iodev.h"

#include "util/log.h"
#include "tetrane/bochs_replayer/icount/fns.h"

namespace reven {
namespace replayer {

namespace {
	// Instructions where we need a sync point
	const std::vector<BxExecutePtr_tR> emulated_instructions{
		&BX_CPU_C::INSB32_YbDX,
		&BX_CPU_C::INSB16_YbDX,
		&BX_CPU_C::INSW32_YwDX,
		&BX_CPU_C::INSW16_YwDX,
		&BX_CPU_C::INSD32_YdDX,
		&BX_CPU_C::INSD16_YdDX,
		&BX_CPU_C::OUTSB32_DXXb,
		&BX_CPU_C::OUTSB16_DXXb,
		&BX_CPU_C::OUTSW32_DXXw,
		&BX_CPU_C::OUTSW16_DXXw,
		&BX_CPU_C::OUTSD32_DXXd,
		&BX_CPU_C::OUTSD16_DXXd,

		&BX_CPU_C::REP_INSB_YbDX,
		&BX_CPU_C::REP_INSW_YwDX,
		&BX_CPU_C::REP_INSD_YdDX,
		&BX_CPU_C::REP_OUTSB_DXXb,
		&BX_CPU_C::REP_OUTSW_DXXw,
		&BX_CPU_C::REP_OUTSD_DXXd,

		&BX_CPU_C::INSB64_YbDX,
		&BX_CPU_C::INSW64_YwDX,
		&BX_CPU_C::INSD64_YdDX,

		&BX_CPU_C::OUTSB64_DXXb,
		&BX_CPU_C::OUTSW64_DXXw,
		&BX_CPU_C::OUTSD64_DXXd,

		&BX_CPU_C::IN_ALIb,
		&BX_CPU_C::IN_AXIb,
		&BX_CPU_C::IN_EAXIb,
		&BX_CPU_C::OUT_IbAL,
		&BX_CPU_C::OUT_IbAX,
		&BX_CPU_C::OUT_IbEAX,

		&BX_CPU_C::IN_ALDX,
		&BX_CPU_C::IN_AXDX,
		&BX_CPU_C::IN_EAXDX,
		&BX_CPU_C::OUT_DXAL,
		&BX_CPU_C::OUT_DXAX,
		&BX_CPU_C::OUT_DXEAX,

		&BX_CPU_C::RDTSC,
		&BX_CPU_C::RDMSR,
		&BX_CPU_C::WRMSR,

		&BX_CPU_C::MONITOR,
		&BX_CPU_C::MWAIT,

		&BX_CPU_C::HLT,
	};

	// Instructions that we don't want to nop to let bochs execute them
	const std::vector<BxExecutePtr_tR> executed_instructions{
		// We don't want to skip WRMSR because it's important for bochs to know the value of some MSRs (FS_BASE for example)
		&BX_CPU_C::WRMSR,
	};

	static bool looks_like_eflags(std::uint32_t value)
	{
		return (((value >> 1)  & 1) == 1 and
		        ((value >> 3)  & 1) == 0 and
		        ((value >> 5)  & 1) == 0 and
		        ((value >> 15) & 1) == 0 and
		        (value >> 22) == 0);
	}

	static bool match_with_no_eflags(reven::vmghost::sync_event::context& a, reven::vmghost::sync_event::context& b) {
		bool valid = true;

	#define TEST_IGNORE_EFL(sync, value) 													\
		valid &= (sync == value) || (looks_like_eflags(sync) && looks_like_eflags(value));

		TEST_IGNORE_EFL(a.rax, b.rax);
		TEST_IGNORE_EFL(a.rbx, b.rbx);
		TEST_IGNORE_EFL(a.rcx, b.rcx);
		TEST_IGNORE_EFL(a.rdx, b.rdx);
		TEST_IGNORE_EFL(a.rsi, b.rsi);
		TEST_IGNORE_EFL(a.rdi, b.rdi);
		TEST_IGNORE_EFL(a.rbp, b.rbp);
		TEST_IGNORE_EFL(a.r8, b.r8);
		TEST_IGNORE_EFL(a.r9, b.r9);
		TEST_IGNORE_EFL(a.r10, b.r10);
		TEST_IGNORE_EFL(a.r11, b.r11);
		TEST_IGNORE_EFL(a.r12, b.r12);
		TEST_IGNORE_EFL(a.r13, b.r13);
		TEST_IGNORE_EFL(a.r14, b.r14);
		TEST_IGNORE_EFL(a.r15, b.r15);

		// There is no way those register can contain an eflag value
		valid &= (a.rsp == b.rsp);
		valid &= (a.cr0 == b.cr0);
		valid &= (a.cr2 == b.cr2);
		valid &= (a.cr3 == b.cr3);
		valid &= (a.cr4 == b.cr4);

		return valid;
	}

	static bool exception_do_have_error_code(std::uint8_t vector) {
		if (vector == 8 // Double fault
			|| vector == 10 // Invalid TSS
			|| vector == 11 // Segment not present
			|| vector == 12 // Stack segment fault
			|| vector == 13 // General Protection
			|| vector == 14 // Page fault
			|| vector == 17 // Alignment check
		) {
			return true;
		}

		return false;
	}
}

Replayer::Replayer() {}

Replayer::~Replayer() {
	for (auto& range : ranges_) {
		delete [] range.memory;
	}

	ranges_.clear();
}

bool Replayer::load(const std::string& core_file, const std::string& analyze_dir) {
	try {
		core_.parse(core_file);
	} catch(const std::exception& e) {
		LOG_FATAL_ERROR("Exception: " << e.what())
		return false;
	}

	if (core_.cpu_count() != 1) {
		LOG_FATAL_ERROR("Core file should have only 1 core for now")
		return false;
	}

	try {
		if (!sync_file_.load(analyze_dir + "/sync_point.bin", analyze_dir + "/sync_point_data.bin")) {
			LOG_FATAL_ERROR("Can't open the sync file")
			return false;
		}

		sync_file_.next();
	} catch(const std::runtime_error& e) {
		LOG_FATAL_ERROR("Error while loading the sync file: " << e.what());
		return false;
	}

	try {
		if (!hardware_file_.load(analyze_dir + "/hardware.bin")) {
			LOG_FATAL_ERROR("Can't open the hardware file");
			return false;
		}

		hardware_file_.next();
	} catch(const std::runtime_error& e) {
		LOG_FATAL_ERROR("Error while loading the hardware file: " << e.what());
		return false;
	}

	return true;
}

static bx_bool memory_read_handler(bx_phy_address addr, unsigned len, void *data, void *param) {
	static_cast<Replayer*>(param)->device_memory_read(addr, len, static_cast<uint8_t*>(data));
	return true;
}

static bx_bool memory_write_handler(bx_phy_address addr, unsigned len, void *data, void *param) {
	static_cast<Replayer*>(param)->device_memory_write(addr, len, static_cast<uint8_t*>(data));
	return true;
}


bool Replayer::reset(unsigned cpu) {
	BX_MEM(0)->enable_smram(true, true);

	// Write the memory
	core_.physical_memory()->visit_chunks([&](const reven::vmghost::MemoryChunk& chunk) {
		// Is the RAM from a device?
		if (chunk.physical_address() != 0) {
			ranges_.push_back({
				chunk.physical_address(),
				chunk.size_in_memory(),
				new std::uint8_t[chunk.size_in_memory()]
			});

			chunk.read(chunk.physical_address(), ranges_.back().memory, chunk.size_in_memory());

			BX_MEM(0)->unregisterMemoryHandlers(NULL, chunk.physical_address(), chunk.physical_address() + chunk.size_in_memory() - 1);
			if (!BX_MEM(0)->registerMemoryHandlers(this, memory_read_handler, memory_write_handler, /* memory_direct_access_handler_t da_handler */ NULL,
			    chunk.physical_address(), chunk.physical_address() + chunk.size_in_memory() - 1)) {
				LOG_ERROR("Can't register memory handler");
			}

			return;
		}

		std::uint8_t* buffer = new std::uint8_t[chunk.size_in_memory()];
		chunk.read(chunk.physical_address(), buffer, chunk.size_in_memory());

		if (!BX_MEM(0)->dbg_set_mem(chunk.physical_address(), chunk.size_in_memory(), reinterpret_cast<const Bit8u*>(buffer))) {
			LOG_ERROR("Can't write memory of size " << chunk.size_in_memory() << " at " << std::showbase << std::hex << chunk.physical_address())
		}

		delete [] buffer;
	});

	// Copy the memory from 0xE0000000 to 0xB8000
	{
		const std::size_t mmio_size = 0xC0000 - 0xB8000;
		std::uint8_t vram[mmio_size * 4];

		// The memory is registered in intern
		device_memory_read(0xE0000000, sizeof(vram), reinterpret_cast<uint8_t*>(vram));

		for (std::size_t i = 0; i < mmio_size / 2; ++i) {
			if (!BX_MEM(0)->dbg_set_mem(0xB8000 + i * 2, 2, reinterpret_cast<const Bit8u*>(&vram[i * 8]))) {
				LOG_ERROR("Can't set the VRAM");
				break;
			}
		}
	}

	const auto it = core_.cpu_begin() + cpu;

	BX_CPU(cpu)->reset(0);
	BX_CPU(cpu)->async_event = 0;

	// Load general registers
	BX_CPU(cpu)->gen_reg[0].rrx = it->rax();
	BX_CPU(cpu)->gen_reg[3].rrx = it->rbx();
	BX_CPU(cpu)->gen_reg[1].rrx = it->rcx();
	BX_CPU(cpu)->gen_reg[2].rrx = it->rdx();

	BX_CPU(cpu)->gen_reg[4].rrx = it->rsp();
	BX_CPU(cpu)->gen_reg[5].rrx = it->rbp();
	BX_CPU(cpu)->gen_reg[6].rrx = it->rsi();
	BX_CPU(cpu)->gen_reg[7].rrx = it->rdi();

	BX_CPU(cpu)->gen_reg[8].rrx = it->r8();
	BX_CPU(cpu)->gen_reg[9].rrx = it->r9();
	BX_CPU(cpu)->gen_reg[10].rrx = it->r10();
	BX_CPU(cpu)->gen_reg[11].rrx = it->r11();
	BX_CPU(cpu)->gen_reg[12].rrx = it->r12();
	BX_CPU(cpu)->gen_reg[13].rrx = it->r13();
	BX_CPU(cpu)->gen_reg[14].rrx = it->r14();
	BX_CPU(cpu)->gen_reg[15].rrx = it->r15();

	// Load rflags
	BX_CPU(cpu)->dbg_set_eflags(it->rflags());

	// Load RIP
	BX_CPU(cpu)->prev_rip = BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx = it->rip();

	// Load FPU state
	BX_CPU(cpu)->the_i387.cwd = it->fpu_control_word();
	BX_CPU(cpu)->the_i387.twd = it->fpu_tag_word();
	BX_CPU(cpu)->the_i387.swd = it->fpu_status_word();
	BX_CPU(cpu)->the_i387.tos = it->fpu_status_word() >> 11;

	BX_CPU(cpu)->the_i387.foo = it->fpu_fop();
	BX_CPU(cpu)->the_i387.fip = it->fpu_ip();
	BX_CPU(cpu)->the_i387.fdp = it->fpu_dp();
	BX_CPU(cpu)->the_i387.fcs = it->fpu_cs();
	BX_CPU(cpu)->the_i387.fds = it->fpu_ds();

	for (unsigned i = 0; i < 8; ++i) {
		floatx80 value;
		const long double double_value = it->fpu_register(i);

		std::memcpy(&value.fraction, reinterpret_cast<const char*>(&double_value), sizeof(value.fraction));
		std::memcpy(&value.exp, reinterpret_cast<const char*>(&double_value) + sizeof(value.fraction), sizeof(value.exp));

		BX_CPU(cpu)->the_i387.st_space[(i + 8 - it->fpu_top()) & 0x7] = value;
	}

	// Set XMM registers
	// Warning: The core file only contains 4 * 32 bits for the XMM
	// so we can't restore all the YMM and ZMM registers that are much bigger
	for (unsigned i = 0; i < 16; ++i) {
		#if BX_SUPPORT_EVEX
			BX_CPU(cpu)->vmm[i].zmm_u32[0] = it->partial_sse_register(i, 0);
			BX_CPU(cpu)->vmm[i].zmm_u32[1] = it->partial_sse_register(i, 1);
			BX_CPU(cpu)->vmm[i].zmm_u32[2] = it->partial_sse_register(i, 2);
			BX_CPU(cpu)->vmm[i].zmm_u32[3] = it->partial_sse_register(i, 3);
		#else
			#if BX_SUPPORT_AVX
				BX_CPU(cpu)->vmm[i].ymm_u32[0] = it->partial_sse_register(i, 0);
				BX_CPU(cpu)->vmm[i].ymm_u32[1] = it->partial_sse_register(i, 1);
				BX_CPU(cpu)->vmm[i].ymm_u32[2] = it->partial_sse_register(i, 2);
				BX_CPU(cpu)->vmm[i].ymm_u32[3] = it->partial_sse_register(i, 3);
			#else
				BX_CPU(cpu)->vmm[i].xmm_u32[0] = it->partial_sse_register(i, 0);
				BX_CPU(cpu)->vmm[i].xmm_u32[1] = it->partial_sse_register(i, 1);
				BX_CPU(cpu)->vmm[i].xmm_u32[2] = it->partial_sse_register(i, 2);
				BX_CPU(cpu)->vmm[i].xmm_u32[3] = it->partial_sse_register(i, 3);
			#endif
		#endif
	}

	// Set control registers
	BX_CPU(cpu)->cr0.set32(it->cr0());
	BX_CPU(cpu)->cr2 = it->cr2();
	BX_CPU(cpu)->cr3 = it->cr3();
	BX_CPU(cpu)->cr4.set32(it->cr4());
	BX_CPU(cpu)->lapic.set_tpr((it->cr8() & 0xF) << 4);

	// Set debug registers
	for (unsigned i = 0; i < 4; ++i)
		BX_CPU(cpu)->dr[i] = it->dr(i);

	BX_CPU(cpu)->dr6.val32 = it->dr(6);
	BX_CPU(cpu)->debug_trap = it->dr(6);
	BX_CPU(cpu)->dr7.val32 = it->dr(7);

	// Load mxcsr
	BX_CPU(cpu)->mxcsr = it->mxcsr();
	BX_CPU(cpu)->mxcsr_mask = it->mxcsr_mask();

	// Load MSR
	BX_CPU(cpu)->efer.set32(it->msrEFER());
	BX_CPU(cpu)->msr.star = it->msrSTAR();
	BX_CPU(cpu)->msr.pat = it->msrPAT();
	BX_CPU(cpu)->msr.lstar = it->msrLSTAR();
	BX_CPU(cpu)->msr.cstar = it->msrCSTAR();
	BX_CPU(cpu)->msr.fmask = it->msrSFMASK();
	BX_CPU(cpu)->msr.kernelgsbase = it->msrKernelGSBase();
	BX_CPU(cpu)->msr.apicbase = it->msrApicBase();

	// Reload the PAE cache if necessary
	if (BX_CPU(cpu)->cr0.get_PG() && BX_CPU(cpu)->cr4.get_PAE() && !BX_CPU(cpu)->long_mode()) {
		if (!BX_CPU(cpu)->CheckPDPTR(BX_CPU(cpu)->cr3)) {
			LOG_ERROR("Can't reload PDPTR cache");
		}
	}

	// Set gdtr
	BX_CPU(cpu)->gdtr.base = it->gdtr_base();
	BX_CPU(cpu)->gdtr.limit = it->gdtr_limit();

	// Set idtr
	BX_CPU(cpu)->idtr.base = it->idtr_base();
	BX_CPU(cpu)->idtr.limit = it->idtr_limit();

	// Load LDTR
	BX_CPU(cpu)->ldtr.selector.value = it->ldtr();
	BX_CPU(cpu)->ldtr.selector.index = it->ldtr() >> 3;
	BX_CPU(cpu)->ldtr.selector.rpl = it->ldtr() & 0x3;
	BX_CPU(cpu)->ldtr.selector.ti = (it->ldtr() >> 2) & 0x01;

	BX_CPU(cpu)->ldtr.cache.u.segment.base = it->ldtr_base();
	BX_CPU(cpu)->ldtr.cache.u.segment.limit_scaled = it->ldtr_limit();
	BX_CPU(cpu)->ldtr.cache.p = it->ldtr_attr_present();
	BX_CPU(cpu)->ldtr.cache.dpl = it->ldtr_attr_dpl();
	BX_CPU(cpu)->ldtr.cache.segment = it->ldtr_attr_desc_type();
	BX_CPU(cpu)->ldtr.cache.type = it->ldtr_attr_type();
	BX_CPU(cpu)->ldtr.cache.u.segment.g = it->ldtr_attr_granularity();
	BX_CPU(cpu)->ldtr.cache.u.segment.d_b = it->ldtr_attr_def_big();
	BX_CPU(cpu)->ldtr.cache.u.segment.l = it->ldtr_attr_long();
	BX_CPU(cpu)->ldtr.cache.u.segment.avl = it->ldtr_attr_available();

	// Load TR
	BX_CPU(cpu)->tr.selector.value = it->tr();
	BX_CPU(cpu)->tr.selector.index = it->tr() >> 3;
	BX_CPU(cpu)->tr.selector.rpl = it->tr() & 0x3;
	BX_CPU(cpu)->tr.selector.ti = (it->tr() >> 2) & 0x01;

	BX_CPU(cpu)->tr.cache.u.segment.base = it->tr_base();
	BX_CPU(cpu)->tr.cache.u.segment.limit_scaled = it->tr_limit();
	BX_CPU(cpu)->tr.cache.p = it->tr_attr_present();
	BX_CPU(cpu)->tr.cache.dpl = it->tr_attr_dpl();
	BX_CPU(cpu)->tr.cache.segment = it->tr_attr_desc_type();
	BX_CPU(cpu)->tr.cache.type = it->tr_attr_type();
	BX_CPU(cpu)->tr.cache.u.segment.g = it->tr_attr_granularity();
	BX_CPU(cpu)->tr.cache.u.segment.d_b = it->tr_attr_def_big();
	BX_CPU(cpu)->tr.cache.u.segment.l = it->tr_attr_long();
	BX_CPU(cpu)->tr.cache.u.segment.avl = it->tr_attr_available();

	// Load segment selector
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].selector.value = it->cs();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].selector.index = it->cs() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].selector.rpl = it->cs() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].selector.ti = (it->cs() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.base = it->cs_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.limit_scaled = it->cs_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.p = it->cs_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.dpl = it->cs_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.segment = it->cs_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.type = it->cs_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.g = it->cs_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.d_b = it->cs_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.l = it->cs_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_CS].cache.u.segment.avl = it->cs_attr_available();

	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].selector.value = it->ds();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].selector.index = it->ds() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].selector.rpl = it->ds() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].selector.ti = (it->ds() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.base = it->ds_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.limit_scaled = it->ds_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.p = it->ds_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.dpl = it->ds_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.segment = it->ds_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.type = it->ds_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.g = it->ds_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.d_b = it->ds_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.l = it->ds_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.u.segment.avl = it->ds_attr_available();

	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].selector.value = it->ss();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].selector.index = it->ss() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].selector.rpl = it->ss() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].selector.ti = (it->ss() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.base = it->ss_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.limit_scaled = it->ss_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.p = it->ss_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.dpl = it->ss_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.segment = it->ss_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.type = it->ss_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.g = it->ss_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.d_b = it->ss_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.l = it->ss_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_SS].cache.u.segment.avl = it->ss_attr_available();

	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].selector.value = it->es();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].selector.index = it->es() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].selector.rpl = it->es() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].selector.ti = (it->es() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.base = it->es_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.limit_scaled = it->es_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.p = it->es_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.dpl = it->es_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.segment = it->es_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.type = it->es_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.g = it->es_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.d_b = it->es_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.l = it->es_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.u.segment.avl = it->es_attr_available();

	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].selector.value = it->fs();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].selector.index = it->fs() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].selector.rpl = it->fs() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].selector.ti = (it->fs() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.base = it->fs_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.limit_scaled = it->fs_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.p = it->fs_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.dpl = it->fs_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.segment = it->fs_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.type = it->fs_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.g = it->fs_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.d_b = it->fs_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.l = it->fs_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_FS].cache.u.segment.avl = it->fs_attr_available();

	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].selector.value = it->gs();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].selector.index = it->gs() >> 3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].selector.rpl = it->gs() & 0x3;
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].selector.ti = (it->gs() >> 2) & 0x01;

	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.base = it->gs_base();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.limit_scaled = it->gs_limit();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.p = it->gs_attr_present();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.dpl = it->gs_attr_dpl();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.segment = it->gs_attr_desc_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.type = it->gs_attr_type();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.g = it->gs_attr_granularity();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.d_b = it->gs_attr_def_big();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.l = it->gs_attr_long();
	BX_CPU(cpu)->sregs[BX_SEG_REG_GS].cache.u.segment.avl = it->gs_attr_available();

	// Load sysenter information
	BX_CPU(cpu)->msr.sysenter_cs_msr = it->sysenter_cs_r0();
	BX_CPU(cpu)->msr.sysenter_esp_msr = it->sysenter_esp_r0();
	BX_CPU(cpu)->msr.sysenter_eip_msr = it->sysenter_eip_r0();

	BX_CPU(cpu)->handleAlignmentCheck();
	BX_CPU(cpu)->handleCpuModeChange();
	BX_CPU(cpu)->handleInterruptMaskChange();
	BX_CPU(cpu)->handleSseModeChange();
	BX_CPU(cpu)->TLB_flush();

	if (::reven::util::verbose_level >= 3) {
		// Display the state after reset
		BX_CPU(cpu)->debug(BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx);
	}

	return true;
}

void Replayer::execute(unsigned cpu) {
	std::cerr << "Info: Launch execution" << std::endl;
	begin_time_ = std::chrono::steady_clock::now();

	// CPU Loop
	while (1) {
		BX_CPU(cpu)->cpu_loop();

		if (bx_pc_system.kill_bochs_request)
			break;
	}

	if (::reven::util::verbose_level >= 3) {
		// Display the state at the end of the execution
		BX_CPU(cpu)->debug(BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx);
	}
}

bool Replayer::is_final_int3(unsigned cpu, const bxInstruction_c *i) const {
	return i->getIaOpcode() == BX_IA_INT3 && BX_CPU(cpu)->gen_reg[2].rrx == 0xdeadbabe && BX_CPU(cpu)->gen_reg[0].rrx == 0xeff1cad1;
}

void Replayer::before_instruction(unsigned cpu, bxInstruction_c *i) {
	// Reset the current event
	current_event_ = reven::vmghost::sync_event();

	// We need to save i->execute1 and restore it later if we change it to not break the iCache
	current_instruction_ = i->execute1;
	current_rip_ = BX_CPU(cpu)->gen_reg[BX_64BIT_REG_RIP].rrx;

	// If we match the previous registered interrupt event, we launch an interrupt
	if (saved_interrupt_event_.is_valid && current_rip_ == saved_interrupt_event_.interrupt_rip) {
		apply_hardware_access(cpu, saved_interrupt_event_.start_context.tsc);

		LOG_WARN("Simulating an interrupt for Sync Event $" << std::dec << saved_interrupt_event_.position)
		BX_CPU(cpu)->interrupt(saved_interrupt_event_.interrupt_vector, BX_EXTERNAL_INTERRUPT, 0, saved_interrupt_event_.fault_error_code);

		saved_interrupt_event_ = reven::vmghost::sync_event();
		longjmp(BX_CPU(cpu)->jmp_buf_env, 0);
	}

	auto sync_event = sync_file_.current_event();

	// The event is invalid, so it's the end
	if (!sync_event.is_valid) {
		LOG_END_REPLAY(cpu, "No more valid sync points")
	}

	current_ctx_.rax = BX_CPU(cpu)->gen_reg[0].rrx;
	current_ctx_.rbx = BX_CPU(cpu)->gen_reg[3].rrx;
	current_ctx_.rcx = BX_CPU(cpu)->gen_reg[1].rrx;
	current_ctx_.rdx = BX_CPU(cpu)->gen_reg[2].rrx;
	current_ctx_.rsi = BX_CPU(cpu)->gen_reg[6].rrx;
	current_ctx_.rdi = BX_CPU(cpu)->gen_reg[7].rrx;
	current_ctx_.rbp = BX_CPU(cpu)->gen_reg[5].rrx;
	current_ctx_.rsp = BX_CPU(cpu)->gen_reg[4].rrx;
	current_ctx_.r8 = BX_CPU(cpu)->gen_reg[8].rrx;
	current_ctx_.r9 = BX_CPU(cpu)->gen_reg[9].rrx;
	current_ctx_.r10 = BX_CPU(cpu)->gen_reg[10].rrx;
	current_ctx_.r11 = BX_CPU(cpu)->gen_reg[11].rrx;
	current_ctx_.r12 = BX_CPU(cpu)->gen_reg[12].rrx;
	current_ctx_.r13 = BX_CPU(cpu)->gen_reg[13].rrx;
	current_ctx_.r14 = BX_CPU(cpu)->gen_reg[14].rrx;
	current_ctx_.r15 = BX_CPU(cpu)->gen_reg[15].rrx;
	current_ctx_.cr0 = BX_CPU(cpu)->cr0.val32;
	current_ctx_.cr2 = BX_CPU(cpu)->cr2;
	current_ctx_.cr3 = BX_CPU(cpu)->cr3;
	current_ctx_.cr4 = BX_CPU(cpu)->cr4.val32;
	current_ctx_.fpu_sw = BX_CPU(cpu)->the_i387.get_status_word();
	current_ctx_.fpu_cw = BX_CPU(cpu)->the_i387.get_control_word();
	current_ctx_.fpu_tags = BX_CPU(cpu)->pack_FPU_TW(BX_CPU(cpu)->the_i387.get_tag_word());

	// The first event is always applied at the beginning
	if (sync_event.is_first_event_context_unknown) {
		current_event_ = sync_event;
		last_sync_point_ = current_event_.position;
		sync_file_.next();

		LOG_MATCH_SYNC_EVENT(cpu, current_event_, sync_file_.sync_point_count(), begin_time_)
		apply_sync_event(cpu, sync_event);
	} else if (current_rip_ == sync_event.start_rip) {
		if (sync_event.start_context.are_values_equivalent(current_ctx_)) {
			current_event_ = sync_event;
			last_sync_point_ = current_event_.position;
			sync_file_.next();

			LOG_MATCH_SYNC_EVENT(cpu, current_event_, sync_file_.sync_point_count(), begin_time_)
		} else if (!sync_event.has_interrupt && match_with_no_eflags(current_ctx_, sync_event.start_context)) {
			current_event_ = sync_event;
			last_sync_point_ = current_event_.position;
			sync_file_.next();

			LOG_MATCH_SYNC_EVENT_EXTRA(cpu, current_event_, sync_file_.sync_point_count(), begin_time_, " without EFLAGS !")
		}
	}

	if (current_event_.is_valid) {
		if (is_final_int3(cpu, i)) {
			LOG_END_REPLAY(cpu, "Found a stopping int3 at " << std::showbase << std::hex << current_rip_)
		}

		if (current_event_.is_last_event) {
			LOG_END_REPLAY(cpu, "Reach the last sync point")
		}

		if (!current_event_.is_first_event_context_unknown) {
			apply_hardware_access(cpu, current_event_.start_context.tsc);
		}

		// If we want to emulate the instruction, we can just nop the instruction
		// We don't nop the instruction if it's the first, if we really need to nop it we will do it at the end of the function
		if (!current_event_.is_first_event_context_unknown && current_event_.is_instruction_emulation) {
			// We don't want to nop some instructions like WRMSR
			if (std::find(executed_instructions.cbegin(), executed_instructions.cend(), i->execute1) == executed_instructions.cend()
				&& std::find(emulated_instructions.cbegin(), emulated_instructions.cend(), i->execute1) != emulated_instructions.cend()) {
				i->execute1 = &BX_CPU_C::NOP;
			}
		}

		if (current_event_.has_interrupt) {
			// We just want to simulate interrupt (like IRQs), not exception (they will be generated by bochs)
			// We do use 15 as the limit even if Intel told us that exception are in the range [0; 31] because we can receive some interrupt from the APIC in this range
			if (current_event_.interrupt_vector >= 16) {
				// If the RIP doesn't match we can just postpone the execution of the interrupt by saving the sync_event in saved_interrupt_event_
				if (current_rip_ != current_event_.interrupt_rip) {
					LOG_WARN("Saving an interrupt for Sync Event $" << std::dec << current_event_.position)
					saved_interrupt_event_ = current_event_;
				} else {
					apply_hardware_access(cpu, current_event_.start_context.tsc);

					LOG_WARN("Simulating an interrupt for Sync Event $" << std::dec << current_event_.position)
					BX_CPU(cpu)->interrupt(current_event_.interrupt_vector, BX_EXTERNAL_INTERRUPT, 0, current_event_.fault_error_code);
					longjmp(BX_CPU(cpu)->jmp_buf_env, 0);
				}
			}
		}
	} else if (current_rip_ == sync_event.start_rip) {
		// TODO: Add some debug to see if we should have match this sync point or not
	}

	if (!current_event_.is_valid) {
		if (is_final_int3(cpu, i)) {
			LOG_DESYNC_SYNC_EVENT(cpu, sync_event)
			LOG_DESYNC(cpu, "The stopping int3 don't have an associated sync point, that means that we desync before")
		}
	}

	// If we know we don't handle the instruction, we can just nop it
	if (std::find(emulated_instructions.cbegin(), emulated_instructions.cend(), i->execute1) != emulated_instructions.cend()) {
		if (!current_event_.is_valid) {
			LOG_DESYNC_SYNC_EVENT(cpu, sync_event)
			LOG_DESYNC(cpu, "We can't execute this instruction without a sync_point")
		}

		// We don't want to nop some instructions like WRMSR
		if (std::find(executed_instructions.cbegin(), executed_instructions.cend(), i->execute1) == executed_instructions.cend()) {
			i->execute1 = &BX_CPU_C::NOP;
		}
	}
}

void Replayer::after_instruction(unsigned cpu, bxInstruction_c *i) {
	// Restore i->execute1 if we have nop it to not mess up the iCache
	i->execute1 = current_instruction_;

	if (current_event_.is_valid) {
		// Do we have an unmatched interrupt? current_event_ is unset when we match an interrupt
		if (current_event_.has_interrupt && current_rip_ == current_event_.interrupt_rip) {
			LOG_DESYNC_SYNC_EVENT(cpu, current_event_)
			LOG_DESYNC(cpu, "Exception " << std::dec << static_cast<uint32_t>(current_event_.interrupt_vector)
			                << " not generated by bochs with error code " << std::dec << current_event_.fault_error_code)
		}

		// Initial event have already been applied
		if (!current_event_.is_first_event_context_unknown)
			apply_sync_event(cpu, current_event_);

		current_event_ = reven::vmghost::sync_event();
	}
}

void Replayer::exception(unsigned cpu, unsigned vector, unsigned error_code) {
	auto sync_event = sync_file_.current_event();

	// When we are having a code pagefault we didn't match the sync event in before_instruction so we need to match it now
	if (!current_event_.is_valid) {
		current_ctx_.rax = BX_CPU(cpu)->gen_reg[0].rrx;
		current_ctx_.rbx = BX_CPU(cpu)->gen_reg[3].rrx;
		current_ctx_.rcx = BX_CPU(cpu)->gen_reg[1].rrx;
		current_ctx_.rdx = BX_CPU(cpu)->gen_reg[2].rrx;
		current_ctx_.rsi = BX_CPU(cpu)->gen_reg[6].rrx;
		current_ctx_.rdi = BX_CPU(cpu)->gen_reg[7].rrx;
		current_ctx_.rbp = BX_CPU(cpu)->gen_reg[5].rrx;
		current_ctx_.rsp = BX_CPU(cpu)->gen_reg[4].rrx;
		current_ctx_.r8 = BX_CPU(cpu)->gen_reg[8].rrx;
		current_ctx_.r9 = BX_CPU(cpu)->gen_reg[9].rrx;
		current_ctx_.r10 = BX_CPU(cpu)->gen_reg[10].rrx;
		current_ctx_.r11 = BX_CPU(cpu)->gen_reg[11].rrx;
		current_ctx_.r12 = BX_CPU(cpu)->gen_reg[12].rrx;
		current_ctx_.r13 = BX_CPU(cpu)->gen_reg[13].rrx;
		current_ctx_.r14 = BX_CPU(cpu)->gen_reg[14].rrx;
		current_ctx_.r15 = BX_CPU(cpu)->gen_reg[15].rrx;
		current_ctx_.cr0 = BX_CPU(cpu)->cr0.val32;
		current_ctx_.cr2 = BX_CPU(cpu)->cr2;
		current_ctx_.cr3 = BX_CPU(cpu)->cr3;
		current_ctx_.cr4 = BX_CPU(cpu)->cr4.val32;
		current_ctx_.fpu_sw = BX_CPU(cpu)->the_i387.get_status_word();
		current_ctx_.fpu_cw = BX_CPU(cpu)->the_i387.get_control_word();
		current_ctx_.fpu_tags = BX_CPU(cpu)->pack_FPU_TW(BX_CPU(cpu)->the_i387.get_tag_word());

		// The event is invalid, so it's the end
		if (!sync_event.is_valid) {
			LOG_END_REPLAY(cpu, "No more valid sync points")
		}

		if (BX_CPU(cpu)->prev_rip == sync_event.interrupt_rip) {
			if (sync_event.start_context.are_values_equivalent(current_ctx_)) {
				current_event_ = sync_event;
				last_sync_point_ = current_event_.position;
				sync_file_.next();

				LOG_MATCH_SYNC_EVENT_EXTRA(cpu, current_event_, sync_file_.sync_point_count(), begin_time_, " during an exception")
			} else if (match_with_no_eflags(current_ctx_, sync_event.start_context)) {
				current_event_ = sync_event;
				last_sync_point_ = current_event_.position;
				sync_file_.next();

				LOG_MATCH_SYNC_EVENT_EXTRA(cpu, current_event_, sync_file_.sync_point_count(), begin_time_, " during an exception without EFLAGS !")
			}
		}

		if (!current_event_.is_valid) {
			// TODO: Add some debug to see if we should have match this sync point or not
		}
	}

	if (!current_event_.is_valid || !current_event_.has_interrupt || current_event_.interrupt_vector != vector
		|| (exception_do_have_error_code(vector) && current_event_.fault_error_code != error_code)) {
		LOG_DESYNC_SYNC_EVENT(cpu, (current_event_.is_valid) ? current_event_ : sync_event)
		LOG_DESYNC(cpu, "Unmatched exception " << std::dec << vector << " with error code " << std::dec << error_code)
	}
}

void Replayer::interrupt(unsigned cpu, unsigned vector) {
	// When we are having an APIC interrupt we didn't match the sync event in before_instruction so we need to match it now
	if (!current_event_.is_valid) {
		current_ctx_.rax = BX_CPU(cpu)->gen_reg[0].rrx;
		current_ctx_.rbx = BX_CPU(cpu)->gen_reg[3].rrx;
		current_ctx_.rcx = BX_CPU(cpu)->gen_reg[1].rrx;
		current_ctx_.rdx = BX_CPU(cpu)->gen_reg[2].rrx;
		current_ctx_.rsi = BX_CPU(cpu)->gen_reg[6].rrx;
		current_ctx_.rdi = BX_CPU(cpu)->gen_reg[7].rrx;
		current_ctx_.rbp = BX_CPU(cpu)->gen_reg[5].rrx;
		current_ctx_.rsp = BX_CPU(cpu)->gen_reg[4].rrx;
		current_ctx_.r8 = BX_CPU(cpu)->gen_reg[8].rrx;
		current_ctx_.r9 = BX_CPU(cpu)->gen_reg[9].rrx;
		current_ctx_.r10 = BX_CPU(cpu)->gen_reg[10].rrx;
		current_ctx_.r11 = BX_CPU(cpu)->gen_reg[11].rrx;
		current_ctx_.r12 = BX_CPU(cpu)->gen_reg[12].rrx;
		current_ctx_.r13 = BX_CPU(cpu)->gen_reg[13].rrx;
		current_ctx_.r14 = BX_CPU(cpu)->gen_reg[14].rrx;
		current_ctx_.r15 = BX_CPU(cpu)->gen_reg[15].rrx;
		current_ctx_.cr0 = BX_CPU(cpu)->cr0.val32;
		current_ctx_.cr2 = BX_CPU(cpu)->cr2;
		current_ctx_.cr3 = BX_CPU(cpu)->cr3;
		current_ctx_.cr4 = BX_CPU(cpu)->cr4.val32;
		current_ctx_.fpu_sw = BX_CPU(cpu)->the_i387.get_status_word();
		current_ctx_.fpu_cw = BX_CPU(cpu)->the_i387.get_control_word();
		current_ctx_.fpu_tags = BX_CPU(cpu)->pack_FPU_TW(BX_CPU(cpu)->the_i387.get_tag_word());

		auto sync_event = sync_file_.current_event();

		// The event is invalid, so it's the end
		if (!sync_event.is_valid) {
			LOG_END_REPLAY(cpu, "No more valid sync points")
		}

		if (BX_CPU(cpu)->prev_rip == sync_event.interrupt_rip) {
			if (sync_event.start_context.are_values_equivalent(current_ctx_)) {
				current_event_ = sync_event;
				last_sync_point_ = current_event_.position;
				sync_file_.next();

				LOG_MATCH_SYNC_EVENT_EXTRA(cpu, current_event_, sync_file_.sync_point_count(), begin_time_, " during an interrupt")
			} else if (match_with_no_eflags(current_ctx_, sync_event.start_context)) {
				current_event_ = sync_event;
				last_sync_point_ = current_event_.position;
				sync_file_.next();

				LOG_MATCH_SYNC_EVENT_EXTRA(cpu, current_event_, sync_file_.sync_point_count(), begin_time_, "during an interrupt without EFLAGS !")
			}
		}

		if (!current_event_.is_valid) {
			// TODO: Add some debug to see if we should have match this sync point or not
		} else {
			if (!current_event_.has_interrupt || current_event_.interrupt_vector != vector) {
				LOG_DESYNC_SYNC_EVENT(cpu, current_event_)
				LOG_DESYNC(cpu, "Unmatched interrupt " << std::dec << vector)
			}
		}
	}

	if (!current_event_.is_valid || !current_event_.has_interrupt || current_event_.interrupt_vector != vector || current_event_.interrupt_rip != BX_CPU(cpu)->prev_rip) {
		// We didn't match this interrupt, can happens in case of software interrupt without vmexit
		return;
	}

	#define UPDATE_FLAG_VALUE(name, value)                                      \
		if (BX_CPU(cpu)->getB_##name() != (value)) {                            \
			LOG_WARN("Forcing " << #name)                                       \
			BX_CPU(cpu)->set_##name(value);                                     \
		}

	UPDATE_FLAG_VALUE(CF, (current_event_.rflags >> 0) & 1);
	UPDATE_FLAG_VALUE(PF, (current_event_.rflags >> 2) & 1);
	UPDATE_FLAG_VALUE(AF, (current_event_.rflags >> 4) & 1);
	UPDATE_FLAG_VALUE(ZF, (current_event_.rflags >> 6) & 1);
	UPDATE_FLAG_VALUE(SF, (current_event_.rflags >> 7) & 1);
	UPDATE_FLAG_VALUE(TF, (current_event_.rflags >> 8) & 1);
	UPDATE_FLAG_VALUE(IF, (current_event_.rflags >> 9) & 1);
	UPDATE_FLAG_VALUE(DF, (current_event_.rflags >> 10) & 1);
	UPDATE_FLAG_VALUE(OF, (current_event_.rflags >> 11) & 1);

	const auto expected_IOPL = (current_event_.rflags >> 12) & 0b11;
	if (BX_CPU(cpu)->get_IOPL() != expected_IOPL) {
		LOG_WARN("Forcing IOPL")
		BX_CPU(cpu)->set_IOPL((current_event_.rflags >> 12) & 3);
	}

	#undef UPDATE_FLAG_VALUE

	apply_hardware_access(cpu, current_event_.new_context.tsc);

	// Reseting the current_event_
	current_event_ = reven::vmghost::sync_event();
}

void Replayer::linear_access(unsigned cpu, std::uint64_t address, unsigned rw) {
	// This PF is certainly legit, even if not detected by bochs because of TLB differences
	if (current_event_.is_valid && current_event_.has_interrupt && current_rip_ == current_event_.interrupt_rip && current_event_.interrupt_vector == 0xE) {
		bool access_is_write = rw == BX_WRITE || rw == BX_RW;
		bool pagefault_is_write = current_event_.fault_error_code & 0x2;

		// Is it really the same kind of access?
		// Check the linear address with the real one and if it's also a read or write operation
		if (address != current_event_.new_context.cr2 || pagefault_is_write != access_is_write) {
			return;
		}

		LOG_WARN("Forcing pagefault at address " << std::showbase << std::hex << address << " for Sync Event $" << std::dec << current_event_.position)
		BX_CPU(cpu)->page_fault(current_event_.fault_error_code, address, 0, rw);
	}
}

void Replayer::apply_sync_event(unsigned cpu, const reven::vmghost::sync_event& sync_event) {
	apply_hardware_access(cpu, sync_event.new_context.tsc);

	if (sync_event.is_instruction_emulation) {
		LOG_INFO("Forcing emulation for Sync Event $" << std::dec << sync_event.position);

		BX_CPU(cpu)->gen_reg[0].rrx = sync_event.new_context.rax;
		BX_CPU(cpu)->gen_reg[3].rrx = sync_event.new_context.rbx;
		BX_CPU(cpu)->gen_reg[1].rrx = sync_event.new_context.rcx;
		BX_CPU(cpu)->gen_reg[2].rrx = sync_event.new_context.rdx;
		BX_CPU(cpu)->gen_reg[6].rrx = sync_event.new_context.rsi;
		BX_CPU(cpu)->gen_reg[7].rrx = sync_event.new_context.rdi;
		BX_CPU(cpu)->gen_reg[5].rrx = sync_event.new_context.rbp;
		BX_CPU(cpu)->gen_reg[4].rrx = sync_event.new_context.rsp;
		BX_CPU(cpu)->gen_reg[8].rrx = sync_event.new_context.r8;
		BX_CPU(cpu)->gen_reg[9].rrx = sync_event.new_context.r9;
		BX_CPU(cpu)->gen_reg[10].rrx = sync_event.new_context.r10;
		BX_CPU(cpu)->gen_reg[11].rrx = sync_event.new_context.r11;
		BX_CPU(cpu)->gen_reg[12].rrx = sync_event.new_context.r12;
		BX_CPU(cpu)->gen_reg[13].rrx = sync_event.new_context.r13;
		BX_CPU(cpu)->gen_reg[14].rrx = sync_event.new_context.r14;
		BX_CPU(cpu)->gen_reg[15].rrx = sync_event.new_context.r15;
		BX_CPU(cpu)->cr0.val32 = sync_event.new_context.cr0;
		BX_CPU(cpu)->cr2 = sync_event.new_context.cr2;
		BX_CPU(cpu)->cr3 = sync_event.new_context.cr3;
		BX_CPU(cpu)->cr4.val32 = sync_event.new_context.cr4;

		BX_CPU(cpu)->the_i387.cwd = sync_event.new_context.fpu_cw;
		BX_CPU(cpu)->the_i387.twd = BX_CPU(cpu)->unpack_FPU_TW(sync_event.new_context.fpu_tags);
		BX_CPU(cpu)->the_i387.swd = sync_event.new_context.fpu_sw;
		BX_CPU(cpu)->the_i387.tos = BX_CPU(cpu)->the_i387.swd >> 11;
	}
}

void Replayer::apply_hardware_access(unsigned cpu, uint64_t tsc) {
	// We assume that we can't read more than 100KB in one access (We did see access of 92KB)
	static const std::size_t read_data_size = 100 * 1024;
	static std::uint8_t read_data[read_data_size];

	BX_CPU(cpu)->set_TSC(tsc);

	while (hardware_file_.current().valid() and hardware_file_.current().tsc <= tsc) {
		const auto& access = hardware_file_.current();

		if (!access.is_write() && access.data.size() > read_data_size) {
			LOG_DESYNC_HARDWARE_ACCESS(cpu, access)
			LOG_DESYNC(cpu, "Encountered a read access bigger than the maximum size");
		}

		if (access.is_port()) {
			if (access.is_write()) {
				// The device writes to main memory: this is an INS
				auto laddr = BX_CPU(cpu)->get_laddr(BX_SEG_REG_ES, BX_CPU(cpu)->gen_reg[7].rrx); // ES:EDI
				if (BX_CPU(cpu)->access_write_linear(laddr, access.data.size(), BX_CPU(cpu)->sregs[BX_SEG_REG_ES].cache.dpl, 0x0, const_cast<unsigned char*>(access.data.data())) == -1) {
					LOG_ERROR("Can't apply write access to " << std::hex << laddr << " (" << std::dec << access.data.size() << " bytes)")
				}
			} else {
				auto laddr = BX_CPU(cpu)->get_laddr(BX_SEG_REG_DS, BX_CPU(cpu)->gen_reg[6].rrx); // DS:ESI

				if (BX_CPU(cpu)->access_read_linear(laddr, access.data.size(), BX_CPU(cpu)->sregs[BX_SEG_REG_DS].cache.dpl, BX_READ, 0x0, read_data) == -1) {
					LOG_ERROR("Can't apply read access to " << std::hex << laddr << " (" << std::dec << access.data.size() << " bytes)")
				} else if (std::memcmp(read_data, access.data.data(), access.data.size()) != 0) {
					LOG_WARN("Difference of memory when applying read access to " << std::hex << laddr << " (" << std::dec << access.data.size() << " bytes)")
				}
			}
		} else if (!access.is_mmio()) {
			if (access.is_write()) {
				DEV_MEM_WRITE_PHYSICAL_DMA(access.physical_address, access.data.size(), const_cast<unsigned char*>(access.data.data()));
			} else {
				DEV_MEM_READ_PHYSICAL_DMA(access.physical_address, access.data.size(), read_data);

				if (std::memcmp(read_data, access.data.data(), access.data.size()) != 0) {
					LOG_WARN("Difference of memory when applying read access to " << std::hex << access.physical_address << " (" << std::dec << access.data.size() << " bytes)")
				}
			}
		}

		hardware_file_.next();
	}
}

void Replayer::end_of_scenario(unsigned cpu, bool desync) {
	std::chrono::duration<double> elapsed_seconds = std::chrono::steady_clock::now() - begin_time_;
	std::cerr << "Info: End of the scenario after " << std::dec << BX_CPU(cpu)->icount << " instructions"
	          << " (at " << std::dec << static_cast<uint64_t>(BX_CPU(cpu)->icount / elapsed_seconds.count()) << " Hz)" << std::endl;
	std::cerr << "Info: Last validated sync point: $" << last_sync_point_ << std::endl;

	desync_ = desync;

	BX_CPU(cpu)->async_event = 1;
	bx_pc_system.kill_bochs_request = 1;

	longjmp(BX_CPU(cpu)->jmp_buf_env, 1);
}

size_t Replayer::get_memory_size() const {
	size_t size = 0;

	core_.physical_memory()->visit_chunks([&size](const reven::vmghost::MemoryChunk& chunk) {
		if (chunk.physical_address() == 0) {
			size = chunk.size_in_memory();
		}
	});

	if (size == 0) {
		throw std::runtime_error("Can't retrieve the size of the memory!");
	}

	return size;
}

const std::vector<Replayer::MemoryRange>& Replayer::get_memory_ranges() const {
	return ranges_;
}

void Replayer::device_memory_read(bx_phy_address addr, unsigned len, uint8_t *data) const {
	for (auto& range : ranges_) {
		if (addr >= range.start_address && addr - range.start_address < range.size) {
			for (unsigned i = 0; i < len; ++i) {
				*(data + i) = *(range.memory + (addr - range.start_address) + i);
			}

			break;
		}
	}
}

void Replayer::device_memory_write(bx_phy_address addr, unsigned len, uint8_t *data) {
	for (auto& range : ranges_) {
		if (addr >= range.start_address && addr - range.start_address < range.size) {
			for (unsigned i = 0; i < len; ++i) {
				*(range.memory + (addr - range.start_address) + i) = *(data + i);
			}

			break;
		}
	}
}

}
}
