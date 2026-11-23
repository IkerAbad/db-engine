#pragma once

#include <string_view>

namespace db::fault {

// Deterministic crash injection used by the recovery test suite.
//
// DBENGINE_CRASH_AT   comma separated point names, for example
//                     "wal.append.after,page.write.half"
// DBENGINE_CRASH_COUNT
//                     hit the configured point on the Nth occurrence (default 1)
//
// When a point fires the process terminates immediately with exit code 86,
// without flushing user-space buffers and without running destructors.
bool enabled(std::string_view point);
void hit(std::string_view point);

void reset_for_test();

}  // namespace db::fault
