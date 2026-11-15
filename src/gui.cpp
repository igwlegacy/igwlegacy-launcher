#include "gui.h"

#include "assets.h"
#include "platform.h"
#include "settings.h"

#include "igwl_config.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>

#if defined(__linux__)
#include <fontconfig/fontconfig.h>
#elif defined(_WIN32)
// NOMINMAX keeps the min/max macros from breaking std::min/std::max.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "resource.h"
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


// The launcher's colours, taken from the logo: dark iron, rust, and the
// red of its accents.
struct Theme {
    Fl_Color background;   // windows, dialogs, plain buttons, menus
    Fl_Color field;        // input fields, the log, empty progress bars
    Fl_Color text;         // labels and text on the colours above
    Fl_Color accent;       // the Start buttons, selected text
    Fl_Color accent_text;  // labels on the accent colour
    Fl_Color progress;     // the filled part of the overall progress bars
};

// fl_rgb_color() is not constexpr in C++11, hence a function instead of a
// constant.
Theme launcher_theme() {
    Theme theme;
    theme.background = fl_rgb_color(30, 28, 27);
    theme.field = fl_rgb_color(48, 45, 43);
    theme.text = fl_rgb_color(232, 226, 216);
    theme.accent = fl_rgb_color(168, 72, 32);
    theme.accent_text = FL_WHITE;
    theme.progress = fl_rgb_color(178, 34, 30);
    return theme;
}

// Makes the theme FLTK's default colours, so that every widget, including
// the ones FLTK creates itself (fl_alert() dialogs, right-click menus,
// tooltips), uses it unless it sets a colour of its own. Has to run before
// any window is created; the accents are applied per widget.
void apply_theme(const Theme& theme) {
    uchar r, g, b;
    Fl::get_color(theme.background, r, g, b);
    Fl::background(r, g, b);
    Fl::get_color(theme.field, r, g, b);
    Fl::background2(r, g, b);
    Fl::get_color(theme.text, r, g, b);
    Fl::foreground(r, g, b);
    Fl::set_color(FL_SELECTION_COLOR, theme.accent);

    Fl_Tooltip::color(theme.field);
    Fl_Tooltip::textcolor(theme.text);

    // The icon of fl_alert() and friends is copied from this template;
    // FLTK's default is a blue sign on white.
    Fl_Widget* icon = fl_message_icon();
    icon->color(theme.field);
    icon->labelcolor(theme.progress);
}

#if defined(_WIN32)
// Makes the executable's own icon (src/icon.rc) the one of every window
// FLTK creates from now on, the dialogs included; without it they show
// Windows' generic application icon in the title bar and the taskbar. Has
// to run before any window is created.
void set_window_icons() {
    const HINSTANCE instance = GetModuleHandleW(NULL);
    // Loaded at exactly the sizes Windows draws them at, so it picks the
    // closest image of the .ico instead of scaling the default 32x32 one.
    const auto load = [instance](int size_metric) {
        const int size = GetSystemMetrics(size_metric);
        return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IGWL_ICON_ID),
                                             IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    HICON big_icon = load(SM_CXICON);
    HICON small_icon = load(SM_CXSMICON);
    // FLTK copies the icons. A missing one (NULL) leaves Windows' default in
    // its place, which is not worth failing the launcher over.
    Fl_Window::default_icons(big_icon, small_icon);
    if (big_icon != NULL) {
        DestroyIcon(big_icon);
    }
    if (small_icon != NULL) {
        DestroyIcon(small_icon);
    }
}
#endif

// Gives a Start button the accent colour.
void style_start_button(Fl_Button* button, const Theme& theme) {
    button->color(theme.accent);
    button->labelcolor(theme.accent_text);
    button->labelfont(FL_HELVETICA_BOLD);
}

// Clicks on the logo, each within the gap (seconds) of the previous one,
// that switch from the minimal to the full view.
const int kSwitchClicks = 4;
const double kSwitchClickGap = 0.8;

// The logo of the minimal view. Clicking it kSwitchClicks times in a row,
// each click within kSwitchClickGap seconds of the previous one, runs the
// widget's callback, which switches to the full view.
class LogoBox : public Fl_Box {
public:
    LogoBox(int x, int y, int w, int h) : Fl_Box(x, y, w, h), clicks_(0) {}

    ~LogoBox() {
        Fl::remove_timeout(reset_clicks, this);
    }

