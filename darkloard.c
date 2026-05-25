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
#include <pty.h>
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

#include <linux/limits.h>

#include "config.h"

#define LOG_ERROR(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__);
#define XMALLOC(size) xmalloc__Darkloard(size)
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
#define DARKLOARD_OSC_BUF_SIZE 1024
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
    int params_len;
    bool private_mode;
    bool secondary;
    char intermediate;
    char final_byte;
};

struct DarkloardParser
{
    enum DarkloardParseState state;
    struct DarkloardCsiParams csi;
    char osc_buf[DARKLOARD_OSC_BUF_SIZE];
    int osc_len;
    bool osc_esc_pending;
    bool str_esc_pending;
    uint32_t utf8_codepoint;
    int utf8_remaining;
};

struct DarkloardSelection
{
    bool active;
    bool selecting;
    uint32_t start_row;
    uint32_t start_col;
    uint32_t end_row;
    uint32_t end_col;
    char *text;
    size_t text_len;
};

static Display *display = NULL;
static int display_fd = -1;
static int window_width = 0;
static int window_height = 0;
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
static struct DarkloardCell *history_lines[DARKLOARD_HISTORY_LINES] = { 0 };
static uint32_t history_line_cols[DARKLOARD_HISTORY_LINES] = { 0 };
static int history_head = 0;
static int history_count = 0;
static int scroll_offset = 0;
static struct DarkloardScreen screen = { 0 };
static struct DarkloardParser parser = { 0 };
static struct DarkloardSelection selection = { 0 };
static bool running = true;
static bool app_cursor_keys = false;
static bool bracketed_paste = false;
static bool focus_events = false;

static Atom atom_utf8_string = None;
static Atom atom_clipboard = None;
static Atom atom_targets = None;
static Atom atom_xsel_data = None;
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

static bool
is_selected__DarkloardSelection(uint32_t row, uint32_t col);

static void
build_selection_text__DarkloardSelection(void);

static void
request_paste__Darkloard(Atom sel_type);

static void
handle_selection_request__Darkloard(XSelectionRequestEvent *req);

static void
handle_selection_notify__Darkloard(XSelectionEvent *event);

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

    tios.c_iflag |= ICRNL;
    tios.c_oflag |= OPOST | ONLCR;
    tios.c_lflag |= ISIG | ICANON | ECHO | IEXTEN;
    tios.c_cflag |= CREAD | CS8;

    if (tcsetattr(slave_fd, TCSADRAIN, &tios) == -1) {
        return 1;
    }

    return 0;
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
                goto child_end;
            } else if (ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
                goto child_end;
            } else if (dup2(slave_fd, STDIN_FILENO) == -1) {
                goto child_end;
            } else if (dup2(slave_fd, STDOUT_FILENO) == -1) {
                goto child_end;
            } else if (dup2(slave_fd, STDERR_FILENO) == -1) {
                goto child_end;
            } else if (configure_terminal__Darkloard(slave_fd) == -1) {
                goto child_end;
            }

            close(slave_fd);

            struct passwd *pw = NULL;

            pw = getpwuid(getuid());

            if (!pw) {
                LOG_ERROR("failed to run `getpwuid`");

                goto child_end;
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
            } else if (setenv("COLORTERM", "truecolor", 1) == -1) {
                goto setenv_failed;
            }

            execv(shell, shell_argv);

        child_end:
            exit(1);
        setenv_failed:
            LOG_ERROR("failed to execute setenv");
            exit(1);
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
    ssize_t res = write(pty_master_fd, buffer, buffer_len);

    if (res == -1) {
        return 1;
    }

    return 0;
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

void
resize__DarkloardScreen(uint32_t rows, uint32_t cols)
{
    bool was_in_alt = in_alt_screen;
    scroll_offset = 0;

    if (!was_in_alt && screen.cells) {
        for (uint32_t i = 0; i < screen.rows; i++) {
            push_history_line__Darkloard(screen.cells[i], screen.cols);
        }
    }

    free_cells__DarkloardScreen();
    init__DarkloardScreen(rows, cols);
    if (was_in_alt) {
        struct DarkloardCell **tmp = screen.cells;
        screen.cells = inactive_cells;
        inactive_cells = tmp;
        in_alt_screen = true;
        screen.cursor.row = 0;
        screen.cursor.col = 0;
    }
}

