#pragma once
#include "common.hpp"

namespace ss::player {

	bool init();
	void on_frame();
	void request_spawn();
	bool has_player2();
	void dump_info();

} // namespace ss::player