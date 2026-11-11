#pragma once

// Scheduling-priority overrides for the runtime's own threads. Apple platforms only; a no-op
// everywhere else.
namespace ThreadPriority {

// Applies "<priority>[rr]" to the calling thread: a Darwin scheduling priority (15..47), time-shared
// by default or fixed-priority round-robin with the "rr" suffix. The value of `environmentVariable`
// replaces `defaultSpec` when it is set; an empty spec or "off" leaves the thread untouched. The
// thread's scheduling state before and after is logged under `label`.
void ApplyToCurrentThread(const char* label, const char* environmentVariable, const char* defaultSpec);

} // namespace ThreadPriority
