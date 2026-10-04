#pragma once
// A few threads shared by all the decoders, for the parts of one frame that can be done side by side:
// the chunks of a HAP frame, the rows of texture blocks decoded on the CPU when the GPU cannot.
// The calling thread works too, so a call never waits for a worker that is busy elsewhere.

#include <functional>

namespace workers {

// Runs fn(0) .. fn(count - 1), spread over the pool and the calling thread. Returns once all are done.
void parallelFor(int count, const std::function<void(int)> &fn);

// Threads of the pool, the calling one not included
int threadCount();

} // namespace workers
