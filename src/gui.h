// The FLTK window: log, progress bars, Start and Cancel.
#ifndef IGWL_GUI_H
#define IGWL_GUI_H

#include "sync.h"

namespace igwl {

// Shows the launcher window and runs the FLTK event loop until it closes.
// Start runs the sync on a worker thread; once it succeeds, the program the
// manifest names for this platform is started (unless `launch` is false)
// and the window closes. Returns the process exit code.
int run_gui(const SyncOptions& options, bool launch);

}  // namespace igwl

#endif
