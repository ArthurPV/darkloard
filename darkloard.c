/* Copyright (C) 2026 ArthurPV
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <time.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#elif defined(__NetBSD__)
#include <util.h>
#else
#error "This OS is not supported"
#endif /* defined(__linux__) */

#if defined(__linux__)
#include <linux/limits.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/syslimits.h>
#else
#error "This OS is not supported"
#endif /* defined(__linux__) */

#if defined(__FreeBSD__) || defined(__NetBSD__)
#include <termios.h>
#endif /* defined(__FreeBSD__) || defined(__NetBSD__) */

#if defined(__FreeBSD__) || defined(__NetBSD__)
#include <signal.h>
#endif /* defined(__FreeBSD__) || defined(__NetBSD__) */

#if defined(__NetBSD__)
#include <sys/ioctl.h>
#endif /* defined(__NetBSD__) */

#include "config.h"

// Defaults for options added after config.h was created (config.h is only
// copied from config.h.def when missing).
#ifndef DARKLOARD_CURSOR_BLINK
// Whether the default cursor (shape 0, also after a reset) blinks.
#define DARKLOARD_CURSOR_BLINK false
#endif

#ifndef DARKLOARD_CURSOR_BLINK_MS
#define DARKLOARD_CURSOR_BLINK_MS 500
#endif

#ifndef DARKLOARD_DOUBLE_CLICK_MS
#define DARKLOARD_DOUBLE_CLICK_MS 400
#endif

// Characters (besides whitespace) that end a word for double-click
// selection. Paths, URLs and user@host stay one word.
#ifndef DARKLOARD_WORD_DELIMITERS
#define DARKLOARD_WORD_DELIMITERS "'\"()[]{}<>,;|`"
#endif

#define LOG_ERROR(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__);
#define XMALLOC(size) xmalloc__Darkloard(size)
#define XREALLOC(ptr, size) xrealloc__Darkloard(ptr, size)
#define XASPRINTF(buffer, fmt, ...) \
    xasprintf__Darkloard(buffer, fmt, ##__VA_ARGS__);

// See:
// - Linux: man console_codes
#define DARKLOARD_BEL 0x07
#define DARKLOARD_BS 0x08
#define DARKLOARD_HT 0x09
#define DARKLOARD_LF 0x0A
#define DARKLOARD_VT 0x0B
#define DARKLOARD_FF 0x0C
#define DARKLOARD_CR 0x0D
#define DARKLOARD_SO 0x0E
#define DARKLOARD_SI 0x0F
#define DARKLOARD_CAN 0x18
#define DARKLOARD_SUB 0x1A
#define DARKLOARD_ESC 0x1B
#define DARKLOARD_DEL 0x7F
#define DARKLOARD_CSI 0x9B

#define DARKLOARD_NUL 0x00
#define DARKLOARD_ENQ 0x05
#define DARKLOARD_DC1 0x11
#define DARKLOARD_DC3 0x13

#define DARKLOARD_ATTR_BOLD (1 << 0)
#define DARKLOARD_ATTR_DIM (1 << 1)
#define DARKLOARD_ATTR_ITALIC (1 << 2)
#define DARKLOARD_ATTR_UNDERLINE (1 << 3)
#define DARKLOARD_ATTR_BLINK (1 << 4)
#define DARKLOARD_ATTR_REVERSE (1 << 5)
#define DARKLOARD_ATTR_INVISIBLE (1 << 6)
#define DARKLOARD_ATTR_STRIKE (1 << 7)

#define DARKLOARD_CSI_MAX_PARAMS 16
#define DARKLOARD_TAB_WIDTH 8
#define DARKLOARD_OSC_MAX_SIZE (8 * 1024 * 1024)
#define DARKLOARD_FONT_CACHE_SIZE 64
#define DARKLOARD_MAX_READ_BYTES (64 * 1024)

struct DarkloardTerminal
{
    uint32_t row;
    uint32_t col;
};

struct DarkloardTerminalProcess
{
    char *shell;
    pid_t shell_pid;
};

struct DarkloardMessage
{
    char *buffer;
    size_t buffer_len;
    struct DarkloardMessage *next;
};

struct DarkloardMessageList
{
    struct DarkloardMessage *first;
    struct DarkloardMessage *last;
};

struct DarkloardParseIterator
{
    char *buffer;
    size_t buffer_len;
    char *current;
};

struct DarkloardCell
{
    uint32_t codepoint;
    uint32_t fg;
    uint32_t bg;
    uint8_t attrs;
};

struct DarkloardCursor
{
    uint32_t row;
    uint32_t col;
    bool visible;
};

struct DarkloardScrollRegion
{
    uint32_t top;
    uint32_t bottom;
};

struct DarkloardScreen
{
    struct DarkloardCell **cells;
    struct DarkloardCursor cursor;
    struct DarkloardScrollRegion scroll_region;
    uint32_t rows;
    uint32_t cols;
    uint32_t current_fg;
    uint32_t current_bg;
    uint8_t current_attrs;
    struct DarkloardCursor saved_cursor;
    bool saved_cursor_valid;
    bool is_dirty;
    bool pending_wrap;
    bool auto_wrap;
    bool insert_mode; // IRM (CSI 4 h)
    // Character sets G0/G1 ('B' = ASCII, '0' = DEC special graphics) and
    // which one is active (SO selects G1, SI selects G0).
    char charsets[2];
    int active_charset;
    // Last printed character, for REP (CSI b).
    uint32_t last_char;
};

enum DarkloardFontVariant
{
    FONT_REGULAR = 0,
    FONT_BOLD = 1,
    FONT_ITALIC = 2,
    FONT_BOLD_ITALIC = 3,
    FONT_COUNT = 4,
};

enum DarkloardParseState
{
    DARKLOARD_PARSE_STATE_NORMAL = 0,
    DARKLOARD_PARSE_STATE_ESC,
    DARKLOARD_PARSE_STATE_CSI,
    DARKLOARD_PARSE_STATE_OSC,
    DARKLOARD_PARSE_STATE_CHARSET,
    DARKLOARD_PARSE_STATE_STR_SKIP,
};

struct DarkloardCsiParams
{
    int params[DARKLOARD_CSI_MAX_PARAMS];
    // is_sub[i]: params[i] was separated from the previous one by ':'
    // (sub-parameter, e.g. "4:3" or "38:2::r:g:b").
    bool is_sub[DARKLOARD_CSI_MAX_PARAMS];
    int params_len;
    bool private_mode;
    bool secondary;
    // '<' or '=' prefix (e.g. kitty keyboard protocol): not supported, the
    // whole sequence is ignored.
    bool other_private;
    char intermediate;
    char final_byte;
};

struct DarkloardParser
{
    enum DarkloardParseState state;
    struct DarkloardCsiParams csi;
    // Growable (OSC 52 carries whole clipboard contents), up to
    // DARKLOARD_OSC_MAX_SIZE; longer sequences are dropped (osc_overflow).
    char *osc_buf;
    int osc_len;
    int osc_cap;
    bool osc_overflow;
    bool osc_esc_pending;
    bool str_esc_pending;
    uint32_t utf8_codepoint;
    int utf8_remaining;
    // Which G set "ESC ( x" / "ESC ) x" designates.
    int charset_target;
    // Whether the current OSC was terminated by BEL (else ST): replies use
    // the same terminator.
    bool osc_bel;
};

struct DarkloardBuffer
{
    char *data;
    size_t len;
    size_t cap;
};

// Selection rows are absolute line numbers (see view_row_to_line__Darkloard)
// so a selection stays attached to its text while scrolling through history.
struct DarkloardSelection
{
    bool active;
    bool selecting;
    int64_t start_row;
    uint32_t start_col;
    int64_t end_row;
    uint32_t end_col;
    char *text;
    size_t text_len;
};

static Display *display = NULL;
static int display_fd = -1;
static unsigned int window_width = 0;
static unsigned int window_height = 0;
static Window window = 0;
static GC window_gc = { 0 };
static int screen_num = 0;
static Colormap colormap = { 0 };
static Visual *visual = NULL;
static int pty_master_fd = -1;
static struct DarkloardTerminal terminal = { 0 };
static struct DarkloardTerminalProcess terminal_process = { 0 };
static struct DarkloardMessageList message_list = { 0 };
static XftFont *fonts[FONT_COUNT] = { 0 };
static XftFont *font_cache[DARKLOARD_FONT_CACHE_SIZE] = { 0 };
static int font_cache_len = 0;
static unsigned int current_font_size = DARKLOARD_FONT_SIZE;
static Pixmap back_buffer = 0;
static unsigned int back_buffer_width = 0;
static unsigned int back_buffer_height = 0;
static struct DarkloardCell **inactive_cells = NULL;
static struct DarkloardCursor main_cursor_saved = { 0 };
static bool in_alt_screen = false;
static struct DarkloardCell **history_lines = NULL;
static uint32_t *history_line_cols = NULL;
static int history_head = 0;
static int history_count = 0;
// Number of lines that fell off the top of the full history buffer. Used to
// give every line a stable absolute number.
static int64_t history_dropped = 0;
static int scroll_offset = 0;
static struct DarkloardScreen screen = { 0 };
static struct DarkloardParser parser = { 0 };
static struct DarkloardSelection selection = { 0 };
// Bytes waiting to be written to the PTY (the master fd is non-blocking, so a
// large paste can't be written in a single call).
static struct DarkloardBuffer pty_out = { 0 };
// Paste being received from another X client (possibly via INCR transfer).
static struct DarkloardBuffer paste_in = { 0 };
static bool paste_incr = false;
// Text owned as CLIPBOARD (Ctrl+Shift+C). Kept separate from the PRIMARY
// selection so a new mouse selection or losing one selection doesn't affect
// the other.
static char *clipboard_text = NULL;
static size_t clipboard_text_len = 0;
static bool running = true;
static bool app_cursor_keys = false;
static bool bracketed_paste = false;
static bool focus_events = false;

// DECSCUSR (CSI Ps SP q). Blinking variants are drawn steady.
enum DarkloardCursorShape
{
    DARKLOARD_CURSOR_BLOCK = 0,
    DARKLOARD_CURSOR_UNDERLINE,
    DARKLOARD_CURSOR_BAR,
};

static enum DarkloardCursorShape cursor_shape = DARKLOARD_CURSOR_BLOCK;
// Blinking (DECSCUSR odd shapes, or mode ?12). cursor_blink_on is the
// current phase; blinking pauses while the window is unfocused.
static bool cursor_blink = DARKLOARD_CURSOR_BLINK;
static bool cursor_blink_on = true;
static int64_t cursor_blink_last_ms = 0;
static bool window_focused = true;

// Pointer position while dragging a selection, for auto-scrolling when it
// is above / below the text area.
static int drag_x = 0;
static int drag_y = 0;
static int64_t autoscroll_last_ms = 0;

// Last left click, to detect double-clicks.
static Time last_click_time = 0;
static int64_t last_click_line = -1;
static uint32_t last_click_col = 0;
#define DARKLOARD_AUTOSCROLL_MS 50

// Mouse reporting (xterm protocol), set by "CSI ? <mode> h/l".
enum DarkloardMouseMode
{
    DARKLOARD_MOUSE_NONE = 0,
    DARKLOARD_MOUSE_X10 = 9,       // presses only
    DARKLOARD_MOUSE_NORMAL = 1000, // presses and releases
    DARKLOARD_MOUSE_BUTTON = 1002, // + motion while a button is held
    DARKLOARD_MOUSE_ANY = 1003,    // + all motion
};

static enum DarkloardMouseMode mouse_mode = DARKLOARD_MOUSE_NONE;
static bool mouse_sgr = false;   // 1006: CSI < b ; x ; y M/m
static bool mouse_urxvt = false; // 1015: CSI b ; x ; y M
static int mouse_button_held = -1; // 0-2 while a button is down, else -1
static uint32_t mouse_last_row = UINT32_MAX;
static uint32_t mouse_last_col = UINT32_MAX;

static Atom atom_utf8_string = None;
static Atom atom_clipboard = None;
static Atom atom_targets = None;
static Atom atom_xsel_data = None;
static Atom atom_incr = None;
static Atom atom_wm_delete = None;
static Atom atom_net_wm_name = None;

static inline struct DarkloardMessage
init__DarkloardMessage(char *buffer, size_t buffer_len);

static inline void
deinit__DarkloardMessage(struct DarkloardMessage *self);

static void
deinit__DarkloardMessageList(struct DarkloardMessageList *self);

static void *
xmalloc__Darkloard(size_t size);

static void *
xrealloc__Darkloard(void *ptr, size_t size);

static void
append__DarkloardBuffer(struct DarkloardBuffer *self,
                        const char *data,
                        size_t data_len);

static void
flush_pty__Darkloard(void);

static void
xasprintf__Darkloard(char **buffer, const char *fmt, ...);

static int
configure_terminal__Darkloard(int slave_fd);

static bool
is_terminal_alive__Darkloard(void);

static int
open_terminal__Darkloard(int slave_fd, char *slave_filename);

static int
open_pty__Darkloard(void);

static int
resize_pty__Darkloard(unsigned short row,
                      unsigned short col,
                      unsigned short xpixel,
                      unsigned short ypixel);

static int
read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read);

static int
write_pty__Darkloard(char *buffer, size_t buffer_len);

static void
init__DarkloardScreen(uint32_t rows, uint32_t cols);

static void
free_cells__DarkloardScreen(void);

static void
deinit__DarkloardScreen(void);

static void
resize__DarkloardScreen(uint32_t rows, uint32_t cols);

static void
scroll_up__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n);

static void
scroll_region_up__DarkloardScreen(uint32_t top,
                                  uint32_t bottom,
                                  uint32_t n,
                                  bool to_history);

static void
scroll_down__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n);

static void
put_char__DarkloardScreen(uint32_t codepoint);

static void
erase_display__DarkloardScreen(int mode);

static void
erase_line__DarkloardScreen(int mode);

static int
codepoint_to_utf8__Darkloard(uint32_t cp, char *buf);

static void
handle_normal__DarkloardParser(unsigned char c);

static void
handle_esc__DarkloardParser(unsigned char c);

static void
handle_csi_byte__DarkloardParser(unsigned char c);

static void
handle_csi__DarkloardParser(void);

static void
handle_sgr__DarkloardParser(void);

static void
handle_osc_byte__DarkloardParser(unsigned char c);

static void
handle_osc__DarkloardParser(void);

static void
handle_str_skip__DarkloardParser(unsigned char c);

static uint32_t
xterm256_to_rgb__Darkloard(int idx);

static void
feed__DarkloardParser(unsigned char c);

static void
parse__Darkloard(struct DarkloardMessage *message);

static void
push_history_line__Darkloard(struct DarkloardCell *line, uint32_t cols);

static void
init_history__Darkloard(void);

static void
erase_history__Darkloard(void);

static struct DarkloardCell *
get_history_line__Darkloard(int i);

static void
clear_history__Darkloard(void);

static void
enter_alt_screen__Darkloard(bool save_cursor);

static void
leave_alt_screen__Darkloard(bool restore_cursor);

static XftFont *
get_font_for_codepoint__Darkloard(uint32_t cp, uint8_t attrs);

static void
draw_glyph__Darkloard(XftDraw *draw,
                      uint32_t codepoint,
                      uint32_t color,
                      uint8_t attrs,
                      int cell_w,
                      int x,
                      int y);

static void
draw__Darkloard(void);

static void
handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel);

static void
init_atoms__Darkloard(void);

static void
deinit__DarkloardSelection(void);

static void
pixel_to_cell__Darkloard(int px, int py, uint32_t *row, uint32_t *col);

