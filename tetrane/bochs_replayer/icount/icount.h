#pragma once

#include <experimental/optional>

extern reven::replayer::Replayer replayer;

namespace reven {
namespace icount {

class ICount {
public:
	ICount() = default;
	explicit ICount(std::uint64_t max_icount): max_icount_(max_icount) {};

	// This method should not be called when icount_ could be 0 (before the launch of the execution)
	std::uint64_t icount() const {
		if (icount_ == 0)
			throw std::logic_error("Call to icount before initialization.");
		return icount_ - 1;
	}

	void before_instruction() {
		check_max_icount();
		++icount_;
	}

	void break_and_start_new_instruction() {
		check_max_icount();
		++icount_;
	}

private:

	void check_max_icount() {
		/* multiple necessary checks before ending the replay:
		 *  * replay is started
		 *  * there is a max_icount specified for the replay
		 *  * the max_icount has been reached
		 * then we can end the replay
		 */
		if (icount_ != 0 && max_icount_ && icount() > max_icount_.value()) {
			::replayer.end_of_scenario(0, false);
		}
	}

	std::uint64_t icount_ = 0; // The next instruction tick
	std::experimental::optional<std::uint64_t> max_icount_; // The number of instructions to replay. If not set, means replay all instructions
};

}
}
