#include "gui.h"

#include "platform.h"
#include "settings.h"

#include "igwl_config.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>

#if defined(__linux__)
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace igwl {

namespace {

// FLTK's right-click handling in text widgets keeps the selection only
// when the click lands inside it; elsewhere it selects the clicked word,
// or clears the selection when the click is past the end of a line. So
// selecting all and right-clicking an empty spot lost the selection
// before it could be copied. The widgets below therefore show the menu
// themselves while text is selected and leave the selection alone;
// without a selection FLTK's default behaviour still applies.
//
// They also keep to one visible selection per window, as the X11 primary
// selection suggests: when the focus moves to another widget of the same
// window, the widget losing it drops its selection. When the window as a
// whole loses the focus (switching to another program, a dialog), the
// focus becomes empty or lies in another window, and the selection stays
// so it can still be pasted elsewhere.

enum EditAction { kNone, kCut, kCopy, kPaste };

// Pops up the context menu for a selection at the mouse position; Cut
// and Paste are only offered when the text can be changed.
EditAction selection_menu(bool editable) {
    // Three items plus the all-zero terminator FLTK menus end with.
    Fl_Menu_Item menu[4] = {};
    menu[0].label(Fl_Input::cut_menu_text);
    menu[1].label(Fl_Input::copy_menu_text);
    menu[2].label(Fl_Input::paste_menu_text);
    if (!editable) {
        menu[0].deactivate();
        menu[2].deactivate();
    }
    const Fl_Menu_Item* picked = menu[0].popup(Fl::event_x(), Fl::event_y());
    if (picked == &menu[0]) return kCut;
    if (picked == &menu[1]) return kCopy;
    if (picked == &menu[2]) return kPaste;
    return kNone;
}

// Whether a widget that just got FL_UNFOCUS lost the focus to another
// widget of its own window (Fl::focus() already names the new one).
bool focus_moved_within_window(const Fl_Widget* widget) {
    const Fl_Widget* focus = Fl::focus();
    return focus && focus != widget && focus->top_window() == widget->top_window();
}

// Moves the focus to a widget as its own left-click would, so keyboard
// shortcuts such as Ctrl+C reach it after a right-click.
void focus_on_click(Fl_Widget* widget) {
    if (Fl::focus() != widget) {
        Fl::focus(widget);
        widget->handle(FL_FOCUS);
    }
}

// The read-only log.
class LogDisplay : public Fl_Text_Display {
public:
    LogDisplay(int x, int y, int w, int h) : Fl_Text_Display(x, y, w, h) {}

    int handle(int event) override {
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && buffer() &&
            buffer()->selected() &&
            // Clicks on the scrollbars are left to the default handling.
            Fl::event_inside(text_area.x, text_area.y, text_area.w, text_area.h)) {
            focus_on_click(this);
            if (selection_menu(false) == kCopy) {
                char* text = buffer()->selection_text();
                Fl::copy(text, static_cast<int>(std::strlen(text)), 1);
                std::free(text);
            }
            return 1;
        }
        if (event == FL_UNFOCUS && buffer() && focus_moved_within_window(this)) {
            buffer()->unselect();
        }
        return Fl_Text_Display::handle(event);
    }
};

// The editable server URL field.
//
// Fl_Input's double-click selects everything between spaces, which for a
// URL is the whole text. Like in the log, a double-click selects just
// the word under the mouse here, and dragging on from it extends the
// selection word by word; a triple-click still selects all of it.
//
// Fl_Input also draws a line with a selection in three pieces (before,
// inside and after it), each starting at a position FLTK rounds to whole
// logical pixels before applying the screen scale. With a fractional
// scale (e.g. Xft.dpi 128, scale 1.33) the pieces then land up to a
// device pixel away from where they sit in the unbroken text, so the
// text jumps around while selecting. draw() repaints a selected line as
// one piece instead.
class UrlInput : public Fl_Input {
public:
    UrlInput(int x, int y, int w, int h, const char* label)
        : Fl_Input(x, y, w, h, label), anchor_start_(0), anchor_end_(0) {}

    void draw() override {
        // Always run Fl_Input's drawing first: besides painting, it keeps
        // the horizontal scroll offset (xscroll()) up to date.
        Fl_Input::draw();
        const int start = std::min(insert_position(), mark());
        const int end = std::max(insert_position(), mark());
        // Fl_Input shows the selection only while focused or clicked on,
        // and the repaint below covers plain single-line text only: no
        // input method composition, no control characters (which
        // Fl_Input shows as "^X" sequences).
        if (start == end || (Fl::focus() != this && Fl::pushed() != this) ||
            Fl::compose_state || input_type() != FL_NORMAL_INPUT) {
            return;
        }
        const char* text = value();
        for (int i = 0; i < size(); i++) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < ' ' || c == 127) return;
        }

        // The text area and line placement exactly as Fl_Input_::drawtext
        // computes them for a single line.
        const Fl_Boxtype b = box();
        const int X = x() + Fl::box_dx(b);
        const int Y = y() + Fl::box_dy(b);
        const int W = w() - Fl::box_dw(b);
        const int H = h() - Fl::box_dh(b);
        fl_font(textfont(), textsize());
        const int height = fl_height();
        const int top = Y - yscroll();
        const float baseline = static_cast<float>(top + height - fl_descent());
        const float xpos = static_cast<float>(X - xscroll() + 1);
        const int sel_x1 = static_cast<int>(xpos + fl_width(text, start) + 0.5);
        const int sel_x2 = static_cast<int>(xpos + fl_width(text, end) + 0.5);

        fl_push_clip(X, Y, W, H);
        // Wipe what Fl_Input drew and draw the whole line in one call, so
        // every character sits where it does without a selection. The
        // box is redrawn rather than the area filled: at fractional scales
        // the area's edges round onto the box's shading.
        draw_box(b, color());
        fl_color(active_r() ? textcolor() : fl_inactive(textcolor()));
        fl_draw(text, size(), xpos, baseline);
        // Then the selection: its background, and the same line drawn
        // again at the same position in the contrasting colour, clipped
        // to the selection.
        fl_color(selection_color());
        fl_rectf(sel_x1, top, sel_x2 - sel_x1, height);
        fl_push_clip(sel_x1, top, sel_x2 - sel_x1, height);
        fl_color(fl_contrast(textcolor(), selection_color()));
        fl_draw(text, size(), xpos, baseline);
        fl_pop_clip();
        fl_pop_clip();
    }

    int handle(int event) override {
        // Fl::event_clicks() is 1 for the second click of a double-click
        // and stays so while the button is held for dragging.
        if ((event == FL_PUSH || event == FL_DRAG) && Fl::event_button() == FL_LEFT_MOUSE &&
            Fl::event_clicks() == 1 && input_type() != FL_SECRET_INPUT) {
            // Let Fl_Input handle it as a plain click or drag, which puts
            // the cursor under the mouse (and takes the focus), then widen
            // the selection to whole words.
            Fl::event_clicks(0);
            const int result = Fl_Input::handle(event);
            // Restored so the release still copies the selection to the
            // primary selection and a third click counts as a triple-click.
            Fl::event_clicks(1);
            const int pos = insert_position();
            if (event == FL_PUSH) {
                anchor_start_ = log_word_start(pos);
                anchor_end_ = log_word_end(pos);
                if (anchor_start_ == anchor_end_ && pos < size()) {
                    // Between two separators: select the one to the right.
                    anchor_end_ = pos + 1;
                }
                insert_position(anchor_end_, anchor_start_);
            } else if (pos >= anchor_end_) {
                insert_position(log_word_end(pos), anchor_start_);
            } else if (pos < anchor_start_) {
                insert_position(log_word_start(pos), anchor_end_);
            } else {
                insert_position(anchor_end_, anchor_start_);
            }
            return result;
        }
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE &&
            insert_position() != mark()) {
            focus_on_click(this);
            switch (selection_menu(!readonly())) {
            case kCut:
                // Like Ctrl+X: copy, then delete the selection.
                copy(1);
                cut();
                break;
            case kCopy:
                copy(1);
                break;
            case kPaste:
                // Replaces the selection once the text arrives (FL_PASTE).
                Fl::paste(*this, 1);
                break;
            case kNone:
                break;
            }
            return 1;
        }
        if (event == FL_UNFOCUS && focus_moved_within_window(this)) {
            // Collapse the selection onto the cursor.
            insert_position(insert_position());
        }
        return Fl_Input::handle(event);
    }