static int64_t
view_row_to_line__Darkloard(uint32_t row);

static struct DarkloardCell *
get_line_cells__Darkloard(int64_t line, uint32_t *cols);

static void
pixel_to_selection__Darkloard(int px, int py, int64_t *line, uint32_t *col);

static bool
is_selected__DarkloardSelection(int64_t line, uint32_t col);

static void
build_selection_text__DarkloardSelection(void);

static void
request_paste__Darkloard(Atom sel_type);

static void
handle_selection_request__Darkloard(XSelectionRequestEvent *req);

static void
handle_selection_notify__Darkloard(XSelectionEvent *event);

static void
handle_property_notify__Darkloard(XPropertyEvent *event);

static void
send_osc_color__Darkloard(const char *key, uint32_t color);

static void
handle_osc52__Darkloard(const char *args);

static int64_t
now_ms__Darkloard(void);

static void
reset_cursor_blink__Darkloard(void);

static void
clear_selection_highlight__Darkloard(void);

static int
autoscroll_lines__Darkloard(void);

static bool
select_word__Darkloard(int64_t line, uint32_t col);

static void
autoscroll__Darkloard(void);

static void
update_event_mask__Darkloard(void);

static bool
mouse_reporting__Darkloard(unsigned int state);

static void
send_mouse_event__Darkloard(int button,
                            bool release,
                            bool motion,
                            unsigned int state,
                            int px,
                            int py);

static void
handle_button_press__Darkloard(XButtonEvent *event);

static void
handle_button_release__Darkloard(XButtonEvent *event);

static void
handle_motion__Darkloard(XMotionEvent *event);

static void
handle_keypress__Darkloard(XKeyEvent *event);

static void
handle_x_events__Darkloard(void);

static bool
handle_pty_events__Darkloard(void);

static void
poll__Darkloard(void);

static int
load_font__Darkloard(void);

static void
reload_font__Darkloard(unsigned int new_size);

static void
close__Darkloard(void);

struct DarkloardMessage
init__DarkloardMessage(char *buffer, size_t buffer_len)
{
    return (struct DarkloardMessage){ .buffer = buffer,
                                      .buffer_len = buffer_len,
                                      .next = NULL };
}

void
deinit__DarkloardMessage(struct DarkloardMessage *self)
{
    free(self->buffer);
    free(self);
}

void
deinit__DarkloardMessageList(struct DarkloardMessageList *self)
{
    struct DarkloardMessage *current = self->first;

    while (current) {
        struct DarkloardMessage *message_to_free = current;

        current = current->next;

        deinit__DarkloardMessage(message_to_free);
    }

    self->first = NULL;
    self->last = NULL;
}

void *
xmalloc__Darkloard(size_t size)
{
    void *ptr = malloc(size);

    if (!ptr) {
        LOG_ERROR("out of memory");
        exit(1);
    }

    return ptr;
}

void *
xrealloc__Darkloard(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);

    if (!new_ptr) {
        LOG_ERROR("out of memory");
        exit(1);
    }

    return new_ptr;
}

void
append__DarkloardBuffer(struct DarkloardBuffer *self,
                        const char *data,
                        size_t data_len)
{
    if (data_len == 0) {
        return;
    }

    if (self->len + data_len > self->cap) {
        size_t new_cap = self->cap ? self->cap : 4096;

        while (new_cap < self->len + data_len) {
            new_cap *= 2;
        }

        self->data = XREALLOC(self->data, new_cap);
        self->cap = new_cap;
    }

    memcpy(self->data + self->len, data, data_len);
    self->len += data_len;
}

void
xasprintf__Darkloard(char **buffer, const char *fmt, ...)
{
    va_list vl;

    va_start(vl, fmt);

    vasprintf(buffer, fmt, vl);

    if (!buffer) {
        LOG_ERROR("out of memory");
        exit(1);
    }

    va_end(vl);
}

int
configure_terminal__Darkloard(int slave_fd)
{
    struct termios tios;

    if (tcgetattr(slave_fd, &tios) == -1) {
        return 1;
    }

#if defined(__linux__)
#define C_IFLAG ICRNL | IUTF8
#else
#define C_IFLAG ICRNL
#endif

    tios.c_iflag |= C_IFLAG;
    tios.c_oflag |= OPOST | ONLCR;
    tios.c_lflag |= ISIG | ICANON | ECHO | IEXTEN;
    tios.c_cflag |= CREAD | CS8;

    if (tcsetattr(slave_fd, TCSADRAIN, &tios) == -1) {
        return 1;
    }

    return 0;

#undef C_IFLAG
}

bool
is_terminal_alive__Darkloard(void)
{
    if (terminal_process.shell_pid == 0 ||
        waitpid(terminal_process.shell_pid, NULL, WNOHANG) != 0) {

        return false;
    }

    return true;
}

int
open_terminal__Darkloard(int slave_fd, char *slave_filename)
{
    char *shell = getenv("SHELL");

    if (!shell) {
        shell = "/bin/sh";
    }

    char *shell_argv[] = { shell, NULL };

    pid_t pid = fork();

    switch (pid) {
        case 0: {
            if (setsid() == -1) {
                goto configure_error;
            } else if (ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
                goto configure_error;
            } else if (dup2(slave_fd, STDIN_FILENO) == -1) {
                goto configure_error;
            } else if (dup2(slave_fd, STDOUT_FILENO) == -1) {
                goto configure_error;
            } else if (dup2(slave_fd, STDERR_FILENO) == -1) {
                goto configure_error;
            } else if (configure_terminal__Darkloard(slave_fd) == -1) {
                goto configure_error;
            }

            close(slave_fd);

            struct passwd *pw = NULL;

            pw = getpwuid(getuid());

            if (!pw) {
                LOG_ERROR("failed to run `getpwuid`");

                goto configure_error;
            }

            if (setenv("LOGNAME", pw->pw_name, 1) == -1) {
                goto setenv_failed;
            } else if (setenv("USER", pw->pw_name, 1) == -1) {
                goto setenv_failed;
            } else if (setenv("SHELL", shell, 1) == -1) {
                goto setenv_failed;
            } else if (setenv("HOME", pw->pw_dir, 1) == -1) {
                goto setenv_failed;
            } else if (setenv("TERM", "xterm-256color", 1) == -1) {
                goto setenv_failed;
            }
#if defined(__linux__)
            else if (setenv("COLORTERM", "truecolor", 1) == -1) {
                goto setenv_failed;
            }
#elif defined(__FreeBSD__) || defined(__NetBSD__)
            else if (setenv("CLICOLOR", "1", 1) == -1) {
                goto setenv_failed;
            }
#else
#error "This OS is not supported"
#endif /* defined(__linux__) */

            execv(shell, shell_argv);
            exit(EXIT_SUCCESS);

        configure_error:
            LOG_ERROR("failed to configure the terminal: %s", strerror(errno));
            exit(EXIT_FAILURE);

        setenv_failed:
            LOG_ERROR("failed to execute setenv: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        case -1:
            LOG_ERROR("failed to fork process (%s)", strerror(errno));

            return 1;
        default:
            terminal_process.shell_pid = pid;
            terminal_process.shell = shell;

            return 0;
    }
}

int
open_pty__Darkloard(void)
{
    int res = 0;
    int slave_fd = -1;
    // We allocate PATH_MAX, just to be (very) safe.
    char *slave_filename = XMALLOC(PATH_MAX);

    if (openpty(&pty_master_fd, &slave_fd, slave_filename, NULL, NULL) == -1) {
        LOG_ERROR("failed to open pty (%s)", strerror(errno));

        goto error;
    }

    if (open_terminal__Darkloard(slave_fd, slave_filename)) {
        goto error;
    }

    int pty_master_fd_flags = fcntl(pty_master_fd, F_GETFL, 0);

    if (pty_master_fd_flags < 0) {
        LOG_ERROR("failed to flags from fd (%s)", strerror(errno));

        goto error;
    }

    if (fcntl(pty_master_fd, F_SETFL, pty_master_fd_flags | O_NONBLOCK) == -1) {
        goto error;
    }

    goto end;

error:
    res = 1;

end:
    free(slave_filename);

    if (slave_fd != -1) {
        close(slave_fd);
    }

    return res;
}

int
resize_pty__Darkloard(unsigned short row,
                      unsigned short col,
                      unsigned short xpixel,
                      unsigned short ypixel)
{
    struct winsize ws;

    memset(&ws, 0, sizeof(struct winsize));

    ws.ws_row = row;
    ws.ws_col = col;
    ws.ws_xpixel = xpixel;
    ws.ws_ypixel = ypixel;

    if (ioctl(pty_master_fd, TIOCSWINSZ, &ws) == -1) {
        LOG_ERROR("failed to resize");

        return 1;
    }

    terminal.row = row;
    terminal.col = col;

    return 0;
}

int
read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read)
{
    char *read_buffer = XMALLOC(BUFSIZ);

    memset(read_buffer, 0, BUFSIZ);

    ssize_t res = read(pty_master_fd, read_buffer, BUFSIZ - 1);

    if (res == -1) {
        *nbytes_read = 0;
        *read_buffer_ptr = NULL;

        free(read_buffer);

        return 1;
    }

    *read_buffer_ptr = read_buffer;
    *nbytes_read = res;

    return 0;
}

int
write_pty__Darkloard(char *buffer, size_t buffer_len)
{
    // Keep ordering: if something is already queued, queue behind it.
    if (pty_out.len > 0) {
        append__DarkloardBuffer(&pty_out, buffer, buffer_len);
        flush_pty__Darkloard();

        return 0;
    }

    while (buffer_len > 0) {
        ssize_t res = write(pty_master_fd, buffer, buffer_len);

        if (res == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            return 1;
        }

        buffer += res;
        buffer_len -= (size_t)res;
    }

    // The PTY is full: keep the remainder and write it once poll() reports
    // the fd as writable again.
    append__DarkloardBuffer(&pty_out, buffer, buffer_len);

    return 0;
}

void
flush_pty__Darkloard(void)
{
    size_t written = 0;

    while (written < pty_out.len) {
        ssize_t res =
          write(pty_master_fd, pty_out.data + written, pty_out.len - written);

        if (res == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Unrecoverable error: drop pending data.
                written = pty_out.len;
            }

            break;
        }

        written += (size_t)res;
    }

    memmove(pty_out.data, pty_out.data + written, pty_out.len - written);
    pty_out.len -= written;
}

void
init__DarkloardScreen(uint32_t rows, uint32_t cols)
{
    screen.rows = rows;
    screen.cols = cols;
    screen.current_fg = DARKLOARD_DEFAULT_FG;
    screen.current_bg = DARKLOARD_DEFAULT_BG;
    screen.current_attrs = 0;
    screen.cursor.row = 0;
    screen.cursor.col = 0;
    screen.cursor.visible = true;
    screen.scroll_region.top = 0;
    screen.scroll_region.bottom = rows - 1;
    screen.saved_cursor_valid = false;
    screen.pending_wrap = false;
    screen.auto_wrap = true;
    screen.insert_mode = false;
    screen.charsets[0] = 'B';
    screen.charsets[1] = 'B';
    screen.active_charset = 0;
    screen.last_char = ' ';
    screen.cells = XMALLOC(rows * sizeof(struct DarkloardCell *));

    for (uint32_t i = 0; i < rows; i++) {
        screen.cells[i] = XMALLOC(cols * sizeof(struct DarkloardCell));

        for (uint32_t j = 0; j < cols; j++) {
            screen.cells[i][j] =
              (struct DarkloardCell){ .codepoint = ' ',
                                      .fg = DARKLOARD_DEFAULT_FG,
                                      .bg = DARKLOARD_DEFAULT_BG,
                                      .attrs = 0 };
        }
    }

    in_alt_screen = false;
    inactive_cells = XMALLOC(rows * sizeof(struct DarkloardCell *));

    for (uint32_t i = 0; i < rows; i++) {
        inactive_cells[i] = XMALLOC(cols * sizeof(struct DarkloardCell));

        for (uint32_t j = 0; j < cols; j++) {
            inactive_cells[i][j] =
              (struct DarkloardCell){ .codepoint = ' ',
                                      .fg = DARKLOARD_DEFAULT_FG,
                                      .bg = DARKLOARD_DEFAULT_BG,
                                      .attrs = 0 };
        }
    }
}

void
free_cells__DarkloardScreen(void)
{
    if (screen.cells) {
        for (uint32_t i = 0; i < screen.rows; i++) {
            free(screen.cells[i]);
        }

        free(screen.cells);
        screen.cells = NULL;
    }

    if (inactive_cells) {
        for (uint32_t i = 0; i < screen.rows; i++) {
            free(inactive_cells[i]);
        }

        free(inactive_cells);
        inactive_cells = NULL;
    }
}

void
deinit__DarkloardScreen(void)
{
    free_cells__DarkloardScreen();
    clear_history__Darkloard();
}

// Returns a rows x cols copy of `old` (old_rows x old_cols), starting at
// old row `skip`: content is kept at the top-left, cropped or padded with
// blanks. Frees `old`.
static struct DarkloardCell **
resize_cells__DarkloardScreen(struct DarkloardCell **old,
                              uint32_t old_rows,
                              uint32_t old_cols,
                              uint32_t skip,
                              uint32_t rows,
                              uint32_t cols)
{
    struct DarkloardCell blank = { .codepoint = ' ',
                                   .fg = DARKLOARD_DEFAULT_FG,
                                   .bg = DARKLOARD_DEFAULT_BG,
                                   .attrs = 0 };
    struct DarkloardCell **cells = XMALLOC(rows * sizeof(struct DarkloardCell *));

    for (uint32_t i = 0; i < rows; i++) {
        cells[i] = XMALLOC(cols * sizeof(struct DarkloardCell));

        uint32_t src = i + skip;
        uint32_t copied = 0;

        if (src < old_rows) {
            copied = old_cols < cols ? old_cols : cols;

            memcpy(cells[i], old[src], copied * sizeof(struct DarkloardCell));
        }

        for (uint32_t j = copied; j < cols; j++) {
            cells[i][j] = blank;
        }
    }

    for (uint32_t i = 0; i < old_rows; i++) {
        free(old[i]);
    }

    free(old);

    return cells;
}

static void
clamp_cursor__DarkloardScreen(struct DarkloardCursor *cursor,
                              uint32_t rows,
                              uint32_t cols)
{
    if (cursor->row >= rows) {
        cursor->row = rows - 1;
    }

    if (cursor->col >= cols) {
        cursor->col = cols - 1;
    }
}

void
resize__DarkloardScreen(uint32_t rows, uint32_t cols)
{
    if (!screen.cells) {
        init__DarkloardScreen(rows, cols);

        return;
    }

    if (rows == screen.rows && cols == screen.cols) {
        return;
    }

    scroll_offset = 0;
    // Line numbers may shift when lines move to history.
    selection.active = false;
    selection.selecting = false;

    struct DarkloardCell ***main_cells =
      in_alt_screen ? &inactive_cells : &screen.cells;
    struct DarkloardCell ***alt_cells =
      in_alt_screen ? &screen.cells : &inactive_cells;
    struct DarkloardCursor *main_cursor =
      in_alt_screen ? &main_cursor_saved : &screen.cursor;

    // Main screen: keep its content. If it gets shorter than the cursor
    // position, move the top lines to the scrollback so the cursor line stays
    // visible (instead of losing the shell's output).
    uint32_t skip = 0;

    if (main_cursor->row >= rows) {
        skip = main_cursor->row - rows + 1;

        for (uint32_t i = 0; i < skip; i++) {
            push_history_line__Darkloard((*main_cells)[i], screen.cols);
        }

        main_cursor->row -= skip;
    }

    *main_cells = resize_cells__DarkloardScreen(
      *main_cells, screen.rows, screen.cols, skip, rows, cols);

    // Alternate screen: the application redraws it on SIGWINCH, just keep
    // what fits meanwhile.
    *alt_cells = resize_cells__DarkloardScreen(
      *alt_cells, screen.rows, screen.cols, 0, rows, cols);

    screen.rows = rows;
    screen.cols = cols;
    screen.scroll_region.top = 0;
    screen.scroll_region.bottom = rows - 1;
    screen.pending_wrap = false;

    clamp_cursor__DarkloardScreen(&screen.cursor, rows, cols);
    clamp_cursor__DarkloardScreen(&main_cursor_saved, rows, cols);
    clamp_cursor__DarkloardScreen(&screen.saved_cursor, rows, cols);

    screen.is_dirty = true;
}

