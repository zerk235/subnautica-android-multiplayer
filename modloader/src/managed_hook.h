#pragma once

// Intercepts managed (C#) methods. Android forbids PROT_EXEC on file-backed pages, so the
// only reliable hook point is data: the MethodInfo pointers and the class vtable, which live
// in writable memory. No executable page is ever touched.
bool install_managed_hooks();