private:
    // Word characters as in the log (Fl_Text_Buffer::is_word_separator):
    // ASCII letters, digits and '_'. Bytes of multi-byte UTF-8 characters
    // count as word characters, so a word never ends inside one.
    bool is_word_char(int i) const {
        const unsigned char c = static_cast<unsigned char>(index(i));
        return c >= 128 || std::isalnum(c) || c == '_';
    }

    // Start and end of the word touching cursor position pos, or pos itself
    // if there is no word character on that side.
    int log_word_start(int pos) const {
        while (pos > 0 && is_word_char(pos - 1)) pos--;
        return pos;
    }
    int log_word_end(int pos) const {
        while (pos < size() && is_word_char(pos)) pos++;
        return pos;
    }

    // The word picked by the double-click, kept while dragging on from it.
    int anchor_start_;
    int anchor_end_;
};

// FLTK is not thread-safe. The sync thread only ever touches widgets while
// holding the global FLTK lock (Fl::lock), which the main thread releases
// whenever it waits for events, and then wakes the main thread with
// Fl::awake() so the change gets drawn. Everything else (button callbacks,
// starting and joining the thread) happens on the main thread.
class LauncherWindow : public SyncObserver {
public:
    LauncherWindow(const SyncOptions& options, const GuiContext& context, bool launch)
        : options_(options),
          context_(context),
          launch_(launch),
          cancel_(false),
          running_(false),
          close_requested_(false),
          exit_code_(0) {
        window_ = new Fl_Double_Window(600, 415, "IGW Legacy Launcher " IGWL_VERSION);
        window_->callback(on_close, this);

        status_ = new Fl_Box(15, 10, 570, 30, "Press Start to update and launch the game.");
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        status_->labelfont(FL_HELVETICA_BOLD);

        // Where to sync from. Saved next to the launcher when Start is
        // pressed with a changed value; clearing it restores the default.
        url_input_ = new UrlInput(95, 45, 490, 25, "Server URL:");
        url_input_->value(options_.base_url.c_str());
        url_input_->tooltip("The address the game files are downloaded from. "
                            "Leave empty for the default server.");

        // A text display rather than a browser so the log can be selected
        // and copied (mouse, Ctrl+C, right-click menu); unlike
        // Fl_Text_Editor it offers no way to change the text.
        log_buffer_ = new Fl_Text_Buffer();
        log_ = new LogDisplay(15, 80, 570, 200);
        log_->buffer(log_buffer_);
        log_->textsize(12);

        current_ = new Fl_Progress(15, 300, 570, 20, "Current file");
        current_->align(FL_ALIGN_TOP_LEFT);
        current_->minimum(0);
        current_->maximum(1);
        current_->selection_color(FL_BLUE);
        current_->labelsize(12);

        overall_ = new Fl_Progress(15, 340, 570, 20, "Overall");
        overall_->align(FL_ALIGN_TOP_LEFT);
        overall_->minimum(0);
        overall_->maximum(1);
        overall_->selection_color(FL_DARK_GREEN);
        overall_->labelsize(12);

        cancel_button_ = new Fl_Button(385, 375, 95, 28, "Quit");
        cancel_button_->callback(on_cancel, this);

        start_button_ = new Fl_Return_Button(490, 375, 95, 28, "Start");
        start_button_->callback(on_start, this);

        window_->resizable(log_);
        window_->end();

        log("Game directory: " + options_.root.u8string());
        if (!context_.settings_error.empty()) {
            log("Ignoring the saved settings: " + context_.settings_error);
        }
    }