void
scroll_up__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n)
{
    scroll_region_up__DarkloardScreen(top, bottom, n, true);
}

void
scroll_region_up__DarkloardScreen(uint32_t top,
                                  uint32_t bottom,
                                  uint32_t n,
                                  bool to_history)
{
    if (top > bottom || bottom >= screen.rows) {
        return;
    }

    uint32_t region_height = bottom - top + 1;
    bool save = (to_history && top == 0 && !in_alt_screen);

    if (n >= region_height) {
        if (save) {
            for (uint32_t i = top; i <= bottom; i++) {
                push_history_line__Darkloard(screen.cells[i], screen.cols);
            }
        }

        for (uint32_t i = top; i <= bottom; i++) {
            for (uint32_t j = 0; j < screen.cols; j++) {
                screen.cells[i][j] =
                  (struct DarkloardCell){ .codepoint = ' ',
                                          .fg = screen.current_fg,
                                          .bg = screen.current_bg,
                                          .attrs = 0 };
            }
        }

        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        struct DarkloardCell *evicted = screen.cells[top];

        if (save) {
            push_history_line__Darkloard(evicted, screen.cols);
        }

        for (uint32_t row = top; row < bottom; row++) {
            screen.cells[row] = screen.cells[row + 1];
        }

        screen.cells[bottom] = evicted;

        for (uint32_t j = 0; j < screen.cols; j++) {
            screen.cells[bottom][j] =
              (struct DarkloardCell){ .codepoint = ' ',
                                      .fg = screen.current_fg,
                                      .bg = screen.current_bg,
                                      .attrs = 0 };
        }
    }
}

void
scroll_down__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n)
{
    if (top > bottom || bottom >= screen.rows) {
        return;
    }

    uint32_t region_height = bottom - top + 1;

    if (n >= region_height) {
        for (uint32_t i = top; i <= bottom; i++) {
            for (uint32_t j = 0; j < screen.cols; j++) {
                screen.cells[i][j] =
                  (struct DarkloardCell){ .codepoint = ' ',
                                          .fg = screen.current_fg,
                                          .bg = screen.current_bg,
                                          .attrs = 0 };
            }
        }

        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        struct DarkloardCell *evicted = screen.cells[bottom];

        for (uint32_t row = bottom; row > top; row--) {
            screen.cells[row] = screen.cells[row - 1];
        }

        screen.cells[top] = evicted;

        for (uint32_t j = 0; j < screen.cols; j++) {
            screen.cells[top][j] =
              (struct DarkloardCell){ .codepoint = ' ',
                                      .fg = screen.current_fg,
                                      .bg = screen.current_bg,
                                      .attrs = 0 };
        }
    }
}

void
put_char__DarkloardScreen(uint32_t codepoint)
{
    if (!screen.cells || screen.cursor.row >= screen.rows ||
        screen.cursor.col >= screen.cols) {
        return;
    }

    // DEC special graphics (line drawing), used by ncurses for boxes.
    if (screen.charsets[screen.active_charset] == '0' && codepoint >= 0x5F &&
        codepoint <= 0x7E) {
        static const uint16_t dec_graphics[32] = {
            0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
            0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
            0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
            0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
        };

        codepoint = dec_graphics[codepoint - 0x5F];
    }

    screen.last_char = codepoint;

    int width = wcwidth((wchar_t)codepoint);

    if (width < 0) {
        width = 1;
    }

    /* Fire pending wrap before placing the character */
    if (screen.pending_wrap && screen.auto_wrap) {
        screen.pending_wrap = false;
        screen.cursor.col = 0;

        if (screen.cursor.row == screen.scroll_region.bottom) {
            scroll_up__DarkloardScreen(
              screen.scroll_region.top, screen.scroll_region.bottom, 1);
        } else if (screen.cursor.row < screen.rows - 1) {
            screen.cursor.row++;
        }
    } else {
        screen.pending_wrap = false;
    }

    /* Wide char that doesn't fit: pad with space and wrap */
    if (width == 2 && screen.cursor.col + 1 >= screen.cols) {
        screen.cells[screen.cursor.row][screen.cursor.col] =
          (struct DarkloardCell){ .codepoint = ' ',
                                  .fg = screen.current_fg,
                                  .bg = screen.current_bg,
                                  .attrs = screen.current_attrs };

        if (screen.auto_wrap) {
            screen.cursor.col = 0;

            if (screen.cursor.row == screen.scroll_region.bottom) {
                scroll_up__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            } else if (screen.cursor.row < screen.rows - 1) {
                screen.cursor.row++;
            }
        }
    }

    if (screen.insert_mode) {
        // IRM: shift the rest of the line right instead of overwriting.
        struct DarkloardCell *line = screen.cells[screen.cursor.row];
        uint32_t w = (uint32_t)width;

        for (uint32_t j = screen.cols - 1;
             j >= screen.cursor.col + w && j < screen.cols;
             j--) {
            line[j] = line[j - w];
        }
    }

    screen.cells[screen.cursor.row][screen.cursor.col] =
      (struct DarkloardCell){ .codepoint = codepoint,
                              .fg = screen.current_fg,
                              .bg = screen.current_bg,
                              .attrs = screen.current_attrs };

    if (width == 2 && screen.cursor.col + 1 < screen.cols) {
        screen.cells[screen.cursor.row][screen.cursor.col + 1] =
          (struct DarkloardCell){ .codepoint = 0,
                                  .fg = screen.current_fg,
                                  .bg = screen.current_bg,
                                  .attrs = screen.current_attrs };
    }

    screen.cursor.col += (uint32_t)width;

    if (screen.cursor.col >= screen.cols) {
        if (screen.auto_wrap) {
            /* Defer the wrap; cursor stays at last column */
            screen.cursor.col = screen.cols - 1;
            screen.pending_wrap = true;
        } else {
            screen.cursor.col = screen.cols - 1;
        }
    }
}

void
erase_display__DarkloardScreen(int mode)
{
    struct DarkloardCell blank = { .codepoint = ' ',
                                   .fg = screen.current_fg,
                                   .bg = screen.current_bg,
                                   .attrs = 0 };

    switch (mode) {
        case 0:
            for (uint32_t col = screen.cursor.col; col < screen.cols; col++) {
                screen.cells[screen.cursor.row][col] = blank;
            }

            for (uint32_t row = screen.cursor.row + 1; row < screen.rows;
                 row++) {
                for (uint32_t col = 0; col < screen.cols; col++) {
                    screen.cells[row][col] = blank;
                }
            }

            break;
        case 1:
            for (uint32_t row = 0; row < screen.cursor.row; row++) {
                for (uint32_t col = 0; col < screen.cols; col++) {
                    screen.cells[row][col] = blank;
                }
            }

            for (uint32_t col = 0; col <= screen.cursor.col; col++) {
                screen.cells[screen.cursor.row][col] = blank;
            }

            break;
        case 2:
        case 3:
            for (uint32_t row = 0; row < screen.rows; row++) {
                for (uint32_t col = 0; col < screen.cols; col++) {
                    screen.cells[row][col] = blank;
                }
            }

            break;
        default:
            break;
    }
}

void
erase_line__DarkloardScreen(int mode)
{
    struct DarkloardCell blank = { .codepoint = ' ',
                                   .fg = screen.current_fg,
                                   .bg = screen.current_bg,
                                   .attrs = 0 };

    switch (mode) {
        case 0:
            for (uint32_t col = screen.cursor.col; col < screen.cols; col++) {
                screen.cells[screen.cursor.row][col] = blank;
            }

            break;
        case 1:
            for (uint32_t col = 0; col <= screen.cursor.col; col++) {
                screen.cells[screen.cursor.row][col] = blank;
            }

            break;
        case 2:
            for (uint32_t col = 0; col < screen.cols; col++) {
                screen.cells[screen.cursor.row][col] = blank;
            }

            break;
        default:
            break;
    }
}

int
codepoint_to_utf8__Darkloard(uint32_t cp, char *buf)
{
    // See RFC 3629
    if (cp < 0x80) {
        buf[0] = (char)cp;

        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));

        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));

        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));

        return 4;
    }
}

void
handle_normal__DarkloardParser(unsigned char c)
{
    switch (c) {
        case DARKLOARD_NUL:
        case DARKLOARD_ENQ:
        case DARKLOARD_DC1:
        case DARKLOARD_DC3:
            break;
        case DARKLOARD_BEL:
            break;
        case DARKLOARD_BS:
            if (screen.cursor.col > 0) {
                screen.cursor.col--;
            }

            break;
        case DARKLOARD_HT: {
            uint32_t next_tab = (screen.cursor.col / DARKLOARD_TAB_WIDTH + 1) *
                                DARKLOARD_TAB_WIDTH;

            if (next_tab >= screen.cols) {
                next_tab = screen.cols - 1;
            }

            screen.cursor.col = next_tab;

            break;
        }
        case DARKLOARD_LF:
        case DARKLOARD_VT:
        case DARKLOARD_FF:
            screen.pending_wrap = false;

            if (screen.cursor.row == screen.scroll_region.bottom) {
                scroll_up__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            } else if (screen.cursor.row < screen.rows - 1) {
                screen.cursor.row++;
            }

            break;
        case DARKLOARD_CR:
            screen.pending_wrap = false;
            screen.cursor.col = 0;

            break;
        case DARKLOARD_SO:
            screen.active_charset = 1;

            break;
        case DARKLOARD_SI:
            screen.active_charset = 0;

            break;
        case DARKLOARD_CAN:
        case DARKLOARD_SUB:
            parser.utf8_remaining = 0;
            parser.state = DARKLOARD_PARSE_STATE_NORMAL;

            break;
        case DARKLOARD_ESC:
            parser.utf8_remaining = 0;
            parser.state = DARKLOARD_PARSE_STATE_ESC;

            break;
        case DARKLOARD_DEL:
            break;
        default:
            // Note: 8-bit C1 controls (e.g. 0x9B CSI) are not interpreted:
            // in UTF-8 those bytes are continuation bytes (0x9B is part of
            // "Û", for instance).
            if (c >= 0xF0 && c <= 0xF4) {
                parser.utf8_codepoint = c & 0x07;
                parser.utf8_remaining = 3;
            } else if (c >= 0xE0 && c <= 0xEF) {
                parser.utf8_codepoint = c & 0x0F;
                parser.utf8_remaining = 2;
            } else if (c >= 0xC2 && c <= 0xDF) {
                parser.utf8_codepoint = c & 0x1F;
                parser.utf8_remaining = 1;
            } else if (c >= 0x80 && c <= 0xBF) {
                if (parser.utf8_remaining > 0) {
                    parser.utf8_codepoint =
                      (parser.utf8_codepoint << 6) | (c & 0x3F);
                    parser.utf8_remaining--;

                    if (parser.utf8_remaining == 0) {
                        put_char__DarkloardScreen(parser.utf8_codepoint);
                    }
                }
            } else if (c >= 0x20) {
                parser.utf8_remaining = 0;

                put_char__DarkloardScreen(c);
            }

            break;
    }
}

void
handle_esc__DarkloardParser(unsigned char c)
{
    parser.state = DARKLOARD_PARSE_STATE_NORMAL;

    switch (c) {
        case '[':
            memset(&parser.csi, 0, sizeof(parser.csi));

            parser.state = DARKLOARD_PARSE_STATE_CSI;

            break;
        case ']':
            parser.osc_len = 0;
            parser.osc_overflow = false;
            parser.osc_esc_pending = false;
            parser.state = DARKLOARD_PARSE_STATE_OSC;

            break;
        case '7':
            screen.saved_cursor = screen.cursor;
            screen.saved_cursor_valid = true;

            break;
        case '8':
            if (screen.saved_cursor_valid) {
                screen.pending_wrap = false;
                screen.cursor = screen.saved_cursor;
            }

            break;
        case 'c':
            leave_alt_screen__Darkloard(false);

            app_cursor_keys = false;
            bracketed_paste = false;
            focus_events = false;
            cursor_shape = DARKLOARD_CURSOR_BLOCK;
            cursor_blink = DARKLOARD_CURSOR_BLINK;
            mouse_mode = DARKLOARD_MOUSE_NONE;
            mouse_sgr = false;
            mouse_urxvt = false;
            mouse_button_held = -1;

            update_event_mask__Darkloard();
            screen.cursor.visible = true;
            screen.saved_cursor_valid = false;
            screen.pending_wrap = false;
            screen.auto_wrap = true;
            screen.insert_mode = false;
            screen.charsets[0] = 'B';
            screen.charsets[1] = 'B';
            screen.active_charset = 0;
            screen.cursor.row = 0;
            screen.cursor.col = 0;
            screen.current_fg = DARKLOARD_DEFAULT_FG;
            screen.current_bg = DARKLOARD_DEFAULT_BG;
            screen.current_attrs = 0;
            screen.scroll_region.top = 0;
            screen.scroll_region.bottom = screen.rows - 1;

            erase_display__DarkloardScreen(2);

            break;
        case 'E':
            screen.cursor.col = 0;
            /* fall through */
        case 'D':
            // Same as LF: scroll only at the bottom margin, otherwise move
            // down (without leaving the screen).
            handle_normal__DarkloardParser(DARKLOARD_LF);

            break;
        case 'M':
            if (screen.cursor.row == screen.scroll_region.top) {
                scroll_down__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            } else if (screen.cursor.row > 0) {
                screen.cursor.row--;
            }

            break;
        case 'H':
            break;
        case '(':
        case ')':
            parser.charset_target = (c == '(') ? 0 : 1;
            parser.state = DARKLOARD_PARSE_STATE_CHARSET;

            break;
        case 'P':
        case '_':
        case '^':
        case 'X':
            parser.str_esc_pending = false;
            parser.state = DARKLOARD_PARSE_STATE_STR_SKIP;

            break;
        case '=':
        case '>':
            break;
        default:
            break;
    }
}

void
handle_csi_byte__DarkloardParser(unsigned char c)
{
    if (c == DARKLOARD_ESC) {
        // ESC aborts the sequence and starts a new one.
        parser.state = DARKLOARD_PARSE_STATE_ESC;
        return;
    }

    if (c == DARKLOARD_CAN || c == DARKLOARD_SUB) {
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;
        return;
    }

    if (c < 0x20) {
        // C0 controls inside a sequence are executed, the sequence goes on.
        handle_normal__DarkloardParser(c);
        return;
    }

    if (c == DARKLOARD_DEL) {
        return;
    }

    if (c == '<' || c == '=') {
        parser.csi.other_private = true;
        return;
    }

    if (c == '?') {
        parser.csi.private_mode = true;
        return;
    }

    if (c == '>') {
        parser.csi.secondary = true;
        return;
    }

    if (c >= 0x20 && c <= 0x2F) {
        parser.csi.intermediate = (char)c;
        return;
    }

    if (c >= '0' && c <= '9') {
        if (parser.csi.params_len == 0) {
            parser.csi.params_len = 1;
        }

        int idx = parser.csi.params_len - 1;

        // Cap values to avoid signed overflow on absurdly long parameters.
        if (parser.csi.params[idx] < 65535) {
            parser.csi.params[idx] = parser.csi.params[idx] * 10 + (c - '0');
        }

        return;
    }

    if (c == ';' || c == ':') {
        if (parser.csi.params_len == 0) {
            parser.csi.params_len = 1;
        }

        if (parser.csi.params_len < DARKLOARD_CSI_MAX_PARAMS) {
            parser.csi.is_sub[parser.csi.params_len] = (c == ':');
            parser.csi.params_len++;
        }

        return;
    }

    if (c >= 0x40 && c <= 0x7E) {
        parser.csi.final_byte = c;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        handle_csi__DarkloardParser();

        return;
    }

    parser.state = DARKLOARD_PARSE_STATE_NORMAL;
}

