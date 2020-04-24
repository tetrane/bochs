#pragma once

#include <chrono>
#include <iostream>

namespace reven {
namespace vmghost {
	struct sync_event;
	struct hardware_access;
}

namespace util {

extern std::uint8_t verbose_level;

void display_hardware_access(unsigned cpu, const reven::vmghost::hardware_access& access);
void display_sync_event(unsigned cpu, const reven::vmghost::sync_event& sync_event);
void display_progress(unsigned cpu, const reven::vmghost::sync_event& sync_event,
                      std::uint64_t nb_sync_point, const std::chrono::steady_clock::time_point& begin_time);

}
}

// Verbosity 0 and above

#define LOG_DESYNC(cpu, message) \
	do { \
		std::cerr << "Desync: " << message << std::endl; \
		end_of_scenario(cpu, true); \
	} while(0);

#define LOG_FATAL_ERROR(message) \
	do { \
		std::cerr << "Fatal Error: " << message << std::endl; \
	} while(0);

#define LOG_ERROR(message) \
	do { \
		std::cerr << "Error: " << message << std::endl; \
	} while(0);

#define LOG_END_REPLAY(cpu, message) \
	do { \
		std::cerr << "Info: " << message << std::endl; \
		end_of_scenario(cpu, false); \
	} while(0);

// Verbosity 1 and above

#define LOG_DESYNC_HARDWARE_ACCESS(cpu, access) \
	do { \
		if (::reven::util::verbose_level >= 1) \
			::reven::util::display_hardware_access(cpu, access); \
	} while(0);

#define LOG_DESYNC_SYNC_EVENT(cpu, sync_event) \
	do { \
		if (::reven::util::verbose_level >= 1) \
			::reven::util::display_sync_event(cpu, sync_event); \
	} while(0);

#define LOG_WARN(message) \
	do { \
		if (::reven::util::verbose_level >= 1) \
			std::cerr << "Warning: " << message << std::endl; \
	} while(0);

// Verbosity 2 and above

#define LOG_MATCH_SYNC_EVENT_EXTRA(cpu, sync_event, nb_sync_point, begin_time, message) \
	do { \
		if (::reven::util::verbose_level >= 2) \
			std::cerr << "Warning: Matching Sync event $" << std::dec << (sync_event).position \
			          << " at #" << std::dec << reven_icount() \
			          << message \
			          << std::endl; \
		::reven::util::display_progress(cpu, sync_event, nb_sync_point, begin_time); \
	} while(0);

// Verbosity 3 and above

#define LOG_INFO(message) \
	do { \
		if (::reven::util::verbose_level >= 3) \
			std::cerr << "Info: " << message << std::endl; \
	} while(0);

#define LOG_MATCH_SYNC_EVENT(cpu, sync_event, nb_sync_point, begin_time) \
	do { \
		if (::reven::util::verbose_level >= 3) \
			std::cerr << "Info: Matching Sync event $" << std::dec << (sync_event).position \
			          << " at #" << std::dec << reven_icount() \
			          << std::endl; \
		::reven::util::display_progress(cpu, sync_event, nb_sync_point, begin_time); \
	} while(0);
