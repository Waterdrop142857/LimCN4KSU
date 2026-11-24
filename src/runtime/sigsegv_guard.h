// SIGSEGV chaining guard for the official game process. Ported from
// IORelocator64.cpp: the 471 AppSealing unwind-recovery branch plus POSIX
// handler chaining and re-wrapping after AppSealing's direct rt_sigaction.
#pragma once

namespace bybus::runtime {

bool install_sigsegv_guard();
void maintain_sigsegv_guard();

} // namespace bybus::runtime