void
handle_csi__DarkloardParser(void)
{
#define CSI_PARAM(n, def)                                    \
    ((n) < parser.csi.params_len && parser.csi.params[n] > 0 \
       ? parser.csi.params[n]                                \
       : (def))

    if (parser.csi.other_private) {
        return;
    }

    // Sequences with a '?' / '>' prefix or an intermediate byte are different
    // commands (e.g. "CSI > 4 ; 2 m" is xterm's modifyOtherKeys, sent by vim,
    // not SGR). Only a few of them are supported, handled below.
    bool plain = !parser.csi.private_mode && !parser.csi.secondary &&
                 parser.csi.intermediate == 0;

    switch (parser.csi.final_byte) {
        case 'A': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row - n;
            // Stop at the top margin, unless the cursor is already above it.
            int limit = screen.cursor.row >= screen.scroll_region.top
                          ? (int)screen.scroll_region.top
                          : 0;

            screen.pending_wrap = false;
            screen.cursor.row =
              new_row < limit ? (uint32_t)limit : (uint32_t)new_row;

            break;
        }
        case 'B': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row + n;
            // Stop at the bottom margin, unless the cursor is already below.
            int limit = screen.cursor.row <= screen.scroll_region.bottom
                          ? (int)screen.scroll_region.bottom
                          : (int)screen.rows - 1;

            screen.pending_wrap = false;
            screen.cursor.row =
              new_row > limit ? (uint32_t)limit : (uint32_t)new_row;

            break;
        }
        case 'C': {
            int n = CSI_PARAM(0, 1);
            int new_col = (int)screen.cursor.col + n;

            screen.pending_wrap = false;
            screen.cursor.col =
              new_col >= (int)screen.cols ? screen.cols - 1 : (uint32_t)new_col;

            break;
        }
        case 'D': {
            int n = CSI_PARAM(0, 1);
            int new_col = (int)screen.cursor.col - n;

            screen.pending_wrap = false;
            screen.cursor.col = new_col < 0 ? 0 : (uint32_t)new_col;

            break;
        }
        case 'E': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row + n;
            // Stop at the bottom margin, unless the cursor is already below.
            int limit = screen.cursor.row <= screen.scroll_region.bottom
                          ? (int)screen.scroll_region.bottom
                          : (int)screen.rows - 1;

            screen.pending_wrap = false;
            screen.cursor.row =
              new_row > limit ? (uint32_t)limit : (uint32_t)new_row;
            screen.cursor.col = 0;

            break;
        }
        case 'F': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row - n;
            // Stop at the top margin, unless the cursor is already above it.
            int limit = screen.cursor.row >= screen.scroll_region.top
                          ? (int)screen.scroll_region.top
                          : 0;

            screen.pending_wrap = false;
            screen.cursor.row =
              new_row < limit ? (uint32_t)limit : (uint32_t)new_row;
            screen.cursor.col = 0;

            break;
        }
        case 'G': {
            int col = CSI_PARAM(0, 1) - 1;

            screen.pending_wrap = false;
            screen.cursor.col = (col < 0)                      ? 0
                                : (uint32_t)col >= screen.cols ? screen.cols - 1
                                                               : (uint32_t)col;
            break;
        }
        case 'H':
        case 'f': {
            int row = CSI_PARAM(0, 1) - 1;
            int col = CSI_PARAM(1, 1) - 1;

            screen.pending_wrap = false;
            screen.cursor.row = (row < 0)                      ? 0
                                : (uint32_t)row >= screen.rows ? screen.rows - 1
                                                               : (uint32_t)row;
            screen.cursor.col = (col < 0)                      ? 0
                                : (uint32_t)col >= screen.cols ? screen.cols - 1
                                                               : (uint32_t)col;
            break;
        }
        case 'J': {
            int mode = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;

            if (mode == 3) {
                // "Erase saved lines": clear the scrollback (sent by `clear`).
                erase_history__Darkloard();
            } else {
                erase_display__DarkloardScreen(mode);
            }

            break;
        }
        case 'K': {
            int mode = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;

            erase_line__DarkloardScreen(mode);

            break;
        }
        case 'L':
        case 'M': {
            int n = CSI_PARAM(0, 1);

            // Outside the scroll region these do nothing. Without this check
            // top > bottom made the scroll functions alias two row pointers
            // (heap corruption / double free).
            if (screen.cursor.row < screen.scroll_region.top ||
                screen.cursor.row > screen.scroll_region.bottom) {
                break;
            }

            screen.pending_wrap = false;
            screen.cursor.col = 0;

            if (parser.csi.final_byte == 'L') {
                scroll_down__DarkloardScreen(
                  screen.cursor.row, screen.scroll_region.bottom, (uint32_t)n);
            } else {
                // Deleted lines must not go to the scrollback.
                scroll_region_up__DarkloardScreen(screen.cursor.row,
                                                  screen.scroll_region.bottom,
                                                  (uint32_t)n,
                                                  false);
            }

            break;
        }
        case 'P': {
            int n = CSI_PARAM(0, 1);
            uint32_t row = screen.cursor.row;
            uint32_t col = screen.cursor.col;
            uint32_t shift = (uint32_t)n;

            for (uint32_t j = col; j + shift < screen.cols; j++) {
                screen.cells[row][j] = screen.cells[row][j + shift];
            }

            for (uint32_t j = screen.cols > shift ? screen.cols - shift : 0;
                 j < screen.cols;
                 j++) {
                screen.cells[row][j] =
                  (struct DarkloardCell){ .codepoint = ' ',
                                          .fg = screen.current_fg,
                                          .bg = screen.current_bg,
                                          .attrs = 0 };
            }

            break;
        }
        case 'S': {
            int n = CSI_PARAM(0, 1);

            scroll_up__DarkloardScreen(screen.scroll_region.top,
                                       screen.scroll_region.bottom,
                                       (uint32_t)n);
            break;
        }
        case 'T': {
            int n = CSI_PARAM(0, 1);

            scroll_down__DarkloardScreen(screen.scroll_region.top,
                                         screen.scroll_region.bottom,
                                         (uint32_t)n);
            break;
        }
        case 'X': {
            int n = CSI_PARAM(0, 1);

            for (int j = 0;
                 j < n && screen.cursor.col + (uint32_t)j < screen.cols;
                 j++) {
                screen
                  .cells[screen.cursor.row][screen.cursor.col + (uint32_t)j] =
                  (struct DarkloardCell){ .codepoint = ' ',
                                          .fg = screen.current_fg,
                                          .bg = screen.current_bg,
                                          .attrs = 0 };
            }

            break;
        }
        case '@': {
            int n = CSI_PARAM(0, 1);
            uint32_t row = screen.cursor.row;
            uint32_t col = screen.cursor.col;
            uint32_t shift = (uint32_t)n;

            for (uint32_t j = screen.cols - 1;
                 j >= col + shift && j < screen.cols;
                 j--) {
                screen.cells[row][j] = screen.cells[row][j - shift];
            }

            for (uint32_t j = col; j < col + shift && j < screen.cols; j++) {
                screen.cells[row][j] =
                  (struct DarkloardCell){ .codepoint = ' ',
                                          .fg = screen.current_fg,
                                          .bg = screen.current_bg,
                                          .attrs = 0 };
            }

            break;
        }
        case 'I': {
            int n = CSI_PARAM(0, 1);

            for (int j = 0; j < n; j++) {
                uint32_t next_tab =
                  (screen.cursor.col / DARKLOARD_TAB_WIDTH + 1) *
                  DARKLOARD_TAB_WIDTH;
                screen.cursor.col =
                  next_tab >= screen.cols ? screen.cols - 1 : next_tab;

                if (next_tab >= screen.cols) {
                    break;
                }
            }

            break;
        }
        case 'Z': {
            int n = CSI_PARAM(0, 1);

            for (int j = 0; j < n; j++) {
                if (screen.cursor.col == 0) {
                    break;
                }

                uint32_t prev_tab = (screen.cursor.col - 1) /
                                    DARKLOARD_TAB_WIDTH * DARKLOARD_TAB_WIDTH;
                screen.cursor.col = prev_tab;
            }

            break;
        }
        case 'b': {
            if (!plain) {
                break;
            }

            // REP: repeat the last printed character (bounded to a screen).
            uint32_t n = (uint32_t)CSI_PARAM(0, 1);
            uint32_t max = screen.rows * screen.cols;

            for (uint32_t k = 0; k < n && k < max; k++) {
                put_char__DarkloardScreen(screen.last_char);
            }

            break;
        }
        case 'c':
            if (!parser.csi.secondary) {
                write_pty__Darkloard("\033[?62;c", 7);
            } else {
                write_pty__Darkloard("\033[>1;10;0c", 10);
            }

            break;
        case 'd': {
            int row = CSI_PARAM(0, 1) - 1;

            screen.pending_wrap = false;
            screen.cursor.row = (row < 0)                      ? 0
                                : (uint32_t)row >= screen.rows ? screen.rows - 1
                                                               : (uint32_t)row;

            break;
        }
        case 'm':
            if (plain) {
                handle_sgr__DarkloardParser();
            }

            break;
        case 'q':
            // DECSCUSR: 0-2 block, 3-4 underline, 5-6 bar.
            if (parser.csi.intermediate == ' ' && !parser.csi.private_mode &&
                !parser.csi.secondary) {
                int shape = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;

                cursor_shape = shape >= 5   ? DARKLOARD_CURSOR_BAR
                               : shape >= 3 ? DARKLOARD_CURSOR_UNDERLINE
                                            : DARKLOARD_CURSOR_BLOCK;
                // 1, 3, 5 blink; 2, 4, 6 are steady; 0 is the default.
                cursor_blink =
                  shape == 0 ? DARKLOARD_CURSOR_BLINK : (shape % 2 == 1);

                reset_cursor_blink__Darkloard();
            }

            break;
        case 'n': {
            int mode = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;

            if (mode == 6) {
                char response[32];
                int len = snprintf(response,
                                   sizeof(response),
                                   "\033[%u;%uR",
                                   screen.cursor.row + 1,
                                   screen.cursor.col + 1);

                write_pty__Darkloard(response, (size_t)len);
            }

            break;
        }
        case 'r': {
            if (!plain) {
                break;
            }

            int top = CSI_PARAM(0, 1);
            int bottom = CSI_PARAM(1, (int)screen.rows);

            if (top < bottom && (uint32_t)bottom <= screen.rows) {
                screen.scroll_region.top = (uint32_t)(top - 1);
                screen.scroll_region.bottom = (uint32_t)(bottom - 1);
            }

            screen.pending_wrap = false;
            screen.cursor.row = 0;
            screen.cursor.col = 0;

            break;
        }
        case 'h':
        case 'l': {
            bool set = parser.csi.final_byte == 'h';

            for (int pi = 0;
                 !parser.csi.private_mode && !parser.csi.secondary &&
                 pi < parser.csi.params_len;
                 pi++) {
                if (parser.csi.params[pi] == 4) {
                    screen.insert_mode = set;
                }
            }

            for (int pi = 0;
                 parser.csi.private_mode && pi < parser.csi.params_len;
                 pi++) {
                int mode = parser.csi.params[pi];

                switch (mode) {
                    case 1:
                        app_cursor_keys = set;

                        break;
                    case 7:
                        screen.auto_wrap = set;

                        if (!set) {
                            screen.pending_wrap = false;
                        }

                        break;
                    case 12:
                        cursor_blink = set;

                        reset_cursor_blink__Darkloard();

                        break;
                    case 25:
                        screen.cursor.visible = set;

                        break;
                    case 47:
                    case 1047:
                        if (set) {
                            enter_alt_screen__Darkloard(false);
                        } else {
                            leave_alt_screen__Darkloard(false);
                        }

                        break;
                    case 9:
                    case 1000:
                    case 1002:
                    case 1003:
                        // Like xterm, resetting any tracking mode turns
                        // mouse reporting off.
                        mouse_mode = set ? (enum DarkloardMouseMode)mode
                                         : DARKLOARD_MOUSE_NONE;
                        mouse_button_held = -1;
                        mouse_last_row = UINT32_MAX;
                        mouse_last_col = UINT32_MAX;

                        update_event_mask__Darkloard();

                        break;
                    case 1006:
                        mouse_sgr = set;

                        break;
                    case 1015:
                        mouse_urxvt = set;

                        break;
                    case 1001:
                        break;
                    case 1004:
                        focus_events = set;

                        break;
                    case 1049:
                        if (set) {
                            enter_alt_screen__Darkloard(true);
                        } else {
                            leave_alt_screen__Darkloard(true);
                        }

                        break;
                    case 2004:
                        bracketed_paste = set;

                        break;
                    default:
                        break;
                }
            }

            break;
        }
        case 's':
            if (!plain) {
                break;
            }

            screen.saved_cursor = screen.cursor;
            screen.saved_cursor_valid = true;

            break;
        case 'u':
            if (!plain) {
                break;
            }

            if (screen.saved_cursor_valid) {
                screen.pending_wrap = false;
                screen.cursor = screen.saved_cursor;
            }

            break;
        default:
            break;
    }

#undef CSI_PARAM
}