void
scroll_up__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n)
{
    uint32_t region_height = bottom - top + 1;
    bool save = (top == 0 && !in_alt_screen);

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

    int width = wcwidth((wchar_t)codepoint);
    if (width < 0) {
        width = 1;
    }

    if (width == 2 && screen.cursor.col + 1 >= screen.cols) {
        screen.cells[screen.cursor.row][screen.cursor.col] =
          (struct DarkloardCell){ .codepoint = ' ',
                                  .fg = screen.current_fg,
                                  .bg = screen.current_bg,
                                  .attrs = screen.current_attrs };
        screen.cursor.col = 0;
        if (screen.cursor.row == screen.scroll_region.bottom) {
            scroll_up__DarkloardScreen(
              screen.scroll_region.top, screen.scroll_region.bottom, 1);
        } else if (screen.cursor.row < screen.rows - 1) {
            screen.cursor.row++;
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
        screen.cursor.col = 0;
        if (screen.cursor.row == screen.scroll_region.bottom) {
            scroll_up__DarkloardScreen(
              screen.scroll_region.top, screen.scroll_region.bottom, 1);
        } else if (screen.cursor.row < screen.rows - 1) {
            screen.cursor.row++;
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
            if (screen.cursor.row == screen.scroll_region.bottom) {
                scroll_up__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            } else if (screen.cursor.row < screen.rows - 1) {
                screen.cursor.row++;
            }
            break;
        case DARKLOARD_CR:
            screen.cursor.col = 0;
            break;
        case DARKLOARD_SO:
        case DARKLOARD_SI:
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
        case DARKLOARD_CSI:
            memset(&parser.csi, 0, sizeof(parser.csi));
            parser.state = DARKLOARD_PARSE_STATE_CSI;
            break;
        default:
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
            parser.osc_esc_pending = false;
            parser.state = DARKLOARD_PARSE_STATE_OSC;
            break;
        case '7':
            screen.saved_cursor = screen.cursor;
            screen.saved_cursor_valid = true;
            break;
        case '8':
            if (screen.saved_cursor_valid) {
                screen.cursor = screen.saved_cursor;
            }
            break;
        case 'c':
            screen.cursor.row = 0;
            screen.cursor.col = 0;
            screen.current_fg = DARKLOARD_DEFAULT_FG;
            screen.current_bg = DARKLOARD_DEFAULT_BG;
            screen.current_attrs = 0;
            screen.scroll_region.top = 0;
            screen.scroll_region.bottom = screen.rows - 1;
            erase_display__DarkloardScreen(2);
            break;
        case 'D':
            screen.cursor.row++;
            if (screen.cursor.row > screen.scroll_region.bottom) {
                screen.cursor.row = screen.scroll_region.bottom;
                scroll_up__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            }
            break;
        case 'E':
            screen.cursor.col = 0;
            screen.cursor.row++;
            if (screen.cursor.row > screen.scroll_region.bottom) {
                screen.cursor.row = screen.scroll_region.bottom;
                scroll_up__DarkloardScreen(
                  screen.scroll_region.top, screen.scroll_region.bottom, 1);
            }
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

        parser.csi.params[idx] = parser.csi.params[idx] * 10 + (c - '0');
        return;
    }

    if (c == ';') {
        if (parser.csi.params_len == 0) {
            parser.csi.params_len = 1;
        }

        if (parser.csi.params_len < DARKLOARD_CSI_MAX_PARAMS) {
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

    switch (parser.csi.final_byte) {
        case 'A': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row - n;
            screen.cursor.row = new_row < (int)screen.scroll_region.top
                                  ? screen.scroll_region.top
                                  : (uint32_t)new_row;
            break;
        }
        case 'B': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row + n;
            screen.cursor.row = new_row > (int)screen.scroll_region.bottom
                                  ? screen.scroll_region.bottom
                                  : (uint32_t)new_row;
            break;
        }
        case 'C': {
            int n = CSI_PARAM(0, 1);
            int new_col = (int)screen.cursor.col + n;
            screen.cursor.col =
              new_col >= (int)screen.cols ? screen.cols - 1 : (uint32_t)new_col;
            break;
        }
        case 'D': {
            int n = CSI_PARAM(0, 1);
            int new_col = (int)screen.cursor.col - n;
            screen.cursor.col = new_col < 0 ? 0 : (uint32_t)new_col;
            break;
        }
        case 'E': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row + n;
            screen.cursor.row = new_row > (int)screen.scroll_region.bottom
                                  ? screen.scroll_region.bottom
                                  : (uint32_t)new_row;
            screen.cursor.col = 0;
            break;
        }
        case 'F': {
            int n = CSI_PARAM(0, 1);
            int new_row = (int)screen.cursor.row - n;
            screen.cursor.row = new_row < (int)screen.scroll_region.top
                                  ? screen.scroll_region.top
                                  : (uint32_t)new_row;
            screen.cursor.col = 0;
            break;
        }
        case 'G': {
            int col = CSI_PARAM(0, 1) - 1;
            screen.cursor.col = (col < 0)                      ? 0
                                : (uint32_t)col >= screen.cols ? screen.cols - 1
                                                               : (uint32_t)col;
            break;
        }
        case 'H':
        case 'f': {
            int row = CSI_PARAM(0, 1) - 1;
            int col = CSI_PARAM(1, 1) - 1;
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
            erase_display__DarkloardScreen(mode);
            break;
        }
        case 'K': {
            int mode = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;
            erase_line__DarkloardScreen(mode);
            break;
        }
        case 'L': {
            int n = CSI_PARAM(0, 1);
            scroll_down__DarkloardScreen(
              screen.cursor.row, screen.scroll_region.bottom, (uint32_t)n);
            break;
        }
        case 'M': {
            int n = CSI_PARAM(0, 1);
            scroll_up__DarkloardScreen(
              screen.cursor.row, screen.scroll_region.bottom, (uint32_t)n);
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
        case 'c':
            if (!parser.csi.secondary) {
                write_pty__Darkloard("\033[?62;c", 7);
            } else {
                write_pty__Darkloard("\033[>1;10;0c", 10);
            }
            break;
        case 'd': {
            int row = CSI_PARAM(0, 1) - 1;
            screen.cursor.row = (row < 0)                      ? 0
                                : (uint32_t)row >= screen.rows ? screen.rows - 1
                                                               : (uint32_t)row;
            break;
        }
        case 'm':
            handle_sgr__DarkloardParser();
            break;
        case 'q':
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
            int top = CSI_PARAM(0, 1);
            int bottom = CSI_PARAM(1, (int)screen.rows);
            if (top < bottom && (uint32_t)bottom <= screen.rows) {
                screen.scroll_region.top = (uint32_t)(top - 1);
                screen.scroll_region.bottom = (uint32_t)(bottom - 1);
            }
            screen.cursor.row = 0;
            screen.cursor.col = 0;
            break;
        }
        case 'h':
        case 'l': {
            bool set = parser.csi.final_byte == 'h';
            if (parser.csi.private_mode) {
                int mode = parser.csi.params_len > 0 ? parser.csi.params[0] : 0;
                switch (mode) {
                    case 1:
                        app_cursor_keys = set;
                        break;
                    case 7:
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
                    case 1000:
                    case 1001:
                    case 1002:
                    case 1003:
                    case 1006:
                    case 1015:
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
            screen.saved_cursor = screen.cursor;
            screen.saved_cursor_valid = true;
            break;
        case 'u':
            if (screen.saved_cursor_valid) {
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
                    screen.current_fg = RGB(parser.csi.params[i + 2],
                                            parser.csi.params[i + 3],
                                            parser.csi.params[i + 4]);
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
                    screen.current_bg = RGB(parser.csi.params[i + 2],
                                            parser.csi.params[i + 3],
                                            parser.csi.params[i + 4]);
                    i += 4;
                }
                break;
            case 49:
                screen.current_bg = DARKLOARD_DEFAULT_BG;
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

    if (parser.osc_len < DARKLOARD_OSC_BUF_SIZE - 1) {
        parser.osc_buf[parser.osc_len++] = (char)c;
    }
}

void
handle_osc__DarkloardParser(void)
{
    if (parser.osc_len < 1) {
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
            default:
                break;
        }
    }
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
            parser.state = DARKLOARD_PARSE_STATE_NORMAL;
            break;
        case DARKLOARD_PARSE_STATE_STR_SKIP:
            handle_str_skip__DarkloardParser(c);
            break;
    }
}

void
parse__Darkloard(struct DarkloardMessage *message)
{
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

    if (restore_cursor) {
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
    }
}

static int
history_line_idx__Darkloard(int i)
{
    return ((history_head - history_count + i) % DARKLOARD_HISTORY_LINES +
            DARKLOARD_HISTORY_LINES) %
           DARKLOARD_HISTORY_LINES;
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
    history_head = 0;
    history_count = 0;
    scroll_offset = 0;
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
        bool from_history = false;

        if (scroll_offset > 0) {
            int abs = (int)history_count - scroll_offset + (int)row;
            if (abs >= 0 && abs < (int)history_count) {
                row_cells = get_history_line__Darkloard(abs);
                uint32_t hcols = get_history_line_cols__Darkloard(abs);
                row_cols = hcols < screen.cols ? hcols : screen.cols;
                from_history = true;
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

            if (!from_history && is_selected__DarkloardSelection(row, col)) {
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

    if (scroll_offset == 0 && screen.cursor.visible) {
        int cx = DARKLOARD_MARGIN_LEFT + (int)screen.cursor.col * cell_w;
        int cy = DARKLOARD_MARGIN_TOP + (int)screen.cursor.row * cell_h;
        XSetForeground(display, window_gc, DARKLOARD_CURSOR_COLOR);
        XFillRectangle(display,
                       back_buffer,
                       window_gc,
                       cx,
                       cy,
                       (unsigned int)cell_w,
                       (unsigned int)cell_h);

        if (screen.cursor.row < screen.rows &&
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

bool
is_selected__DarkloardSelection(uint32_t row, uint32_t col)
{
    if (!selection.active && !selection.selecting) {
        return false;
    }

    uint32_t r1 = selection.start_row, c1 = selection.start_col;
    uint32_t r2 = selection.end_row, c2 = selection.end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        uint32_t tr = r1;
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

    uint32_t r1 = selection.start_row, c1 = selection.start_col;
    uint32_t r2 = selection.end_row, c2 = selection.end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        uint32_t tr = r1;
        r1 = r2;
        r2 = tr;
        uint32_t tc = c1;
        c1 = c2;
        c2 = tc;
    }

    size_t max_size = (size_t)(r2 - r1 + 2) * ((size_t)screen.cols * 4 + 2);
    char *buf = XMALLOC(max_size);
    size_t len = 0;

    for (uint32_t row = r1; row <= r2; row++) {
        uint32_t col_start = (row == r1) ? c1 : 0;
        uint32_t col_end = (row == r2) ? c2 : screen.cols - 1;

        // Find last non-space in this row segment to strip trailing whitespace.
        uint32_t last = col_start;
        bool has_content = false;
        for (uint32_t c = col_end + 1; c-- > col_start;) {
            uint32_t cp = screen.cells[row][c].codepoint;
            if (cp != 0 && cp != ' ') {
                last = c;
                has_content = true;
                break;
            }
        }

        if (has_content) {
            for (uint32_t c = col_start; c <= last; c++) {
                uint32_t cp = screen.cells[row][c].codepoint;
                if (cp == 0) {
                    cp = ' ';
                }
                char utf8[5];
                int utf8_len = codepoint_to_utf8__Darkloard(cp, utf8);
                memcpy(buf + len, utf8, (size_t)utf8_len);
                len += (size_t)utf8_len;
            }
        }

        if (row < r2) {
            buf[len++] = '\n';
        }
    }

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

    if (!selection.text || selection.text_len == 0) {
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
                        (unsigned char *)selection.text,
                        (int)selection.text_len);
        notify.property = req->property;
    }

    XSendEvent(req->display, req->requestor, False, 0, (XEvent *)&notify);
}

void
handle_selection_notify__Darkloard(XSelectionEvent *event)
{
    if (event->property == None) {
        return;
    }

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    XGetWindowProperty(display,
                       window,
                       atom_xsel_data,
                       0,
                       1024 * 1024L,
                       True,
                       AnyPropertyType,
                       &actual_type,
                       &actual_format,
                       &nitems,
                       &bytes_after,
                       &data);

    if (data && nitems > 0) {
        if (bracketed_paste) {
            write_pty__Darkloard("\033[200~", 6);
        }
        write_pty__Darkloard((char *)data, nitems);
        if (bracketed_paste) {
            write_pty__Darkloard("\033[201~", 6);
        }
    }

    if (data) {
        XFree(data);
    }
}

void
handle_button_press__Darkloard(XButtonEvent *event)
{
    if (event->button == Button1) {
        deinit__DarkloardSelection();
        selection.selecting = true;
        pixel_to_cell__Darkloard(
          event->x, event->y, &selection.start_row, &selection.start_col);
        selection.end_row = selection.start_row;
        selection.end_col = selection.start_col;
    } else if (event->button == Button2) {
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
}

void
handle_button_release__Darkloard(XButtonEvent *event)
{
    if (event->button != Button1 || !selection.selecting) {
        return;
    }

    selection.selecting = false;

    pixel_to_cell__Darkloard(
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
    if (!selection.selecting) {
        return;
    }

    pixel_to_cell__Darkloard(
      event->x, event->y, &selection.end_row, &selection.end_col);
}

void
handle_keypress__Darkloard(XKeyEvent *event)
{
    char buf[32];
    KeySym keysym = NoSymbol;
    int len = XLookupString(event, buf, (int)sizeof(buf) - 1, &keysym, NULL);

    if (event->state & ShiftMask) {
        if (keysym == XK_Page_Up) {
            scroll_offset += (int)screen.rows / 2;
            if (scroll_offset > history_count) {
                scroll_offset = history_count;
            }
            return;
        }
        if (keysym == XK_Page_Down) {
            scroll_offset -= (int)screen.rows / 2;
            if (scroll_offset < 0) {
                scroll_offset = 0;
            }
            return;
        }
    }

    scroll_offset = 0;

    if ((event->state & ControlMask) && (event->state & ShiftMask)) {
        if (keysym == XK_c || keysym == XK_C) {
            if (selection.active && selection.text_len > 0) {
                XSetSelectionOwner(
                  display, atom_clipboard, window, CurrentTime);
            }
            return;
        }
        if (keysym == XK_v || keysym == XK_V) {
            request_paste__Darkloard(atom_clipboard);
            return;
        }
    }

    if (event->state & ControlMask) {
        if (keysym == XK_plus || keysym == XK_equal || keysym == XK_KP_Add) {
            if (current_font_size < 72) {
                reload_font__Darkloard(current_font_size + 1);
            }
            return;
        }
        if (keysym == XK_minus || keysym == XK_KP_Subtract) {
            if (current_font_size > 4) {
                reload_font__Darkloard(current_font_size - 1);
            }
            return;
        }
        if (keysym == XK_0 || keysym == XK_KP_0) {
            reload_font__Darkloard(DARKLOARD_FONT_SIZE);
            return;
        }
    }

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
                break;
            case SelectionRequest:
                handle_selection_request__Darkloard(&event.xselectionrequest);
                break;
            case SelectionNotify:
                handle_selection_notify__Darkloard(&event.xselection);
                break;
            case FocusIn:
                if (focus_events) {
                    write_pty__Darkloard("\033[I", 3);
                }
                break;
            case FocusOut:
                if (focus_events) {
                    write_pty__Darkloard("\033[O", 3);
                }
                break;
            case SelectionClear:
                deinit__DarkloardSelection();
                break;
            case ConfigureNotify: {
                XConfigureEvent configure_event = event.xconfigure;

                handle_window_resize__Darkloard(
                  (unsigned short)configure_event.width,
                  (unsigned short)configure_event.height);
                break;
            }
            default:
                screen.is_dirty = false;

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

void
poll__Darkloard(void)
{
#define FDS_LEN 2
    struct pollfd fds[FDS_LEN] = { { .fd = display_fd, .events = POLLIN },
                                   { .fd = pty_master_fd, .events = POLLIN } };

    while (is_terminal_alive__Darkloard() && running) {
        int ret = poll(fds, FDS_LEN, -1);

        if (ret > 0) {
            if (fds[0].revents & POLLIN) {
                handle_x_events__Darkloard();
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
    XWindowAttributes window_root_attr;

    if (XGetWindowAttributes(display, window_root, &window_root_attr) == 0) {
        LOG_ERROR("unable to get window attributes\n");
    }

    if (load_font__Darkloard()) {
        LOG_ERROR("failed to load font");

        return 1;
    }

    window_width = window_root_attr.width;
    window_height = window_root_attr.height;
    window = XCreateSimpleWindow(display,
                                 window_root,
                                 0,
                                 0,
                                 (unsigned int)window_width,
                                 (unsigned int)window_height,
                                 0,
                                 RGB(0, 0, 0),
                                 RGB(0, 0, 0));
    window_gc = XCreateGC(display, window, 0, NULL);

    XStoreName(display, window, "Darkloard");
    XSelectInput(display,
                 window,
                 ExposureMask | KeyPressMask | StructureNotifyMask |
                   ButtonPressMask | ButtonReleaseMask | Button1MotionMask |
                   FocusChangeMask);
    XMapWindow(display, window);

    init_atoms__Darkloard();
    open_pty__Darkloard();
    handle_window_resize__Darkloard((unsigned short)window_width,
                                    (unsigned short)window_height);
    handle_x_events__Darkloard();
    poll__Darkloard();
    close__Darkloard();
}
