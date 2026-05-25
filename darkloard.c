#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

// Every config value that can change during the terminal running.
struct DarkloardConfig
{
    unsigned int font_size;
};

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
};

enum DarkloardParseState
{
    DARKLOARD_PARSE_STATE_NORMAL = 0,
    DARKLOARD_PARSE_STATE_ESC,
    DARKLOARD_PARSE_STATE_CSI,
    DARKLOARD_PARSE_STATE_OSC,
    DARKLOARD_PARSE_STATE_CHARSET,
};

struct DarkloardCsiParams
{
    int params[DARKLOARD_CSI_MAX_PARAMS];
    int params_len;
    bool private_mode;
    char final_byte;
};

struct DarkloardParser
{
    enum DarkloardParseState state;
    struct DarkloardCsiParams csi;
    char osc_buf[DARKLOARD_OSC_BUF_SIZE];
    int osc_len;
    bool osc_esc_pending;
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
static struct DarkloardConfig config = { 0 };
static XftFont *font = NULL;
static struct DarkloardScreen screen = { 0 };
static struct DarkloardParser parser = { 0 };

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
feed__DarkloardParser(unsigned char c);

static void
parse__Darkloard(struct DarkloardMessage *message);

static void
draw__Darkloard(void);

static void
handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel);

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
        case 0:
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
            execv(shell, shell_argv);

        child_end:
            exit(1);
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
}

void
deinit__DarkloardScreen(void)
{
    if (!screen.cells) {
        return;
    }

    for (uint32_t i = 0; i < screen.rows; i++) {
        free(screen.cells[i]);
    }

    free(screen.cells);
    screen.cells = NULL;
}

void
resize__DarkloardScreen(uint32_t rows, uint32_t cols)
{
    deinit__DarkloardScreen();
    init__DarkloardScreen(rows, cols);
}

void
scroll_up__DarkloardScreen(uint32_t top, uint32_t bottom, uint32_t n)
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
        struct DarkloardCell *evicted = screen.cells[top];

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

    screen.cells[screen.cursor.row][screen.cursor.col] =
      (struct DarkloardCell){ .codepoint = codepoint,
                              .fg = screen.current_fg,
                              .bg = screen.current_bg,
                              .attrs = screen.current_attrs };

    screen.cursor.col++;

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
            parser.state = DARKLOARD_PARSE_STATE_NORMAL;
            break;
        case DARKLOARD_ESC:
            parser.state = DARKLOARD_PARSE_STATE_ESC;
            break;
        case DARKLOARD_DEL:
            break;
        case DARKLOARD_CSI:
            memset(&parser.csi, 0, sizeof(parser.csi));
            parser.state = DARKLOARD_PARSE_STATE_CSI;
            break;
        default:
            if (c >= 0x20) {
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

    if (c >= 0x20 && c <= 0x2F) {
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
                    case 25:
                        screen.cursor.visible = set;
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
        DARKLOARD_COLOR_0,  DARKLOARD_COLOR_1,  DARKLOARD_COLOR_2,  DARKLOARD_COLOR_3,
        DARKLOARD_COLOR_4,  DARKLOARD_COLOR_5,  DARKLOARD_COLOR_6,  DARKLOARD_COLOR_7,
        DARKLOARD_COLOR_8,  DARKLOARD_COLOR_9,  DARKLOARD_COLOR_10, DARKLOARD_COLOR_11,
        DARKLOARD_COLOR_12, DARKLOARD_COLOR_13, DARKLOARD_COLOR_14, DARKLOARD_COLOR_15,
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
                    int idx = parser.csi.params[i + 2];
                    if (idx >= 0 && idx < 16) {
                        screen.current_fg = ansi_colors[idx];
                    }
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
                    int idx = parser.csi.params[i + 2];
                    if (idx >= 0 && idx < 16) {
                        screen.current_bg = ansi_colors[idx];
                    }
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
            case 1:
            case 2:
                // Window title set — handled by the X11 layer in the future.
                break;
            default:
                break;
        }
    }
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
    }
}

void
parse__Darkloard(struct DarkloardMessage *message)
{
    for (size_t i = 0; i < message->buffer_len; i++) {
        feed__DarkloardParser((unsigned char)message->buffer[i]);
    }
}