void
handle_sgr__DarkloardParser(void)
{
    static const uint32_t ansi_colors[16] = {
        DARKLOARD_COLOR_0,  DARKLOARD_COLOR_1,  DARKLOARD_COLOR_2,
        DARKLOARD_COLOR_3,  DARKLOARD_COLOR_4,  DARKLOARD_COLOR_5,
        DARKLOARD_COLOR_6,  DARKLOARD_COLOR_7,  DARKLOARD_COLOR_8,
        DARKLOARD_COLOR_9,  DARKLOARD_COLOR_10, DARKLOARD_COLOR_11,
        DARKLOARD_COLOR_12, DARKLOARD_COLOR_13, DARKLOARD_COLOR_14,
        DARKLOARD_COLOR_15,
    };

    int params_len = parser.csi.params_len == 0 ? 1 : parser.csi.params_len;

    for (int i = 0; i < params_len; i++) {
        int p = parser.csi.params[i];
        // Sub-parameters ("4:3", "38:2::r:g:b") belonging to this one.
        int sub_start = i + 1;
        int sub_count = 0;

        while (sub_start + sub_count < params_len &&
               parser.csi.is_sub[sub_start + sub_count]) {
            sub_count++;
        }

        if (sub_count > 0) {
            const int *sub = &parser.csi.params[sub_start];

            i += sub_count;

            if (p == 4) {
                // Underline style: 0 = none, anything else = underlined.
                if (sub[0] == 0) {
                    screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_UNDERLINE;
                } else {
                    screen.current_attrs |= DARKLOARD_ATTR_UNDERLINE;
                }
            } else if (p == 38 || p == 48) {
                uint32_t color = 0;
                bool ok = false;

                if (sub[0] == 5 && sub_count >= 2) {
                    color = xterm256_to_rgb__Darkloard(sub[1]);
                    ok = true;
                } else if (sub[0] == 2 && sub_count >= 4) {
                    // 38:2:r:g:b or 38:2:colorspace:r:g:b
                    const int *rgb = sub_count >= 5 ? &sub[2] : &sub[1];

                    color = RGB(rgb[0] & 0xFF, rgb[1] & 0xFF, rgb[2] & 0xFF);
                    ok = true;
                }

                if (ok && p == 38) {
                    screen.current_fg = color;
                } else if (ok) {
                    screen.current_bg = color;
                }
            }

            // Other sub-parameters (e.g. underline color 58:...) are ignored.
            continue;
        }

        switch (p) {
            case 0:
                screen.current_fg = DARKLOARD_DEFAULT_FG;
                screen.current_bg = DARKLOARD_DEFAULT_BG;
                screen.current_attrs = 0;

                break;
            case 1:
                screen.current_attrs |= DARKLOARD_ATTR_BOLD;

                break;
            case 2:
                screen.current_attrs |= DARKLOARD_ATTR_DIM;

                break;
            case 3:
                screen.current_attrs |= DARKLOARD_ATTR_ITALIC;

                break;
            case 4:
                screen.current_attrs |= DARKLOARD_ATTR_UNDERLINE;

                break;
            case 5:
                screen.current_attrs |= DARKLOARD_ATTR_BLINK;

                break;
            case 7:
                screen.current_attrs |= DARKLOARD_ATTR_REVERSE;

                break;
            case 8:
                screen.current_attrs |= DARKLOARD_ATTR_INVISIBLE;

                break;
            case 9:
                screen.current_attrs |= DARKLOARD_ATTR_STRIKE;

                break;
            case 22:
                screen.current_attrs &=
                  (uint8_t)~(DARKLOARD_ATTR_BOLD | DARKLOARD_ATTR_DIM);

                break;
            case 23:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_ITALIC;

                break;
            case 24:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_UNDERLINE;

                break;
            case 25:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_BLINK;

                break;
            case 27:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_REVERSE;

                break;
            case 28:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_INVISIBLE;

                break;
            case 29:
                screen.current_attrs &= (uint8_t)~DARKLOARD_ATTR_STRIKE;

                break;
            case 30:
            case 31:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
                screen.current_fg = ansi_colors[p - 30];

                break;
            case 38:
                if (i + 2 < params_len && parser.csi.params[i + 1] == 5) {
                    screen.current_fg =
                      xterm256_to_rgb__Darkloard(parser.csi.params[i + 2]);
                    i += 2;
                } else if (i + 4 < params_len &&
                           parser.csi.params[i + 1] == 2) {
                    screen.current_fg = RGB(parser.csi.params[i + 2] & 0xFF,
                                            parser.csi.params[i + 3] & 0xFF,
                                            parser.csi.params[i + 4] & 0xFF);
                    i += 4;
                }

                break;
            case 39:
                screen.current_fg = DARKLOARD_DEFAULT_FG;

                break;
            case 40:
            case 41:
            case 42:
            case 43:
            case 44:
            case 45:
            case 46:
            case 47:
                screen.current_bg = ansi_colors[p - 40];

                break;
            case 48:
                if (i + 2 < params_len && parser.csi.params[i + 1] == 5) {
                    screen.current_bg =
                      xterm256_to_rgb__Darkloard(parser.csi.params[i + 2]);
                    i += 2;
                } else if (i + 4 < params_len &&
                           parser.csi.params[i + 1] == 2) {
                    screen.current_bg = RGB(parser.csi.params[i + 2] & 0xFF,
                                            parser.csi.params[i + 3] & 0xFF,
                                            parser.csi.params[i + 4] & 0xFF);
                    i += 4;
                }

                break;
            case 49:
                screen.current_bg = DARKLOARD_DEFAULT_BG;

                break;
            case 58:
                // Underline color: not supported, but skip its arguments so
                // they aren't taken as SGR codes ("2" would set dim).
                if (i + 1 < params_len && parser.csi.params[i + 1] == 5) {
                    i += 2;
                } else if (i + 1 < params_len &&
                           parser.csi.params[i + 1] == 2) {
                    i += 4;
                }

                break;
            case 90:
            case 91:
            case 92:
            case 93:
            case 94:
            case 95:
            case 96:
            case 97:
                screen.current_fg = ansi_colors[p - 90 + 8];

                break;
            case 100:
            case 101:
            case 102:
            case 103:
            case 104:
            case 105:
            case 106:
            case 107:
                screen.current_bg = ansi_colors[p - 100 + 8];

                break;
            default:
                break;
        }
    }
}

void
handle_osc_byte__DarkloardParser(unsigned char c)
{
    if (c == DARKLOARD_BEL) {
        parser.osc_bel = true;

        handle_osc__DarkloardParser();

        parser.osc_len = 0;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        return;
    }

    if (c == DARKLOARD_ESC) {
        parser.osc_esc_pending = true;

        return;
    }

    if (c == '\\' && parser.osc_esc_pending) {
        parser.osc_bel = false;

        handle_osc__DarkloardParser();

        parser.osc_len = 0;
        parser.osc_esc_pending = false;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        return;
    }

    if (c == DARKLOARD_CAN || c == DARKLOARD_SUB) {
        parser.osc_len = 0;
        parser.osc_esc_pending = false;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        return;
    }

    parser.osc_esc_pending = false;

    if (parser.osc_overflow) {
        return;
    }

    if (parser.osc_len + 1 >= DARKLOARD_OSC_MAX_SIZE) {
        parser.osc_overflow = true;
        return;
    }

    if (parser.osc_len + 1 >= parser.osc_cap) {
        parser.osc_cap = parser.osc_cap ? parser.osc_cap * 2 : 1024;
        parser.osc_buf = XREALLOC(parser.osc_buf, (size_t)parser.osc_cap);
    }

    parser.osc_buf[parser.osc_len++] = (char)c;
}

void
handle_osc__DarkloardParser(void)
{
    if (parser.osc_len < 1 || parser.osc_overflow) {
        return;
    }

    parser.osc_buf[parser.osc_len] = '\0';

    int cmd = 0;
    int i = 0;

    while (i < parser.osc_len && parser.osc_buf[i] >= '0' &&
           parser.osc_buf[i] <= '9') {
        cmd = cmd * 10 + (parser.osc_buf[i] - '0');
        i++;
    }

    if (i < parser.osc_len && parser.osc_buf[i] == ';') {
        switch (cmd) {
            case 0:
            case 2: {
                const char *title = parser.osc_buf + i + 1;
                int title_len = parser.osc_len - i - 1;

                XStoreName(display, window, title);
                XChangeProperty(display,
                                window,
                                atom_net_wm_name,
                                atom_utf8_string,
                                8,
                                PropModeReplace,
                                (const unsigned char *)title,
                                title_len);

                break;
            }
            case 4: {
                // "4;index;?" (possibly several pairs): palette query.
                char *p = parser.osc_buf + i + 1;

                while (*p) {
                    char *end = NULL;
                    long idx = strtol(p, &end, 10);

                    if (end == p || *end != ';') {
                        break;
                    }

                    p = end + 1;

                    if (p[0] == '?' && (p[1] == ';' || p[1] == '\0') &&
                        idx >= 0 && idx < 256) {
                        char key[16];

                        snprintf(key, sizeof(key), "4;%ld", idx);
                        send_osc_color__Darkloard(
                          key, xterm256_to_rgb__Darkloard((int)idx));
                    }

                    // Skip to the next pair.
                    while (*p && *p != ';') {
                        p++;
                    }

                    if (*p == ';') {
                        p++;
                    }
                }

                break;
            }
            case 52:
                handle_osc52__Darkloard(parser.osc_buf + i + 1);

                break;
            case 10:
            case 11:
            case 12: {
                // "?" asks for the foreground / background / cursor color
                // (vim, neovim and fish use it to pick a light/dark theme).
                if (strcmp(parser.osc_buf + i + 1, "?") == 0) {
                    uint32_t color = cmd == 10   ? DARKLOARD_DEFAULT_FG
                                     : cmd == 11 ? DARKLOARD_DEFAULT_BG
                                                 : DARKLOARD_CURSOR_COLOR;
                    char key[8];

                    snprintf(key, sizeof(key), "%d", cmd);
                    send_osc_color__Darkloard(key, color);
                }

                break;
            }
            default:
                break;
        }
    }
}

// Decodes base64 `in` into a new buffer (whitespace ignored, stops at '=').
// Returns NULL on invalid input.
static char *
base64_decode__Darkloard(const char *in, size_t *out_len)
{
    size_t in_len = strlen(in);
    char *out = XMALLOC(in_len / 4 * 3 + 4);
    uint32_t acc = 0;
    int bits = 0;
    size_t len = 0;

    for (const char *p = in; *p && *p != '='; p++) {
        int v;
        char c = *p;

        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        } else {
            free(out);
            return NULL;
        }

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out[len++] = (char)((acc >> bits) & 0xFF);
        }
    }

    *out_len = len;

    return out;
}

// OSC 52 ; Pc ; Pd: set the clipboard (Pc has 'c' or is empty) and/or the
// primary selection ('p' or 's') to the base64 data Pd. Used by neovim,
// tmux, etc. to copy, also over ssh. Reading the clipboard ("?") is
// deliberately not supported: any program could read it.
void
handle_osc52__Darkloard(const char *args)
{
    const char *data = strchr(args, ';');

    if (!data) {
        return;
    }

    size_t targets_len = (size_t)(data - args);
    bool to_clipboard = targets_len == 0 || memchr(args, 'c', targets_len);
    bool to_primary =
      memchr(args, 'p', targets_len) || memchr(args, 's', targets_len);

    data++;

    if (strcmp(data, "?") == 0) {
        return;
    }

    size_t len = 0;
    char *text = base64_decode__Darkloard(data, &len);

    if (!text) {
        return;
    }

    if (len == 0) {
        free(text);
        return;
    }

    if (to_primary) {
        deinit__DarkloardSelection();

        selection.text = XMALLOC(len);
        selection.text_len = len;

        memcpy(selection.text, text, len);
        XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    }

    if (to_clipboard) {
        free(clipboard_text);

        clipboard_text = text;
        clipboard_text_len = len;
        text = NULL;

        XSetSelectionOwner(display, atom_clipboard, window, CurrentTime);
    }

    free(text);
}

// Replies "OSC key ; rgb:rrrr/gggg/bbbb" with the query's terminator.
void
send_osc_color__Darkloard(const char *key, uint32_t color)
{
    unsigned r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    char buf[64];
    int len = snprintf(buf,
                       sizeof(buf),
                       "\033]%s;rgb:%02x%02x/%02x%02x/%02x%02x%s",
                       key,
                       r,
                       r,
                       g,
                       g,
                       b,
                       b,
                       parser.osc_bel ? "\a" : "\033\\");

    write_pty__Darkloard(buf, (size_t)len);
}

void
handle_str_skip__DarkloardParser(unsigned char c)
{
    if (c == DARKLOARD_ESC) {
        parser.str_esc_pending = true;

        return;
    }

    if (c == '\\' && parser.str_esc_pending) {
        parser.str_esc_pending = false;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        return;
    }

    if (c == DARKLOARD_BEL || c == DARKLOARD_CAN || c == DARKLOARD_SUB) {
        parser.str_esc_pending = false;
        parser.state = DARKLOARD_PARSE_STATE_NORMAL;

        return;
    }

    parser.str_esc_pending = false;
}

uint32_t
xterm256_to_rgb__Darkloard(int idx)
{
    static const uint32_t base[16] = {
        DARKLOARD_COLOR_0,  DARKLOARD_COLOR_1,  DARKLOARD_COLOR_2,
        DARKLOARD_COLOR_3,  DARKLOARD_COLOR_4,  DARKLOARD_COLOR_5,
        DARKLOARD_COLOR_6,  DARKLOARD_COLOR_7,  DARKLOARD_COLOR_8,
        DARKLOARD_COLOR_9,  DARKLOARD_COLOR_10, DARKLOARD_COLOR_11,
        DARKLOARD_COLOR_12, DARKLOARD_COLOR_13, DARKLOARD_COLOR_14,
        DARKLOARD_COLOR_15,
    };

    if (idx < 0) {
        return DARKLOARD_DEFAULT_FG;
    }

    if (idx < 16) {
        return base[idx];
    }

    if (idx < 232) {
        idx -= 16;

        int r = idx / 36, g = (idx / 6) % 6, b = idx % 6;
        int rv = r ? 55 + r * 40 : 0;
        int gv = g ? 55 + g * 40 : 0;
        int bv = b ? 55 + b * 40 : 0;

        return RGB(rv, gv, bv);
    }

    if (idx < 256) {
        int v = 8 + (idx - 232) * 10;

        return RGB(v, v, v);
    }

    return DARKLOARD_DEFAULT_FG;
}

void
feed__DarkloardParser(unsigned char c)
{
    switch (parser.state) {
        case DARKLOARD_PARSE_STATE_NORMAL:
            handle_normal__DarkloardParser(c);

            break;
        case DARKLOARD_PARSE_STATE_ESC:
            handle_esc__DarkloardParser(c);

            break;
        case DARKLOARD_PARSE_STATE_CSI:
            handle_csi_byte__DarkloardParser(c);

            break;
        case DARKLOARD_PARSE_STATE_OSC:
            handle_osc_byte__DarkloardParser(c);

            break;
        case DARKLOARD_PARSE_STATE_CHARSET:
            // Only DEC special graphics is special, anything else is ASCII.
            screen.charsets[parser.charset_target] = (c == '0') ? '0' : 'B';
            parser.state = DARKLOARD_PARSE_STATE_NORMAL;

            break;
        case DARKLOARD_PARSE_STATE_STR_SKIP:
            handle_str_skip__DarkloardParser(c);

            break;
        default:
            break;
    }
}

void
parse__Darkloard(struct DarkloardMessage *message)
{
    reset_cursor_blink__Darkloard();

    for (size_t i = 0; i < message->buffer_len; i++) {
        feed__DarkloardParser((unsigned char)message->buffer[i]);
    }

    screen.is_dirty = true;
}

void
enter_alt_screen__Darkloard(bool save_cursor)
{
    if (in_alt_screen) {
        return;
    }

    if (save_cursor) {
        main_cursor_saved = screen.cursor;
    }

    struct DarkloardCell **tmp = screen.cells;

    screen.cells = inactive_cells;
    inactive_cells = tmp;
    in_alt_screen = true;
    selection.active = false;
    selection.selecting = false;
    screen.pending_wrap = false;
    screen.cursor.row = 0;
    screen.cursor.col = 0;

    erase_display__DarkloardScreen(2);
}

void
leave_alt_screen__Darkloard(bool restore_cursor)
{
    if (!in_alt_screen) {
        return;
    }

    struct DarkloardCell **tmp = screen.cells;

    screen.cells = inactive_cells;
    inactive_cells = tmp;
    in_alt_screen = false;
    selection.active = false;
    selection.selecting = false;

    if (restore_cursor) {
        screen.pending_wrap = false;
        screen.cursor = main_cursor_saved;

        if (screen.cursor.row >= screen.rows) {
            screen.cursor.row = screen.rows > 0 ? screen.rows - 1 : 0;
        }

        if (screen.cursor.col >= screen.cols) {
            screen.cursor.col = screen.cols > 0 ? screen.cols - 1 : 0;
        }
    }
}

