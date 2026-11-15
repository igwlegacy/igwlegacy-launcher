#include "gui.h"

#include "platform.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>

#if defined(__linux__)
#include <fontconfig/fontconfig.h>
#endif

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace igwl {

namespace {

// FLTK is not thread-safe. The sync thread only ever touches widgets while
// holding the global FLTK lock (Fl::lock), which the main thread releases
// whenever it waits for events, and then wakes the main thread with
// Fl::awake() so the change gets drawn. Everything else (button callbacks,
// starting and joining the thread) happens on the main thread.
class LauncherWindow : public SyncObserver {
public:
    LauncherWindow(const SyncOptions& options, bool launch)
        : options_(options),
          launch_(launch),
          cancel_(false),
          running_(false),
          close_requested_(false),
          exit_code_(0) {
        window_ = new Fl_Double_Window(600, 380, "IGW Legacy Launcher " IGWL_VERSION);
        window_->callback(on_close, this);

        status_ = new Fl_Box(15, 10, 570, 30, "Press Start to update and launch the game.");
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        status_->labelfont(FL_HELVETICA_BOLD);

        log_ = new Fl_Browser(15, 45, 570, 200);
        log_->textsize(12);

        current_ = new Fl_Progress(15, 265, 570, 20, "Current file");
        current_->align(FL_ALIGN_TOP_LEFT);
        current_->minimum(0);
        current_->maximum(1);
        current_->selection_color(FL_BLUE);
        current_->labelsize(12);

        overall_ = new Fl_Progress(15, 305, 570, 20, "Overall");
        overall_->align(FL_ALIGN_TOP_LEFT);
        overall_->minimum(0);
        overall_->maximum(1);
        overall_->selection_color(FL_DARK_GREEN);
        overall_->labelsize(12);

        cancel_button_ = new Fl_Button(385, 340, 95, 28, "Quit");
        cancel_button_->callback(on_cancel, this);

        start_button_ = new Fl_Return_Button(490, 340, 95, 28, "Start");
        start_button_->callback(on_start, this);

        window_->resizable(log_);
        window_->end();

        log("Game directory: " + options_.root.u8string());
        log("Server: " + options_.base_url);
    }

    ~LauncherWindow() {
        delete window_;
    }

    int run() {
        window_->show();
        Fl::run();
        return exit_code_;
    }

    // --- SyncObserver, called on the sync thread --------------------------

    void log(const std::string& line) override {
        Fl::lock();
        // '@' starts a formatting code in Fl_Browser lines; "@." ends the
        // codes, so the text is shown verbatim.
        log_->add(("@." + line).c_str());
        log_->bottomline(log_->size());
        Fl::unlock();
        Fl::awake();
    }

    void status(const std::string& text) override {
        Fl::lock();
        status_->copy_label(text.c_str());
        Fl::unlock();
        Fl::awake();
    }

    void progress(double current, double overall) override {
        Fl::lock();
        set_progress(current_, current);
        set_progress(overall_, overall);
        Fl::unlock();
        Fl::awake();
    }

private:
    static void set_progress(Fl_Progress* bar, double value) {
        bar->value(static_cast<float>(value));
        char text[16];
        std::snprintf(text, sizeof text, "%.0f%%", value * 100.0);
        bar->copy_label(text);
        // The percentage replaces the caption while a sync runs; keep it
        // readable inside the bar instead of above it.
        bar->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    // --- Main thread ------------------------------------------------------

    static void on_start(Fl_Widget*, void* data) {
        static_cast<LauncherWindow*>(data)->start();
    }

    static void on_cancel(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        if (self->running_) {
            self->cancel_ = true;
            self->cancel_button_->deactivate();
            self->status_->copy_label("Cancelling...");
        } else {
            self->window_->hide();
        }
    }

    // Closing the window while a sync runs only requests cancellation; the
    // window goes away once the thread has finished (see on_finished).
    // Hiding it right away would end Fl::run() with the thread still alive.
    static void on_close(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        // Escape also triggers the window callback; treat it like Quit.
        if (self->running_) {
            self->close_requested_ = true;
            self->cancel_ = true;
            self->status_->copy_label("Cancelling...");
        } else {
            self->window_->hide();
        }
    }

    void start() {
        running_ = true;
        cancel_ = false;
        start_button_->deactivate();
        cancel_button_->label("Cancel");
        cancel_button_->activate();
        set_progress(current_, 0.0);
        set_progress(overall_, 0.0);

        worker_ = std::thread([this]() {
            result_ = run_sync(options_, *this, cancel_);
            // Hand the result over to the main thread; joining the thread
            // and touching the buttons happens there.
            Fl::awake(on_finished, this);
        });
    }

    static void on_finished(void* data) {
        static_cast<LauncherWindow*>(data)->finished();
    }

    void finished() {
        worker_.join();
        running_ = false;

        if (close_requested_) {
            window_->hide();
            return;
        }

        if (result_.ok) {
            if (!launch_ || result_.launch.empty()) {
                status_->copy_label(result_.launch.empty()
                                        ? "Up to date. The manifest names no program to start here."
                                        : "Up to date.");
            } else {
                std::string error;
                if (platform::launch(result_.launch, options_.root, error)) {
                    window_->hide();
                    return;
                }
                log(error);
                status_->copy_label("Up to date, but the game could not be started.");
                exit_code_ = 1;
            }
        } else if (result_.cancelled) {
            log("Cancelled.");
            status_->copy_label("Cancelled. Press Start to try again.");
        } else {
            log("Error: " + result_.error);
            status_->copy_label("Update failed. Press Start to try again.");
            exit_code_ = 1;
            fl_alert("Update failed:\n\n%s", result_.error.c_str());
        }

        start_button_->label("Start");
        start_button_->activate();
        cancel_button_->label("Quit");
        cancel_button_->activate();
    }

    const SyncOptions& options_;
    const bool launch_;

    Fl_Double_Window* window_;
    Fl_Box* status_;
    Fl_Browser* log_;
    Fl_Progress* current_;
    Fl_Progress* overall_;
    Fl_Button* cancel_button_;
    Fl_Return_Button* start_button_;

    std::thread worker_;
    std::atomic<bool> cancel_;
    SyncResult result_;
    bool running_;
    bool close_requested_;
    int exit_code_;
};

}  // namespace

int run_gui(const SyncOptions& options, bool launch) {
    // Enables FLTK's locking support; the main thread holds the lock from
    // here on, except while Fl::run() waits for events.
    Fl::lock();
#if defined(__linux__)
    // FLTK's Xft text drawing uses fontconfig without initialising it, which
    // recent fontconfig releases complain about on stderr at startup.
    FcInit();
#endif
    Fl::scheme("gtk+");

    LauncherWindow window(options, launch);
    return window.run();
}

}  // namespace igwl