void
draw__Darkloard(void)
{
    if (!screen.cells) {
        return;
    }

    XClearWindow(display, window);

    XftDraw *draw = XftDrawCreate(display, window, visual, colormap);
    int cell_w = font->max_advance_width;
    int cell_h = font->ascent + font->descent;

    for (uint32_t row = 0; row < screen.rows; row++) {
        for (uint32_t col = 0; col < screen.cols; col++) {
            struct DarkloardCell *cell = &screen.cells[row][col];

            uint32_t fg = cell->fg;
            uint32_t bg = cell->bg;

            if (cell->attrs & DARKLOARD_ATTR_REVERSE) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }

            int x = DARKLOARD_MARGIN_LEFT + (int)col * cell_w;
            int y = DARKLOARD_MARGIN_TOP + (int)row * cell_h;

            if (bg != DARKLOARD_DEFAULT_BG) {
                XSetForeground(display, window_gc, bg);
                XFillRectangle(display,
                               window,
                               window_gc,
                               x,
                               y,
                               (unsigned int)cell_w,
                               (unsigned int)cell_h);
            }

            if (cell->codepoint != 0 && cell->codepoint != ' ') {
                XftColor xft_fg;
                XRenderColor xrc = {
                    .red = (unsigned short)(((fg >> 16) & 0xFF) * 257),
                    .green = (unsigned short)(((fg >> 8) & 0xFF) * 257),
                    .blue = (unsigned short)(((fg) & 0xFF) * 257),
                    .alpha = 0xFFFF
                };
                XftColorAllocValue(display, visual, colormap, &xrc, &xft_fg);

                char utf8[5] = { 0 };
                int utf8_len =
                  codepoint_to_utf8__Darkloard(cell->codepoint, utf8);
                XftDrawStringUtf8(draw,
                                  &xft_fg,
                                  font,
                                  x,
                                  y + font->ascent,
                                  (FcChar8 *)utf8,
                                  utf8_len);

                XftColorFree(display, visual, colormap, &xft_fg);
            }
        }
    }

    if (screen.cursor.visible) {
        int cx = DARKLOARD_MARGIN_LEFT + (int)screen.cursor.col * cell_w;
        int cy = DARKLOARD_MARGIN_TOP + (int)screen.cursor.row * cell_h;
        XSetForeground(display, window_gc, DARKLOARD_CURSOR_COLOR);
        XDrawRectangle(display,
                       window,
                       window_gc,
                       cx,
                       cy,
                       (unsigned int)cell_w - 1,
                       (unsigned int)cell_h - 1);
    }

    XftDrawDestroy(draw);
    XFlush(display);
}

void
handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel)
{
    unsigned short num_cols = (unsigned short)((xpixel - DARKLOARD_MARGIN_LEFT -
                                                DARKLOARD_MARGIN_RIGHT) /
                                               font->max_advance_width);
    unsigned short num_rows = (unsigned short)((ypixel - DARKLOARD_MARGIN_TOP -
                                                DARKLOARD_MARGIN_BOTTOM) /
                                               (font->ascent + font->descent));

    if (num_rows == 0 || num_cols == 0) {
        return;
    }

    resize_pty__Darkloard(num_rows, num_cols, xpixel, ypixel);
    resize__DarkloardScreen(num_rows, num_cols);
}

void
handle_keypress__Darkloard(XKeyEvent *event)
{
    char buf[32];
    KeySym keysym = NoSymbol;
    int len = XLookupString(event, buf, (int)sizeof(buf) - 1, &keysym, NULL);

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
            write_pty__Darkloard("\033[A", 3);
            break;
        case XK_Down:
            write_pty__Darkloard("\033[B", 3);
            break;
        case XK_Right:
            write_pty__Darkloard("\033[C", 3);
            break;
        case XK_Left:
            write_pty__Darkloard("\033[D", 3);
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

        switch (event.type) {
            case Expose:
                break;
            case KeyPress:
                handle_keypress__Darkloard(&event.xkey);
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
    while (is_terminal_alive__Darkloard()) {
        char *read_buffer;
        size_t read_buffer_len;

        if (read_pty__Darkloard(&read_buffer, &read_buffer_len)) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return true;
            }

            LOG_ERROR("failed to read on master fd (%s)", strerror(errno));

            return true;
        }

        struct DarkloardMessage message =
          init__DarkloardMessage(read_buffer, read_buffer_len);
        parse__Darkloard(&message);
        free(read_buffer);
    }

    return false;
}

void
poll__Darkloard(void)
{
#define FDS_LEN 2
    struct pollfd fds[FDS_LEN] = { { .fd = display_fd, .events = POLLIN },
                                   { .fd = pty_master_fd, .events = POLLIN } };

    while (is_terminal_alive__Darkloard()) {
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
    char *font_name = NULL;

    XASPRINTF(
      &font_name, "%s:size=%d", DARKLOARD_FONT_NAME, DARKLOARD_FONT_SIZE);

    font = XftFontOpenName(display, screen_num, font_name);

    free(font_name);

    if (!font) {
        return 1;
    }

    return 0;
}

void
close__Darkloard(void)
{
    XftFontClose(display, font);
    XFreeGC(display, window_gc);
    XCloseDisplay(display);
    close(pty_master_fd);

    if (is_terminal_alive__Darkloard()) {
        kill(terminal_process.shell_pid, SIGTERM);
    }

    deinit__DarkloardMessageList(&message_list);
    deinit__DarkloardScreen();
}

int
main()
{
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
    XSelectInput(
      display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    open_pty__Darkloard();
    handle_window_resize__Darkloard((unsigned short)window_width,
                                    (unsigned short)window_height);
    handle_x_events__Darkloard();
    poll__Darkloard();
    close__Darkloard();
}