XftFont *
get_font_for_codepoint__Darkloard(uint32_t cp, uint8_t attrs)
{
    int variant = ((attrs & DARKLOARD_ATTR_BOLD) ? 1 : 0) |
                  ((attrs & DARKLOARD_ATTR_ITALIC) ? 2 : 0);
    XftFont *base = fonts[variant] ? fonts[variant] : fonts[FONT_REGULAR];

    if (XftCharExists(display, base, cp)) {
        return base;
    }

    for (int i = 0; i < font_cache_len; i++) {
        if (XftCharExists(display, font_cache[i], cp)) {
            return font_cache[i];
        }
    }

    FcPattern *pat = FcPatternCreate();

    FcPatternAddDouble(pat, FC_SIZE, (double)current_font_size);

    FcCharSet *cs = FcCharSetCreate();

    FcCharSetAddChar(cs, (FcChar32)cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcCharSetDestroy(cs);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);

    FcPatternDestroy(pat);

    if (!match) {
        return fonts[FONT_REGULAR];
    }

    XftFont *fb = XftFontOpenPattern(display, match);

    if (!fb) {
        return fonts[FONT_REGULAR];
    }

    if (font_cache_len < DARKLOARD_FONT_CACHE_SIZE) {
        font_cache[font_cache_len++] = fb;
    } else {
        XftFontClose(display, font_cache[0]);
        memmove(font_cache,
                font_cache + 1,
                (DARKLOARD_FONT_CACHE_SIZE - 1) * sizeof(XftFont *));

        font_cache[DARKLOARD_FONT_CACHE_SIZE - 1] = fb;
    }

    return fb;
}

void
push_history_line__Darkloard(struct DarkloardCell *line, uint32_t cols)
{
    if (history_lines[history_head] &&
        history_line_cols[history_head] != cols) {
        free(history_lines[history_head]);
        history_lines[history_head] = NULL;
    }

    if (!history_lines[history_head]) {
        history_lines[history_head] =
          XMALLOC(cols * sizeof(struct DarkloardCell));
    }

    memcpy(
      history_lines[history_head], line, cols * sizeof(struct DarkloardCell));

    history_line_cols[history_head] = cols;
    history_head = (history_head + 1) % DARKLOARD_HISTORY_LINES;

    if (history_count < DARKLOARD_HISTORY_LINES) {
        history_count++;
    } else {
        history_dropped++;
    }
}

static int
history_line_idx__Darkloard(int i)
{
    return ((history_head - history_count + i) % DARKLOARD_HISTORY_LINES +
            DARKLOARD_HISTORY_LINES) %
           DARKLOARD_HISTORY_LINES;
}

void
init_history__Darkloard(void)
{
    history_lines =
      XMALLOC(sizeof(struct DarkloardCell *) * DARKLOARD_HISTORY_LINES);
    history_line_cols = XMALLOC(sizeof(uint32_t) * DARKLOARD_HISTORY_LINES);

    // push_history_line__Darkloard() checks these slots for NULL.
    memset(history_lines, 0, sizeof(struct DarkloardCell *) * DARKLOARD_HISTORY_LINES);
    memset(history_line_cols, 0, sizeof(uint32_t) * DARKLOARD_HISTORY_LINES);
}

// Empties the scrollback but keeps it usable (unlike clear_history).
void
erase_history__Darkloard(void)
{
    for (int i = 0; i < DARKLOARD_HISTORY_LINES; i++) {
        free(history_lines[i]);

        history_lines[i] = NULL;
        history_line_cols[i] = 0;
    }

    history_head = 0;
    history_count = 0;
    history_dropped = 0;
    scroll_offset = 0;
    // Line numbers changed.
    selection.active = false;
    selection.selecting = false;
}

struct DarkloardCell *
get_history_line__Darkloard(int i)
{
    return history_lines[history_line_idx__Darkloard(i)];
}

static uint32_t
get_history_line_cols__Darkloard(int i)
{
    return history_line_cols[history_line_idx__Darkloard(i)];
}

void
clear_history__Darkloard(void)
{
    for (int i = 0; i < DARKLOARD_HISTORY_LINES; i++) {
        free(history_lines[i]);
        history_lines[i] = NULL;
        history_line_cols[i] = 0;
    }

    free(history_lines);
    free(history_line_cols);

    history_head = 0;
    history_count = 0;
    history_dropped = 0;
    scroll_offset = 0;
    history_lines = NULL;
    history_line_cols = NULL;
}

void
draw_glyph__Darkloard(XftDraw *draw,
                      uint32_t codepoint,
                      uint32_t color,
                      uint8_t attrs,
                      int cell_w,
                      int x,
                      int y)
{
    int baseline_y = y + fonts[FONT_REGULAR]->ascent;
    XftColor xft_color;
    XRenderColor xrc = { .red = (unsigned short)(((color >> 16) & 0xFF) * 257),
                         .green = (unsigned short)(((color >> 8) & 0xFF) * 257),
                         .blue = (unsigned short)(((color) & 0xFF) * 257),
                         .alpha = 0xFFFF };

    XftColorAllocValue(display, visual, colormap, &xrc, &xft_color);

    char utf8[5] = { 0 };
    int utf8_len = codepoint_to_utf8__Darkloard(codepoint, utf8);
    XftFont *glyph_font = get_font_for_codepoint__Darkloard(codepoint, attrs);

    XftDrawStringUtf8(
      draw, &xft_color, glyph_font, x, baseline_y, (FcChar8 *)utf8, utf8_len);
    XftColorFree(display, visual, colormap, &xft_color);

    if (attrs & DARKLOARD_ATTR_UNDERLINE) {
        XSetForeground(display, window_gc, color);
        XFillRectangle(display,
                       back_buffer,
                       window_gc,
                       x,
                       baseline_y + 1,
                       (unsigned int)cell_w,
                       1);
    }

    if (attrs & DARKLOARD_ATTR_STRIKE) {
        XSetForeground(display, window_gc, color);
        XFillRectangle(display,
                       back_buffer,
                       window_gc,
                       x,
                       y + fonts[FONT_REGULAR]->ascent / 2,
                       (unsigned int)cell_w,
                       1);
    }
}

void
draw__Darkloard(void)
{
    if (!screen.is_dirty || !screen.cells || !back_buffer) {
        return;
    }

    XSetForeground(display, window_gc, DARKLOARD_DEFAULT_BG);
    XFillRectangle(display,
                   back_buffer,
                   window_gc,
                   0,
                   0,
                   back_buffer_width,
                   back_buffer_height);

    XftDraw *draw = XftDrawCreate(display, back_buffer, visual, colormap);
    int cell_w = fonts[FONT_REGULAR]->max_advance_width;
    int cell_h = fonts[FONT_REGULAR]->ascent + fonts[FONT_REGULAR]->descent;

    for (uint32_t row = 0; row < screen.rows; row++) {
        struct DarkloardCell *row_cells = NULL;
        uint32_t row_cols = screen.cols;

        if (scroll_offset > 0) {
            int abs = (int)history_count - scroll_offset + (int)row;

            if (abs >= 0 && abs < (int)history_count) {
                uint32_t hcols = get_history_line_cols__Darkloard(abs);

                row_cells = get_history_line__Darkloard(abs);
                row_cols = hcols < screen.cols ? hcols : screen.cols;
            } else if (abs >= (int)history_count) {
                uint32_t srow = (uint32_t)(abs - (int)history_count);

                if (srow < screen.rows) {
                    row_cells = screen.cells[srow];
                }
            }
        } else {
            row_cells = screen.cells[row];
        }

        if (!row_cells) {
            continue;
        }

        for (uint32_t col = 0; col < row_cols; col++) {
            struct DarkloardCell *cell = &row_cells[col];
            uint32_t fg = cell->fg;
            uint32_t bg = cell->bg;

            if (cell->attrs & DARKLOARD_ATTR_REVERSE) {
                uint32_t tmp = fg;

                fg = bg;
                bg = tmp;
            }

            if (is_selected__DarkloardSelection(
                  view_row_to_line__Darkloard(row), col)) {
                fg = DARKLOARD_DEFAULT_BG;
                bg = DARKLOARD_SELECTION_BG;
            }

            int x = DARKLOARD_MARGIN_LEFT + (int)col * cell_w;
            int y = DARKLOARD_MARGIN_TOP + (int)row * cell_h;

            if (bg != DARKLOARD_DEFAULT_BG) {
                XSetForeground(display, window_gc, bg);
                XFillRectangle(display,
                               back_buffer,
                               window_gc,
                               x,
                               y,
                               (unsigned int)cell_w,
                               (unsigned int)cell_h);
            }

            if (cell->codepoint != 0 && cell->codepoint != ' ') {
                draw_glyph__Darkloard(
                  draw, cell->codepoint, fg, cell->attrs, cell_w, x, y);
            }
        }
    }

    bool blink_hidden = cursor_blink && window_focused && !cursor_blink_on;

    if (scroll_offset == 0 && screen.cursor.visible && !blink_hidden) {
        int cx = DARKLOARD_MARGIN_LEFT + (int)screen.cursor.col * cell_w;
        int cy = DARKLOARD_MARGIN_TOP + (int)screen.cursor.row * cell_h;

        XSetForeground(display, window_gc, DARKLOARD_CURSOR_COLOR);

        if (cursor_shape == DARKLOARD_CURSOR_UNDERLINE) {
            int h = cell_h / 10 > 2 ? cell_h / 10 : 2;

            XFillRectangle(display,
                           back_buffer,
                           window_gc,
                           cx,
                           cy + cell_h - h,
                           (unsigned int)cell_w,
                           (unsigned int)h);
        } else if (cursor_shape == DARKLOARD_CURSOR_BAR) {
            int w = cell_w / 6 > 2 ? cell_w / 6 : 2;

            XFillRectangle(
              display, back_buffer, window_gc, cx, cy, (unsigned int)w, (unsigned int)cell_h);
        } else {
            XFillRectangle(display,
                           back_buffer,
                           window_gc,
                           cx,
                           cy,
                           (unsigned int)cell_w,
                           (unsigned int)cell_h);
        }

        if (cursor_shape == DARKLOARD_CURSOR_BLOCK &&
            screen.cursor.row < screen.rows &&
            screen.cursor.col < screen.cols) {
            struct DarkloardCell *cur_cell =
              &screen.cells[screen.cursor.row][screen.cursor.col];

            if (cur_cell->codepoint != 0 && cur_cell->codepoint != ' ') {
                draw_glyph__Darkloard(draw,
                                      cur_cell->codepoint,
                                      DARKLOARD_DEFAULT_BG,
                                      cur_cell->attrs,
                                      cell_w,
                                      cx,
                                      cy);
            }
        }
    }

    XftDrawDestroy(draw);
    XCopyArea(display,
              back_buffer,
              window,
              window_gc,
              0,
              0,
              back_buffer_width,
              back_buffer_height,
              0,
              0);
    XFlush(display);

    screen.is_dirty = false;
}

void
handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel)
{
    window_width = xpixel;
    window_height = ypixel;

    unsigned short num_cols =
      (unsigned short)((xpixel - DARKLOARD_MARGIN_LEFT -
                        DARKLOARD_MARGIN_RIGHT) /
                       fonts[FONT_REGULAR]->max_advance_width);
    unsigned short num_rows = (unsigned short)((ypixel - DARKLOARD_MARGIN_TOP -
                                                DARKLOARD_MARGIN_BOTTOM) /
                                               (fonts[FONT_REGULAR]->ascent +
                                                fonts[FONT_REGULAR]->descent));

    if (num_rows == 0 || num_cols == 0) {
        return;
    }

    if (back_buffer) {
        XFreePixmap(display, back_buffer);
    }

    back_buffer =
      XCreatePixmap(display,
                    window,
                    xpixel,
                    ypixel,
                    (unsigned int)DefaultDepth(display, screen_num));
    back_buffer_width = xpixel;
    back_buffer_height = ypixel;

    resize_pty__Darkloard(num_rows, num_cols, xpixel, ypixel);
    resize__DarkloardScreen(num_rows, num_cols);
}

void
init_atoms__Darkloard(void)
{
    atom_utf8_string = XInternAtom(display, "UTF8_STRING", False);
    atom_clipboard = XInternAtom(display, "CLIPBOARD", False);
    atom_targets = XInternAtom(display, "TARGETS", False);
    atom_xsel_data = XInternAtom(display, "XSEL_DATA", False);
    atom_incr = XInternAtom(display, "INCR", False);
    atom_wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    atom_net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);

    XSetWMProtocols(display, window, &atom_wm_delete, 1);
}

void
deinit__DarkloardSelection(void)
{
    free(selection.text);

    selection.text = NULL;
    selection.text_len = 0;
    selection.active = false;
    selection.selecting = false;
}

void
pixel_to_cell__Darkloard(int px, int py, uint32_t *row, uint32_t *col)
{
    int cell_w = fonts[FONT_REGULAR]->max_advance_width;
    int cell_h = fonts[FONT_REGULAR]->ascent + fonts[FONT_REGULAR]->descent;
    int r = (py - DARKLOARD_MARGIN_TOP) / cell_h;
    int c = (px - DARKLOARD_MARGIN_LEFT) / cell_w;

    *row = r < 0                        ? 0
           : (uint32_t)r >= screen.rows ? screen.rows - 1
                                        : (uint32_t)r;
    *col = c < 0                        ? 0
           : (uint32_t)c >= screen.cols ? screen.cols - 1
                                        : (uint32_t)c;
}

// Absolute line number of a visible row: lines are numbered from the oldest
// line ever pushed to history, followed by the screen rows.
int64_t
view_row_to_line__Darkloard(uint32_t row)
{
    return history_dropped + history_count - scroll_offset + (int64_t)row;
}

// Cells of an absolute line (history or screen), or NULL if it no longer
// exists.
struct DarkloardCell *
get_line_cells__Darkloard(int64_t line, uint32_t *cols)
{
    int64_t idx = line - history_dropped;

    if (idx < 0) {
        return NULL;
    }

    if (idx < history_count) {
        *cols = get_history_line_cols__Darkloard((int)idx);

        return get_history_line__Darkloard((int)idx);
    }

    idx -= history_count;

    if (!screen.cells || idx >= screen.rows) {
        return NULL;
    }

    *cols = screen.cols;

    return screen.cells[idx];
}

void
pixel_to_selection__Darkloard(int px, int py, int64_t *line, uint32_t *col)
{
    uint32_t row;

    pixel_to_cell__Darkloard(px, py, &row, col);

    *line = view_row_to_line__Darkloard(row);
}

bool
is_selected__DarkloardSelection(int64_t row, uint32_t col)
{
    if (!selection.active && !selection.selecting) {
        return false;
    }

    int64_t r1 = selection.start_row, r2 = selection.end_row;
    uint32_t c1 = selection.start_col, c2 = selection.end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int64_t tr = r1;

        r1 = r2;
        r2 = tr;

        uint32_t tc = c1;

        c1 = c2;
        c2 = tc;
    }

    if (row < r1 || row > r2) {
        return false;
    }

    if (row == r1 && col < c1) {
        return false;
    }

    if (row == r2 && col > c2) {
        return false;
    }

    return true;
}