    int handle(int event) override {
        if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
            // Every click restarts the gap; when it runs out, counting
            // starts over.
            Fl::remove_timeout(reset_clicks, this);
            if (++clicks_ >= kSwitchClicks) {
                clicks_ = 0;
                do_callback();
            } else {
                Fl::add_timeout(kSwitchClickGap, reset_clicks, this);
            }
            return 1;
        }
        return Fl_Box::handle(event);
    }

private:
    static void reset_clicks(void* data) {
        static_cast<LogoBox*>(data)->clicks_ = 0;
    }

    int clicks_;
};

// The launcher has two views of the same state, each in its own window:
//
// * the minimal view, shown at startup: the logo on the left and to its
//   right the server URL, Start (Cancel while syncing), one progress bar
//   for the overall progress and the status text;
// * the full view, the fallback reached by clicking the logo four times
//   in a row: additionally the log, a progress bar for the current file
//   and a separate Quit/Cancel button.
//
// Only one of the windows is shown at a time, but both are kept up to date
// so that switching mid-sync loses nothing.
//
// FLTK is not thread-safe. The sync thread only ever touches widgets while
// holding the global FLTK lock (Fl::lock), which the main thread releases
// whenever it waits for events, and then wakes the main thread with
// Fl::awake() so the change gets drawn. Everything else (button callbacks,
// starting and joining the thread) happens on the main thread.
class LauncherWindow : public SyncObserver {
public:
    // The theme has to be applied (apply_theme()) beforehand.
    LauncherWindow(const SyncOptions& options, const GuiContext& context, bool launch,
                   const Theme& theme)
        : options_(options),
          context_(context),
          launch_(launch),
          logo_image_(nullptr),
          shown_(nullptr),
          cancel_(false),
          running_(false),
          close_requested_(false),
          exit_code_(0) {
        build_minimal_view(theme);
        build_full_view(theme);

        log("Game directory: " + options_.root.u8string());
        if (!context_.settings_error.empty()) {
            log("Ignoring the saved settings: " + context_.settings_error);
        }
    }

    ~LauncherWindow() {
        delete mini_window_;
        delete full_window_;
        // Neither the display nor the box own what they show; free it once
        // the widgets, which still refer to it, are gone.
        delete log_buffer_;
        delete logo_image_;
    }

    int run() {
        shown_ = mini_window_;
        mini_window_->show();
        Fl::run();
        return exit_code_;
    }

    // --- SyncObserver, called on the sync thread --------------------------

    // Only the full view has a log; the minimal one shows the status alone.
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
        set_status(text);
        Fl::unlock();
        Fl::awake();
    }

    void progress(double current, double overall) override {
        Fl::lock();
        set_progress(current_, current);
        set_progress(overall_, overall);
        set_progress(mini_progress_, overall);
        Fl::unlock();
        Fl::awake();
    }

