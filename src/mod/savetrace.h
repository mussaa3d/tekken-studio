#pragma once
#include <string>

namespace savetrace {

void Start();
// Logs engine events. `words` (comma separated) picks the functions logged every time, with their parameters' first bytes and the
// native call path; empty keeps the default of Rewind, Jump and Replay.
void TraceEvents(bool on, const std::string& words = {});

} // namespace savetrace