void
build_selection_text__DarkloardSelection(void)
{
    free(selection.text);

    selection.text = NULL;
    selection.text_len = 0;

    if (!screen.cells) {
        return;
    }

    int64_t r1 = selection.start_row, r2 = selection.end_row;
    uint32_t c1 = selection.start_col, c2 = selection.end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int64_t tr = r1;

        r1 = r2;
        r2 = tr;

        uint32_t tc = c1;

        c1 = c2;
        c2 = tc;
    }

    struct DarkloardBuffer out = { 0 };

    for (int64_t row = r1; row <= r2; row++) {
        uint32_t cols = 0;
        struct DarkloardCell *cells = get_line_cells__Darkloard(row, &cols);
        uint32_t col_start = (row == r1) ? c1 : 0;
        uint32_t col_end = (row == r2) ? c2 : cols - 1;

        if (cells && cols > 0 && col_start < cols) {
            if (col_end >= cols) {
                col_end = cols - 1;
            }

            // Find last non-space in this row segment to strip trailing
            // whitespace.
            uint32_t last = col_start;
            bool has_content = false;

            for (uint32_t c = col_end + 1; c-- > col_start;) {
                uint32_t cp = cells[c].codepoint;

                if (cp != 0 && cp != ' ') {
                    last = c;
                    has_content = true;
                    break;
                }
            }

            if (has_content) {
                for (uint32_t c = col_start; c <= last; c++) {
                    uint32_t cp = cells[c].codepoint;

                    // Right half of a wide character: nothing to copy.
                    if (cp == 0) {
                        continue;
                    }

                    char utf8[5] = { 0 };
                    int utf8_len = codepoint_to_utf8__Darkloard(cp, utf8);

                    append__DarkloardBuffer(&out, utf8, (size_t)utf8_len);
                }
            }
        }

        if (row < r2) {
            append__DarkloardBuffer(&out, "\n", 1);
        }
    }

    char *buf = out.data;
    size_t len = out.len;

    selection.text = buf;
    selection.text_len = len;
}

void
request_paste__Darkloard(Atom sel_type)
{
    XConvertSelection(
      display, sel_type, atom_utf8_string, atom_xsel_data, window, CurrentTime);
}

void
handle_selection_request__Darkloard(XSelectionRequestEvent *req)
{
    XSelectionEvent notify = {
        .type = SelectionNotify,
        .display = req->display,
        .requestor = req->requestor,
        .selection = req->selection,
        .target = req->target,
        .property = None,
        .time = req->time,
    };

    const char *text = selection.text;
    size_t text_len = selection.text_len;

    if (req->selection == atom_clipboard) {
        text = clipboard_text;
        text_len = clipboard_text_len;
    }

    if (!text || text_len == 0) {
        XSendEvent(req->display, req->requestor, False, 0, (XEvent *)&notify);
        return;
    }

    if (req->target == atom_targets) {
        Atom supported[2] = { atom_utf8_string, XA_STRING };

        XChangeProperty(req->display,
                        req->requestor,
                        req->property,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        (unsigned char *)supported,
                        2);
        notify.property = req->property;
    } else if (req->target == atom_utf8_string || req->target == XA_STRING) {
        XChangeProperty(req->display,
                        req->requestor,
                        req->property,
                        req->target,
                        8,
                        PropModeReplace,
                        (unsigned char *)text,
                        (int)text_len);

        notify.property = req->property;
    }

    XSendEvent(req->display, req->requestor, False, 0, (XEvent *)&notify);
}

// Reads the whole XSEL_DATA property (in chunks, so there is no size cap),
// appends it to paste_in and deletes the property. Returns the property type.
static Atom
read_selection_property__Darkloard(void)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    long offset = 0;

    do {
        unsigned char *data = NULL;

        if (XGetWindowProperty(display,
                               window,
                               atom_xsel_data,
                               offset,
                               64 * 1024L, // in 32-bit units (256KiB)
                               False,
                               AnyPropertyType,
                               &actual_type,
                               &actual_format,
                               &nitems,
                               &bytes_after,
                               &data) != Success) {
            break;
        }

        if (data && nitems > 0 && actual_format == 8 &&
            actual_type == XA_STRING) {
            // STRING is Latin-1: convert to UTF-8.
            for (unsigned long i = 0; i < nitems; i++) {
                unsigned char c = data[i];

                if (c < 0x80) {
                    append__DarkloardBuffer(&paste_in, (char *)&c, 1);
                } else {
                    char utf8[2] = { (char)(0xC0 | (c >> 6)),
                                     (char)(0x80 | (c & 0x3F)) };

                    append__DarkloardBuffer(&paste_in, utf8, 2);
                }
            }
        } else if (data && nitems > 0 && actual_format == 8 &&
                   actual_type != atom_incr) {
            append__DarkloardBuffer(&paste_in, (char *)data, nitems);
        }

        if (data) {
            XFree(data);
        }

        offset += (long)(nitems * (unsigned long)actual_format / 32);
    } while (bytes_after > 0 && nitems > 0);

    XDeleteProperty(display, window, atom_xsel_data);

    return actual_type;
}

static void
finish_paste__Darkloard(void)
{
    if (paste_in.len > 0) {
        if (bracketed_paste) {
            // Drop ESC bytes so the pasted text can't contain its own
            // "\033[201~" terminator and escape bracketed paste: otherwise
            // copied text could inject commands that the shell runs.
            size_t out = 0;

            for (size_t i = 0; i < paste_in.len; i++) {
                if (paste_in.data[i] != '\033') {
                    paste_in.data[out++] = paste_in.data[i];
                }
            }

            paste_in.len = out;

            write_pty__Darkloard("\033[200~", 6);
        }

        write_pty__Darkloard(paste_in.data, paste_in.len);

        if (bracketed_paste) {
            write_pty__Darkloard("\033[201~", 6);
        }
    }

    free(paste_in.data);

    paste_in = (struct DarkloardBuffer){ 0 };
    paste_incr = false;
}

void
handle_selection_notify__Darkloard(XSelectionEvent *event)
{
    if (event->property == None) {
        // The owner can't provide UTF8_STRING: fall back to plain STRING.
        if (event->target == atom_utf8_string) {
            XConvertSelection(display,
                              event->selection,
                              XA_STRING,
                              atom_xsel_data,
                              window,
                              CurrentTime);
        }

        return;
    }

    // A new paste replaces any unfinished INCR transfer.
    free(paste_in.data);

    paste_in = (struct DarkloardBuffer){ 0 };
    paste_incr = false;

    // Large selections are sent with the INCR protocol: the owner gives us an
    // INCR property, we delete it, then receive the data in chunks through
    // PropertyNotify events until an empty chunk arrives.
    if (read_selection_property__Darkloard() == atom_incr) {
        paste_incr = true;

        return;
    }

    finish_paste__Darkloard();
}

void
handle_property_notify__Darkloard(XPropertyEvent *event)
{
    if (!paste_incr || event->atom != atom_xsel_data ||
        event->state != PropertyNewValue) {
        return;
    }

    size_t prev_len = paste_in.len;

    read_selection_property__Darkloard();

    if (paste_in.len == prev_len) {
        // Zero-length chunk: end of INCR transfer.
        finish_paste__Darkloard();
    }
}

// Only ask X for the motion events we need: all pointer motion is a lot of
// events, and is only needed for mouse mode 1003.
void
update_event_mask__Darkloard(void)
{
    long mask = ExposureMask | KeyPressMask | StructureNotifyMask |
                ButtonPressMask | ButtonReleaseMask | FocusChangeMask |
                PropertyChangeMask;

    if (mouse_mode == DARKLOARD_MOUSE_ANY) {
        mask |= PointerMotionMask;
    } else if (mouse_mode == DARKLOARD_MOUSE_BUTTON) {
        mask |= ButtonMotionMask;
    } else {
        mask |= Button1MotionMask; // selection
    }

    XSelectInput(display, window, mask);
}

// Whether a mouse event goes to the application. Holding Shift keeps the
// terminal's own selection / scrolling, as in xterm.
bool
mouse_reporting__Darkloard(unsigned int state)
{
    return mouse_mode != DARKLOARD_MOUSE_NONE && !(state & ShiftMask);
}

// button: 0-2 (left, middle, right), 3 (no button, for motion), 64-67
// (wheel).
void
send_mouse_event__Darkloard(int button,
                            bool release,
                            bool motion,
                            unsigned int state,
                            int px,
                            int py)
{
    uint32_t row, col;

    pixel_to_cell__Darkloard(px, py, &row, &col);

    if (motion) {
        if (row == mouse_last_row && col == mouse_last_col) {
            return;
        }
    }

    mouse_last_row = row;
    mouse_last_col = col;

    int cb = button;

    if (mouse_mode != DARKLOARD_MOUSE_X10) {
        if (state & ShiftMask) {
            cb += 4;
        }

        if (state & Mod1Mask) {
            cb += 8;
        }

        if (state & ControlMask) {
            cb += 16;
        }
    }

    if (motion) {
        cb += 32;
    }

    int x = (int)col + 1;
    int y = (int)row + 1;
    char buf[64];
    int len;

    if (mouse_sgr) {
        // SGR keeps the released button and has no coordinate limit.
        len = snprintf(buf,
                       sizeof(buf),
                       "\033[<%d;%d;%d%c",
                       cb,
                       x,
                       y,
                       release ? 'm' : 'M');
    } else {
        if (release) {
            cb = (cb & ~3) | 3; // legacy encodings: release is button 3
        }

        if (mouse_urxvt) {
            len = snprintf(
              buf, sizeof(buf), "\033[%d;%d;%dM", cb + 32, x, y);
        } else {
            // Legacy encoding: one byte each, so coordinates stop at 223.
            if (x > 223 || y > 223) {
                return;
            }

            len = snprintf(buf,
                           sizeof(buf),
                           "\033[M%c%c%c",
                           (char)(cb + 32),
                           (char)(x + 32),
                           (char)(y + 32));
        }
    }

    // The view must show what the application sees.
    scroll_offset = 0;

    write_pty__Darkloard(buf, (size_t)len);
}

void
handle_button_press__Darkloard(XButtonEvent *event)
{
    if (mouse_reporting__Darkloard(event->state)) {
        int button = -1;

        switch (event->button) {
            case Button1:
            case Button2:
            case Button3:
                button = (int)event->button - Button1;
                mouse_button_held = button;

                break;
            case Button4:
            case Button5:
                button = 64 + (int)event->button - Button4;

                break;
            case 6:
            case 7:
                button = 66 + (int)event->button - 6;

                break;
            default:
                break;
        }

        // X10 mode doesn't report the wheel.
        if (button >= 0 &&
            !(mouse_mode == DARKLOARD_MOUSE_X10 && button >= 64)) {
            send_mouse_event__Darkloard(
              button, false, false, event->state, event->x, event->y);
        }

        return;
    }

    if (event->button == Button1) {
        int64_t line;
        uint32_t col;

        pixel_to_selection__Darkloard(event->x, event->y, &line, &col);

        bool double_click = last_click_line == line && last_click_col == col &&
                            event->time - last_click_time <=
                              DARKLOARD_DOUBLE_CLICK_MS;

        last_click_time = event->time;
        last_click_line = line;
        last_click_col = col;

        if (double_click && select_word__Darkloard(line, col)) {
            // A third click starts a new double-click detection.
            last_click_line = -1;

            return;
        }

        deinit__DarkloardSelection();

        selection.selecting = true;

        pixel_to_selection__Darkloard(
          event->x, event->y, &selection.start_row, &selection.start_col);

        selection.end_row = selection.start_row;
        selection.end_col = selection.start_col;
        drag_x = event->x;
        drag_y = event->y;
    } else if (event->button == Button2) {
        clear_selection_highlight__Darkloard();
        request_paste__Darkloard(XA_PRIMARY);
    } else if (event->button == Button4) {
        if (in_alt_screen) {
            for (int i = 0; i < 3; i++) {
                write_pty__Darkloard(app_cursor_keys ? "\033OA" : "\033[A", 3);
            }
        } else {
            scroll_offset += 3;

            if (scroll_offset > history_count) {
                scroll_offset = history_count;
            }
        }
    } else if (event->button == Button5) {
        if (in_alt_screen) {
            for (int i = 0; i < 3; i++) {
                write_pty__Darkloard(app_cursor_keys ? "\033OB" : "\033[B", 3);
            }
        } else {
            scroll_offset -= 3;

            if (scroll_offset < 0) {
                scroll_offset = 0;
            }
        }
    }

    // Scrolling with the wheel while dragging extends the selection.
    if (selection.selecting &&
        (event->button == Button4 || event->button == Button5)) {
        pixel_to_selection__Darkloard(
          drag_x, drag_y, &selection.end_row, &selection.end_col);
    }
}

void
handle_button_release__Darkloard(XButtonEvent *event)
{
    // A selection started with Shift ends normally even if Shift was
    // released meanwhile.
    if (!selection.selecting && mouse_reporting__Darkloard(event->state)) {
        if (event->button >= Button1 && event->button <= Button3) {
            int button = (int)event->button - Button1;

            if (button == mouse_button_held) {
                mouse_button_held = -1;
            }

            if (mouse_mode != DARKLOARD_MOUSE_X10) {
                send_mouse_event__Darkloard(
                  button, true, false, event->state, event->x, event->y);
            }
        }

        return;
    }

    if (event->button != Button1 || !selection.selecting) {
        return;
    }

    selection.selecting = false;

    pixel_to_selection__Darkloard(
      event->x, event->y, &selection.end_row, &selection.end_col);

    if (selection.start_row == selection.end_row &&
        selection.start_col == selection.end_col) {
        return;
    }

    selection.active = true;

    build_selection_text__DarkloardSelection();
    XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
}

void
handle_motion__Darkloard(XMotionEvent *event)
{
    if (selection.selecting) {
        drag_x = event->x;
        drag_y = event->y;

        pixel_to_selection__Darkloard(
          event->x, event->y, &selection.end_row, &selection.end_col);

        return;
    }

    if (!mouse_reporting__Darkloard(event->state)) {
        return;
    }

    bool held = mouse_button_held >= 0;

    if (mouse_mode == DARKLOARD_MOUSE_ANY ||
        (mouse_mode == DARKLOARD_MOUSE_BUTTON && held)) {
        send_mouse_event__Darkloard(held ? mouse_button_held : 3,
                                    false,
                                    true,
                                    event->state,
                                    event->x,
                                    event->y);
    }
}

