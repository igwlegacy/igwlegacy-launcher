// The FLTK window: server URL, log, progress bars, Start and Cancel.
#ifndef IGWL_GUI_H
#define IGWL_GUI_H

#include "sync.h"

#include <string>

namespace igwl {

// What the window needs to know beyond the sync options.
struct GuiContext {
    fs::path settings_file;      // where a changed server URL is saved
    std::string settings_error;  // why loading the settings failed, if it did
};

// Shows the launcher window and runs the FLTK event loop until it closes.
// The server URL field starts out with `options.base_url`; Start saves it
// to the settings file if it was changed, then runs the sync on a worker
// thread. Once that succeeds, the program the manifest names for this
// platform is started (unless `launch` is false) and the window closes.
// Returns the process exit code.
int run_gui(const SyncOptions& options, const GuiContext& context, bool launch);

}  // namespace igwl

#endif