private:
    // --- Building the views -----------------------------------------------

    // 1024x319: the logo (512x319) fills the left half, the controls are
    // stacked, vertically centred, in the right one.
    void build_minimal_view(const Theme& theme) {
        mini_window_ = new Fl_Double_Window(1024, 319, "IGW Legacy Launcher " IGWL_VERSION);
        mini_window_->callback(on_close, this);

        logo_ = new LogoBox(0, 0, 512, 319);
        logo_->box(FL_NO_BOX);
        logo_image_ = new Fl_PNG_Image(nullptr, assets::logo_flat_png,
                                       static_cast<int>(assets::logo_flat_png_size));
        // A logo that fails to decode costs the picture, not the launcher:
        // the box stays empty and clicking it still works.
        if (logo_image_->fail() == 0) {
            logo_->image(logo_image_);
        }
        logo_->callback(on_logo_clicks, this);

        // Right column: 452 pixels wide, 28 from the logo and 32 from the
        // window's right edge.
        const int x = 540;
        const int w = 452;

        mini_url_ = new UrlInput(x, 67, w, 30, "Server URL");
        mini_url_->align(FL_ALIGN_TOP_LEFT);
        mini_url_->labelsize(12);
        mini_url_->value(options_.base_url.c_str());
        mini_url_->tooltip(kUrlTooltip);

        // A plain button with Enter as its shortcut rather than an
        // Fl_Return_Button, which would draw its return-key glyph on it.
        mini_start_ = new Fl_Button(x, 113, w, 52, "Start");
        mini_start_->shortcut(FL_Enter);
        style_start_button(mini_start_, theme);
        mini_start_->labelsize(20);
        mini_start_->callback(on_mini_start, this);

        mini_progress_ = new Fl_Progress(x, 193, w, 24);
        mini_progress_->minimum(0);
        mini_progress_->maximum(1);
        mini_progress_->selection_color(theme.progress);
        mini_progress_->labelsize(12);

        // Two lines, so the longer messages wrap instead of being cut off.
        mini_status_ = new Fl_Box(x, 227, w, 44);
        mini_status_->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CLIP);
        mini_status_->labelsize(14);

        // No resizable widget: the window keeps its size.
        mini_window_->end();
    }

    // The original window, kept as the fallback.
    void build_full_view(const Theme& theme) {
        full_window_ = new Fl_Double_Window(600, 415, "IGW Legacy Launcher " IGWL_VERSION);
        full_window_->callback(on_close, this);

        status_ = new Fl_Box(15, 10, 570, 30);
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        status_->labelfont(FL_HELVETICA_BOLD);

        // Where to sync from. Saved next to the launcher when Start is
        // pressed with a changed value; clearing it restores the default.
        url_input_ = new UrlInput(95, 45, 490, 25, "Server URL:");
        url_input_->value(options_.base_url.c_str());
        url_input_->tooltip(kUrlTooltip);

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
        // The accent, to tell it apart from the overall progress below.
        current_->selection_color(theme.accent);
        current_->labelsize(12);

        overall_ = new Fl_Progress(15, 340, 570, 20, "Overall");
        overall_->align(FL_ALIGN_TOP_LEFT);
        overall_->minimum(0);
        overall_->maximum(1);
        overall_->selection_color(theme.progress);
        overall_->labelsize(12);

        cancel_button_ = new Fl_Button(385, 375, 95, 28, "Quit");
        cancel_button_->callback(on_cancel, this);

        start_button_ = new Fl_Return_Button(490, 375, 95, 28, "Start");
        style_start_button(start_button_, theme);
        start_button_->callback(on_start, this);

        full_window_->resizable(log_);
        full_window_->end();

        set_status("Press Start to update and launch the game.");
    }

    // --- Shared by both views ---------------------------------------------

    static const char kUrlTooltip[];

    void set_status(const std::string& text) {
        status_->copy_label(text.c_str());
        mini_status_->copy_label(text.c_str());
    }

    static void set_progress(Fl_Progress* bar, double value) {
        bar->value(static_cast<float>(value));
        char text[16];
        std::snprintf(text, sizeof text, "%.0f%%", value * 100.0);
        bar->copy_label(text);
        // The percentage replaces the caption while a sync runs; keep it
        // readable inside the bar instead of above it.
        bar->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    // The server URL field of the view in use.
    Fl_Input* active_url_input() const {
        return shown_ == mini_window_ ? mini_url_ : url_input_;
    }

    // Closes whichever window is shown, which ends Fl::run().
    void close() {
        mini_window_->hide();
        full_window_->hide();
    }

    void request_cancel() {
        cancel_ = true;
        cancel_button_->deactivate();
        mini_start_->deactivate();
        set_status("Cancelling...");
    }

    // --- Main thread ------------------------------------------------------

    // Four clicks on the logo: replace the minimal view by the full one at
    // the same place. A sync in progress carries on and keeps reporting to
    // the full view.
    static void on_logo_clicks(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        if (self->shown_ != self->mini_window_) {
            return;
        }
        // Take over what was typed but not yet applied by Start.
        self->url_input_->value(self->mini_url_->value());
        self->full_window_->position(self->mini_window_->x(), self->mini_window_->y());
        // Show the new window before hiding the old one: with no window
        // shown, Fl::run() would return.
        self->full_window_->show();
        self->mini_window_->hide();
        self->shown_ = self->full_window_;
    }

    static void on_start(Fl_Widget*, void* data) {
        static_cast<LauncherWindow*>(data)->start();
    }

    // The minimal view's only button: Start, or Cancel while syncing.
    static void on_mini_start(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        if (self->running_) {
            self->request_cancel();
        } else {
            self->start();
        }
    }

    static void on_cancel(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        if (self->running_) {
            self->request_cancel();
        } else {
            self->close();
        }
    }

    // Closing a window while a sync runs only requests cancellation; the
    // window goes away once the thread has finished (see finished()).
    // Hiding it right away would end Fl::run() with the thread still alive.
    static void on_close(Fl_Widget*, void* data) {
        LauncherWindow* self = static_cast<LauncherWindow*>(data);
        // Escape also triggers the window callback; treat it like Quit.
        if (self->running_) {
            self->close_requested_ = true;
            self->cancel_ = true;
            self->set_status("Cancelling...");
        } else {
            self->close();
        }
    }

    // Takes over the server URL from the input field of the view in use,
    // saving it if it was changed. Returns false (after telling the user)
    // if it is not usable.
    bool apply_server_url() {
        Fl_Input* input = active_url_input();
        std::string url = input->value();
        // Empty means the default; spaces only count as empty too.
        const bool use_default = url.find_first_not_of(" \t\r\n") == std::string::npos;
        if (use_default) {
            url = IGWL_BASE_URL;
        }
        std::string error;
        if (!normalize_base_url(url, error)) {
            fl_alert("Invalid server URL:\n\n%s", error.c_str());
            input->take_focus();
            return false;
        }
        // Both views show the URL in use.
        url_input_->value(url.c_str());
        mini_url_->value(url.c_str());
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
        mini_url_->deactivate();
        start_button_->deactivate();
        cancel_button_->label("Cancel");
        cancel_button_->activate();
        // The minimal view has no separate cancel button.
        mini_start_->label("Cancel");
        mini_start_->activate();
        set_progress(current_, 0.0);
        set_progress(overall_, 0.0);
        set_progress(mini_progress_, 0.0);

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
            close();
            return;
        }

        if (result_.ok) {
            if (!launch_ || result_.launch.empty()) {
                set_status(result_.launch.empty()
                               ? "Up to date. The manifest names no program to start here."
                               : "Up to date.");
            } else {
                std::string error;
                if (platform::launch(result_.launch, options_.root, error)) {
                    close();
                    return;
                }
                log(error);
                set_status("Up to date, but the game could not be started.");
                exit_code_ = 1;
            }
        } else if (result_.cancelled) {
            log("Cancelled.");
            set_status("Cancelled. Press Start to try again.");
        } else {
            log("Error: " + result_.error);
            set_status("Update failed. Press Start to try again.");
            exit_code_ = 1;
            fl_alert("Update failed:\n\n%s", result_.error.c_str());
        }

        url_input_->activate();
        mini_url_->activate();
        start_button_->label("Start");
        start_button_->activate();
        mini_start_->label("Start");
        mini_start_->activate();
        cancel_button_->label("Quit");
        cancel_button_->activate();
    }

    // A copy: the server URL changes when the user edits it. The worker
    // thread only reads it while running, when the fields are locked.
    SyncOptions options_;
    const GuiContext context_;
    const bool launch_;

    // Minimal view.
    Fl_Double_Window* mini_window_;
    LogoBox* logo_;
    Fl_PNG_Image* logo_image_;
    Fl_Input* mini_url_;
    Fl_Button* mini_start_;
    Fl_Progress* mini_progress_;
    Fl_Box* mini_status_;

    // Full view.
    Fl_Double_Window* full_window_;
    Fl_Box* status_;
    Fl_Input* url_input_;
    Fl_Text_Buffer* log_buffer_;
    Fl_Text_Display* log_;
    Fl_Progress* current_;
    Fl_Progress* overall_;
    Fl_Button* cancel_button_;
    Fl_Return_Button* start_button_;

    // The window of the view in use.
    Fl_Double_Window* shown_;

    std::thread worker_;
    std::atomic<bool> cancel_;
    SyncResult result_;
    bool running_;
    bool close_requested_;
    int exit_code_;
};

const char LauncherWindow::kUrlTooltip[] =
    "The address the game files are downloaded from. Leave empty for the default server.";

}  // namespace

int run_gui(const SyncOptions& options, const GuiContext& context, bool launch) {
    // Enables FLTK's locking support; the main thread holds the lock from
    // here on, except while Fl::run() waits for events.
    Fl::lock();
#if defined(__linux__)
    // FLTK's Xft text drawing uses fontconfig without initialising it, which
    // recent fontconfig releases complain about on stderr at startup.
    FcInit();
#elif defined(_WIN32)
    set_window_icons();
#endif
    Fl::scheme("gtk+");
    const Theme theme = launcher_theme();
    apply_theme(theme);

    LauncherWindow window(options, context, launch, theme);
    return window.run();
}

}  // namespace igwl