void
handle_keypress__Darkloard(XKeyEvent *event)
{
    char buf[32];
    KeySym keysym = NoSymbol;
    int len = XLookupString(event, buf, (int)sizeof(buf) - 1, &keysym, NULL);

    if (event->state & ShiftMask) {
        switch (keysym) {
            case XK_Page_Up:
                scroll_offset += (int)screen.rows / 2;

                if (scroll_offset > history_count) {
                    scroll_offset = history_count;
                }

                return;
            case XK_Page_Down:
                scroll_offset -= (int)screen.rows / 2;

                if (scroll_offset < 0) {
                    scroll_offset = 0;
                }

                return;
            default:
                break;
        }
    }

    if ((event->state & ControlMask) && (event->state & ShiftMask)) {
        switch (keysym) {
            case XK_c:
            case XK_C:
                if (selection.active && selection.text_len > 0) {
                    free(clipboard_text);

                    clipboard_text = XMALLOC(selection.text_len);
                    clipboard_text_len = selection.text_len;

                    memcpy(clipboard_text, selection.text, selection.text_len);
                    XSetSelectionOwner(
                      display, atom_clipboard, window, CurrentTime);
                }

                return;
            case XK_v:
            case XK_V:
                scroll_offset = 0;

                clear_selection_highlight__Darkloard();

                request_paste__Darkloard(atom_clipboard);

                return;
            default:
                break;
        }
    }

    if (event->state & ControlMask) {
        switch (keysym) {
            case XK_plus:
            case XK_equal:
            case XK_KP_Add:
                if (current_font_size < 72) {
                    reload_font__Darkloard(current_font_size + 1);
                }

                return;
            case XK_minus:
            case XK_KP_Subtract:
                if (current_font_size > 4) {
                    reload_font__Darkloard(current_font_size - 1);
                }

                return;
            case XK_0:
            case XK_KP_0:
                reload_font__Darkloard(DARKLOARD_FONT_SIZE);

                return;
            default:
                break;
        }
    }

    // Pressing a lone modifier (Shift, Ctrl, ...) must not leave scrollback.
    if (IsModifierKey(keysym)) {
        return;
    }

    // Typing returns the view to the bottom.
    scroll_offset = 0;

    reset_cursor_blink__Darkloard();
    clear_selection_highlight__Darkloard();

    if (len > 0) {
        write_pty__Darkloard(buf, (size_t)len);

        return;
    }

    switch (keysym) {
        case XK_Return:
        case XK_KP_Enter:
            write_pty__Darkloard("\r", 1);

            break;
        case XK_BackSpace:
            write_pty__Darkloard("\177", 1);

            break;
        case XK_Delete:
            write_pty__Darkloard("\033[3~", 4);

            break;
        case XK_Up:
            write_pty__Darkloard(app_cursor_keys ? "\033OA" : "\033[A", 3);

            break;
        case XK_Down:
            write_pty__Darkloard(app_cursor_keys ? "\033OB" : "\033[B", 3);

            break;
        case XK_Right:
            write_pty__Darkloard(app_cursor_keys ? "\033OC" : "\033[C", 3);

            break;
        case XK_Left:
            write_pty__Darkloard(app_cursor_keys ? "\033OD" : "\033[D", 3);

            break;
        case XK_Home:
            write_pty__Darkloard("\033[H", 3);

            break;
        case XK_End:
            write_pty__Darkloard("\033[F", 3);

            break;
        case XK_Page_Up:
            write_pty__Darkloard("\033[5~", 4);

            break;
        case XK_Page_Down:
            write_pty__Darkloard("\033[6~", 4);

            break;
        default:
            break;
    }
}

void
handle_x_events__Darkloard(void)
{
    while (XPending(display)) {
        XEvent event;

        XNextEvent(display, &event);

        bool was_dirty = screen.is_dirty;

        // Results of our own XCopyArea: nothing changed.
        if (event.type == NoExpose || event.type == GraphicsExpose) {
            continue;
        }

        screen.is_dirty = true;

        switch (event.type) {
            case Expose:
                draw__Darkloard();

                break;
            case ClientMessage:
                if (event.xclient.data.l[0] == atom_wm_delete) {
                    running = false;
                }

                break;
            case KeyPress:
                handle_keypress__Darkloard(&event.xkey);

                break;
            case ButtonPress:
                handle_button_press__Darkloard(&event.xbutton);

                break;
            case ButtonRelease:
                handle_button_release__Darkloard(&event.xbutton);

                break;
            case MotionNotify:
                handle_motion__Darkloard(&event.xmotion);

                // Only a selection being dragged changes what is drawn.
                screen.is_dirty = was_dirty || selection.selecting;

                break;
            case SelectionRequest:
                handle_selection_request__Darkloard(&event.xselectionrequest);

                break;
            case SelectionNotify:
                handle_selection_notify__Darkloard(&event.xselection);

                break;
            case PropertyNotify:
                handle_property_notify__Darkloard(&event.xproperty);

                break;
            case FocusIn:
                window_focused = true;

                reset_cursor_blink__Darkloard();

                if (focus_events) {
                    write_pty__Darkloard("\033[I", 3);
                }

                break;
            case FocusOut:
                // Stop blinking (cursor stays visible) while unfocused.
                window_focused = false;

                if (focus_events) {
                    write_pty__Darkloard("\033[O", 3);
                }

                break;
            case SelectionClear:
                if (event.xselectionclear.selection == atom_clipboard) {
                    free(clipboard_text);

                    clipboard_text = NULL;
                    clipboard_text_len = 0;
                } else {
                    deinit__DarkloardSelection();
                }

                break;
            case ConfigureNotify: {
                XConfigureEvent configure_event = event.xconfigure;

                handle_window_resize__Darkloard(
                  (unsigned short)configure_event.width,
                  (unsigned short)configure_event.height);

                break;
            }
            default:
                break;
        }
    }
}

bool
handle_pty_events__Darkloard(void)
{
    size_t total = 0;

    while (is_terminal_alive__Darkloard()) {
        // In case of command such as `yes`
        if (XPending(display)) {
            handle_x_events__Darkloard();
        }

        char *read_buffer;
        size_t read_buffer_len;

        if (read_pty__Darkloard(&read_buffer, &read_buffer_len)) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return true;
            }

            LOG_ERROR("failed to read on master fd (%s)", strerror(errno));

            return true;
        }

        total += read_buffer_len;

        struct DarkloardMessage message =
          init__DarkloardMessage(read_buffer, read_buffer_len);

        parse__Darkloard(&message);
        free(read_buffer);

        if (total >= DARKLOARD_MAX_READ_BYTES) {
            break;
        }
    }

    return is_terminal_alive__Darkloard();
}

// Character class for word selection: 0 = space, 1 = delimiter, 2 = word.
static int
char_class__Darkloard(uint32_t cp)
{
    if (cp == ' ' || cp == '\t' || cp == 0x00A0) {
        return 0;
    }

    if (cp < 0x80 && cp != 0 && strchr(DARKLOARD_WORD_DELIMITERS, (int)cp)) {
        return 1;
    }

    return 2;
}

// Selects the word (or run of spaces, or single delimiter) at line/col and
// makes it the PRIMARY selection. Returns false if there is nothing there.
bool
select_word__Darkloard(int64_t line, uint32_t col)
{
    uint32_t cols = 0;
    struct DarkloardCell *cells = get_line_cells__Darkloard(line, &cols);

    if (!cells || col >= cols) {
        return false;
    }

    // The right half of a wide character belongs to the character before.
    while (col > 0 && cells[col].codepoint == 0) {
        col--;
    }

    int cls = char_class__Darkloard(cells[col].codepoint);
    uint32_t start = col, end = col;

    if (cls != 1) {
        while (start > 0 &&
               (cells[start - 1].codepoint == 0 ||
                char_class__Darkloard(cells[start - 1].codepoint) == cls)) {
            start--;
        }

        while (end + 1 < cols &&
               (cells[end + 1].codepoint == 0 ||
                char_class__Darkloard(cells[end + 1].codepoint) == cls)) {
            end++;
        }
    } else if (end + 1 < cols && cells[end + 1].codepoint == 0) {
        end++;
    }

    deinit__DarkloardSelection();

    selection.start_row = line;
    selection.start_col = start;
    selection.end_row = line;
    selection.end_col = end;
    selection.active = true;
    screen.is_dirty = true;

    build_selection_text__DarkloardSelection();

    // Only spaces: highlighted, but nothing to copy.
    if (selection.text_len > 0) {
        XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    }

    return true;
}

// Lines to scroll per step while dragging a selection: negative = towards
// the history (pointer above the text), positive = towards the bottom,
// 0 = pointer inside. Faster the further the pointer is.
int
autoscroll_lines__Darkloard(void)
{
    if (!selection.selecting || !fonts[FONT_REGULAR]) {
        return 0;
    }

    int cell_h = fonts[FONT_REGULAR]->ascent + fonts[FONT_REGULAR]->descent;
    int top = DARKLOARD_MARGIN_TOP;
    int bottom = DARKLOARD_MARGIN_TOP + (int)screen.rows * cell_h;

    if (drag_y < top) {
        int n = 1 + (top - drag_y) / cell_h;

        return -(n > 10 ? 10 : n);
    }

    if (drag_y >= bottom) {
        int n = 1 + (drag_y - bottom) / cell_h;

        return n > 10 ? 10 : n;
    }

    return 0;
}

void
autoscroll__Darkloard(void)
{
    int lines = autoscroll_lines__Darkloard();

    if (lines == 0) {
        return;
    }

    int64_t now = now_ms__Darkloard();

    if (now - autoscroll_last_ms < DARKLOARD_AUTOSCROLL_MS) {
        return;
    }

    autoscroll_last_ms = now;

    int old_offset = scroll_offset;

    scroll_offset -= lines;

    if (scroll_offset > history_count) {
        scroll_offset = history_count;
    }

    if (scroll_offset < 0) {
        scroll_offset = 0;
    }

    if (scroll_offset != old_offset) {
        pixel_to_selection__Darkloard(
          drag_x, drag_y, &selection.end_row, &selection.end_col);

        screen.is_dirty = true;
    }
}

int64_t
now_ms__Darkloard(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Removes the selection highlight when the user sends input. The selected
// text is kept, so it can still be pasted with the middle button.
void
clear_selection_highlight__Darkloard(void)
{
    if (selection.active && !selection.selecting) {
        selection.active = false;
        screen.is_dirty = true;
    }
}

// Show the cursor and restart the blink period (on input / output, so the
// cursor doesn't disappear while typing).
void
reset_cursor_blink__Darkloard(void)
{
    if (!cursor_blink_on) {
        screen.is_dirty = true;
    }

    cursor_blink_on = true;
    cursor_blink_last_ms = now_ms__Darkloard();
}

void
poll__Darkloard(void)
{
#define FDS_LEN 2
    struct pollfd fds[FDS_LEN] = { { .fd = display_fd, .events = POLLIN },
                                   { .fd = pty_master_fd, .events = POLLIN } };

    while (is_terminal_alive__Darkloard() && running) {
        fds[1].events = POLLIN | (pty_out.len > 0 ? POLLOUT : 0);

        // Only wake up for blinking when a blinking cursor is shown.
        int timeout = -1;
        bool blinking = cursor_blink && window_focused &&
                        screen.cursor.visible && scroll_offset == 0;

        if (blinking) {
            int64_t elapsed = now_ms__Darkloard() - cursor_blink_last_ms;

            if (elapsed >= DARKLOARD_CURSOR_BLINK_MS) {
                cursor_blink_on = !cursor_blink_on;
                cursor_blink_last_ms = now_ms__Darkloard();
                screen.is_dirty = true;

                draw__Darkloard();

                elapsed = 0;
            }

            timeout = (int)(DARKLOARD_CURSOR_BLINK_MS - elapsed);
        }

        // Keep scrolling while a selection is dragged outside the text, even
        // if the pointer doesn't move.
        if (autoscroll_lines__Darkloard() != 0) {
            autoscroll__Darkloard();
            draw__Darkloard();

            if (timeout < 0 || timeout > DARKLOARD_AUTOSCROLL_MS) {
                timeout = DARKLOARD_AUTOSCROLL_MS;
            }
        }

        // Xlib may already have read events into its queue (e.g. during
        // drawing): the socket then has nothing new and poll() would sleep
        // with events pending. Don't wait in that case.
        if (XPending(display) > 0) {
            timeout = 0;
        }

        int ret = poll(fds, FDS_LEN, timeout);

        // Handles queued events too, not only when the socket is readable.
        handle_x_events__Darkloard();

        if (ret > 0) {
            if (fds[1].revents & POLLOUT) {
                flush_pty__Darkloard();
            }

            if (fds[1].revents & POLLIN) {
                if (!handle_pty_events__Darkloard()) {
                    break;
                }
            }
        }

        draw__Darkloard();
    }
#undef FDS_LEN
}

static int
load_font__Darkloard(void)
{
    static const char *suffixes[FONT_COUNT] = {
        "", ":bold", ":italic", ":bold:italic"
    };
    char *font_name = NULL;

    for (int v = 0; v < FONT_COUNT; v++) {
        XASPRINTF(&font_name,
                  "%s:size=%d%s",
                  DARKLOARD_FONT_NAME,
                  DARKLOARD_FONT_SIZE,
                  suffixes[v]);

        fonts[v] = XftFontOpenName(display, screen_num, font_name);

        free(font_name);
    }

    return fonts[FONT_REGULAR] ? 0 : 1;
}

void
reload_font__Darkloard(unsigned int new_size)
{
    for (int i = 0; i < font_cache_len; i++) {
        XftFontClose(display, font_cache[i]);

        font_cache[i] = NULL;
    }

    font_cache_len = 0;

    for (int v = 0; v < FONT_COUNT; v++) {
        if (fonts[v]) {
            XftFontClose(display, fonts[v]);

            fonts[v] = NULL;
        }
    }

    static const char *suffixes[FONT_COUNT] = {
        "", ":bold", ":italic", ":bold:italic"
    };
    char *font_name = NULL;

    xasprintf__Darkloard(
      &font_name, "%s:size=%u", DARKLOARD_FONT_NAME, new_size);

    fonts[FONT_REGULAR] = XftFontOpenName(display, screen_num, font_name);

    free(font_name);

    if (!fonts[FONT_REGULAR]) {
        xasprintf__Darkloard(
          &font_name, "%s:size=%u", DARKLOARD_FONT_NAME, current_font_size);

        fonts[FONT_REGULAR] = XftFontOpenName(display, screen_num, font_name);

        free(font_name);

        return;
    }

    for (int v = FONT_BOLD; v < FONT_COUNT; v++) {
        xasprintf__Darkloard(&font_name,
                             "%s:size=%u%s",
                             DARKLOARD_FONT_NAME,
                             new_size,
                             suffixes[v]);

        fonts[v] = XftFontOpenName(display, screen_num, font_name);

        free(font_name);
    }

    current_font_size = new_size;

    handle_window_resize__Darkloard((unsigned short)window_width,
                                    (unsigned short)window_height);
}

void
close__Darkloard(void)
{
    for (int i = 0; i < font_cache_len; i++) {
        XftFontClose(display, font_cache[i]);
    }

    font_cache_len = 0;

    for (int v = 0; v < FONT_COUNT; v++) {
        if (fonts[v]) {
            XftFontClose(display, fonts[v]);
            fonts[v] = NULL;
        }
    }

    if (back_buffer) {
        XFreePixmap(display, back_buffer);
    }

    XFreeGC(display, window_gc);
    XCloseDisplay(display);
    close(pty_master_fd);

    if (is_terminal_alive__Darkloard()) {
        kill(terminal_process.shell_pid, SIGTERM);
    }

    deinit__DarkloardMessageList(&message_list);
    deinit__DarkloardScreen();
    deinit__DarkloardSelection();
    free(clipboard_text);
    free(pty_out.data);
    free(paste_in.data);
    free(parser.osc_buf);
}

int
main()
{
    setlocale(LC_ALL, "");

    if (!(display = XOpenDisplay(NULL))) {
        LOG_ERROR("failed to open display\n");

        return 1;
    }

    display_fd = ConnectionNumber(display);
    screen_num = DefaultScreen(display);
    colormap = DefaultColormap(display, screen_num);
    visual = DefaultVisual(display, screen_num);

    Window window_root = XDefaultRootWindow(display);

    if (load_font__Darkloard()) {
        LOG_ERROR("failed to load font");

        return 1;
    }

    // Default width and height
    window_width = 900;
    window_height = 500;
    window = XCreateSimpleWindow(display,
                                 window_root,
                                 0,
                                 0,
                                 window_width,
                                 window_height,
                                 0,
                                 RGB(0, 0, 0),
                                 RGB(0, 0, 0));
    window_gc = XCreateGC(display, window, 0, NULL);
    // Otherwise every XCopyArea of the back buffer sends a NoExpose event,
    // which would trigger another redraw, endlessly.
    XSetGraphicsExposures(display, window_gc, False);

    XStoreName(display, window, "Darkloard");
    update_event_mask__Darkloard();
    XMapWindow(display, window);

    init_atoms__Darkloard();
    init_history__Darkloard();
    open_pty__Darkloard();
    handle_window_resize__Darkloard((unsigned short)window_width,
                                    (unsigned short)window_height);
    handle_x_events__Darkloard();
    poll__Darkloard();
    close__Darkloard();
}