    ~LauncherWindow() {
        delete window_;
        // The display does not own its buffer; free it once the display,
        // which still refers to it, is gone.
        delete log_buffer_;
    }

    int run() {
        window_->show();
        Fl::run();
        return exit_code_;
    }

    // --- SyncObserver, called on the sync thread --------------------------

    void log(const std::string& line) override {
        Fl::lock();
        // Separate lines with newlines instead of ending each one with it,
        // so there is no empty line below the last entry.
        if (log_buffer_->length() > 0) {
            log_buffer_->append("\n");
        }
        log_buffer_->append(line.c_str());
        // Keep the newest line in view. The insert position is invisible
        // (no cursor is shown) and independent of the selection.
        log_->insert_position(log_buffer_->length());
        log_->show_insert_position();
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

    // Takes over the server URL from the input field, saving it if it was
    // changed. Returns false (after telling the user) if it is not usable.
    bool apply_server_url() {
        std::string url = url_input_->value();
        // Empty means the default; spaces only count as empty too.
        const bool use_default = url.find_first_not_of(" \t\r\n") == std::string::npos;
        if (use_default) {
            url = IGWL_BASE_URL;
        }
        std::string error;
        if (!normalize_base_url(url, error)) {
            fl_alert("Invalid server URL:\n\n%s", error.c_str());
            url_input_->take_focus();
            return false;
        }
        url_input_->value(url.c_str());
        if (url == options_.base_url) {
            return true;
        }

        options_.base_url = url;
        Settings settings;
        // The default is stored as "no setting", so a launcher update that
        // changes the default is followed.
        std::string default_url = IGWL_BASE_URL;
        if (!normalize_base_url(default_url, error) || url != default_url) {
            settings.base_url = url;
        }
        if (save_settings(context_.settings_file, settings, error)) {
            log("Saved server URL " + url + " to " + context_.settings_file.u8string());
        } else {
            // Not fatal: the URL is still used for this session.
            log("Could not save the server URL: " + error);
        }
        return true;
    }

    void start() {
        if (!apply_server_url()) {
            return;
        }
        running_ = true;
        cancel_ = false;
        url_input_->deactivate();
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

        url_input_->activate();
        start_button_->label("Start");
        start_button_->activate();
        cancel_button_->label("Quit");
        cancel_button_->activate();
    }

    // A copy: the server URL changes when the user edits it. The worker
    // thread only reads it while running, when the field is locked.
    SyncOptions options_;
    const GuiContext context_;
    const bool launch_;

    Fl_Double_Window* window_;
    Fl_Box* status_;
    Fl_Input* url_input_;
    Fl_Text_Buffer* log_buffer_;
    Fl_Text_Display* log_;
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

int run_gui(const SyncOptions& options, const GuiContext& context, bool launch) {
    // Enables FLTK's locking support; the main thread holds the lock from
    // here on, except while Fl::run() waits for events.
    Fl::lock();
#if defined(__linux__)
    // FLTK's Xft text drawing uses fontconfig without initialising it, which
    // recent fontconfig releases complain about on stderr at startup.
    FcInit();
#endif
    Fl::scheme("gtk+");

    LauncherWindow window(options, context, launch);
    return window.run();
}

}  // namespace igwl
