#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>
#include <stdarg.h>

#include <linux/limits.h>

#include "config.h"

#define RGB(r, g, b) ((r) << 16 | (g) << 8 | (b))
#define LOG_ERROR(fmt, ...) fprintf(stderr, "Error: "fmt"\n", ##__VA_ARGS__);
#define XMALLOC(size) xmalloc__Darkloard(size)
#define XASPRINTF(buffer, fmt, ...) xasprintf__Darkloard(buffer, fmt, ##__VA_ARGS__);

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

// Every config value that can change during the terminal running.
struct DarkloardConfig {
	unsigned int font_size;
};

struct DarkloardTerminal {
	uint32_t row;
	uint32_t col;
};

struct DarkloardTerminalProcess {
	char *shell;
	pid_t shell_pid;
};

struct DarkloardMessage {
	char *buffer;
	size_t buffer_len;
	struct DarkloardMessage *next;
};

struct DarkloardMessageList {
	struct DarkloardMessage *first;
	struct DarkloardMessage *last;
};

struct DarkloardParseIterator {
	char *buffer;
	size_t buffer_len;
	char *current;
};

static Display *display = NULL;
static int display_fd = -1;
static int window_width = 0;
static int window_height = 0;
static Window window = 0;
static GC window_gc = {0};
static int screen = 0;
static Colormap colormap = {0};
static Visual *visual = NULL;
static int pty_master_fd = -1; 
static struct DarkloardTerminal terminal = {0};
static struct DarkloardTerminalProcess terminal_process = {0};
static struct DarkloardMessageList message_list = {0};
static struct DarkloardConfig config = {0};
static XftFont *font = NULL;

static inline struct DarkloardMessage init__DarkloardMessage(char *buffer, size_t buffer_len);

static inline void deinit__DarkloardMessage(struct DarkloardMessage *self);

static void deinit__DarkloardMessageList(struct DarkloardMessageList *self);

static inline struct DarkloardParseIterator init__DarkloardParseIterator(char *buffer, size_t buffer_len);

static inline bool has_reach_end__DarkloardParseIterator(const struct DarkloardParseIterator *self);

static unsigned char current__DarkloardParseIterator(const struct DarkloardParseIterator *self);

static unsigned char next__DarkloardParseIterator(struct DarkloardParseIterator *self);

static void *xmalloc__Darkloard(size_t size);

static void xasprintf__Darkloard(char **buffer, const char *fmt, ...);

static int configure_terminal__Darkloard(int slave_fd);

static bool is_terminal_alive__Darkloard(void);

static int open_terminal__Darkloard(int slave_fd, char *slave_filename);

static int open_pty__Darkloard(void);

static int resize_pty__Darkloard(unsigned short row, unsigned short col, unsigned short xpixel, unsigned short ypixel);

static int read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read);

static int write_pty__Darkloard(char *buffer, size_t buffer_len);

static void parse__Darkloard(struct DarkloardMessage *message);

static void draw__Darkloard(void);

static void handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel);

static void handle_x_events__Darkloard(void);

static bool handle_pty_events__Darkloard(void);

static void poll__Darkloard(void);

static int load_font__Darkloard(void);

static void close__Darkloard(void);

struct DarkloardMessage init__DarkloardMessage(char *buffer, size_t buffer_len)
{
	return (struct DarkloardMessage){
		.buffer = buffer,
		.buffer_len = buffer_len,
		.next = NULL
	};
}

void deinit__DarkloardMessage(struct DarkloardMessage *self)
{
	free(self->buffer);
	free(self);
}

void deinit__DarkloardMessageList(struct DarkloardMessageList *self)
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

struct DarkloardParseIterator init__DarkloardParseIterator(char *buffer, size_t buffer_len)
{
	assert(buffer && "Buffer should non-null");
	assert(buffer_len > 0 && "Buffer should be greater than 0");

	return (struct DarkloardParseIterator){
		.buffer = buffer,
		.buffer_len = buffer_len,
		.current = buffer
	};
}

bool has_reach_end__DarkloardParseIterator(const struct DarkloardParseIterator *self)
{
	return self->current - self->buffer < self->buffer_len;
}

unsigned char current__DarkloardParseIterator(const struct DarkloardParseIterator *self)
{
	return *self->current;
}

unsigned char next__DarkloardParseIterator(struct DarkloardParseIterator *self)
{
	if (self->current - self->buffer < self->buffer_len) {
		++self->current;
	}

	return *self->current;
}

void *xmalloc__Darkloard(size_t size)
{
	void *ptr = malloc(size);

	if (!ptr) {
		LOG_ERROR("out of memory");
		exit(1);
	}

	return ptr;
}

void xasprintf__Darkloard(char **buffer, const char *fmt, ...)
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

int configure_terminal__Darkloard(int slave_fd)
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

bool is_terminal_alive__Darkloard(void)
{
	if (terminal_process.shell_pid == 0 || waitpid(terminal_process.shell_pid, NULL, WNOHANG) != 0) {
		return false;
	}

	return true;
}

int open_terminal__Darkloard(int slave_fd, char *slave_filename)
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

int open_pty__Darkloard(void)
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

int resize_pty__Darkloard(unsigned short row, unsigned short col, unsigned short xpixel, unsigned short ypixel)
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

int read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read)
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

int write_pty__Darkloard(char *buffer, size_t buffer_len)
{
	ssize_t res = write(pty_master_fd, buffer, buffer_len);

	if (res == -1) {
		return 1;
	}

	return 0;
}

void parse__Darkloard(struct DarkloardMessage *message)
{
	struct DarkloardParseIterator iterator = init__DarkloardParseIterator(message->buffer, message->buffer_len);

	while (has_reach_end__DarkloardParseIterator(&iterator)) {
		switch (current__DarkloardParseIterator(&iterator)) {
			case DARKLOARD_BEL:
				// We ignore the bep because it's anoying.
				break;
			case DARKLOARD_BS:
				break;
			case DARKLOARD_HT:
				break;
			case DARKLOARD_LF:
				break;
			case DARKLOARD_VT:
				break;
			case DARKLOARD_FF:
				break;
			case DARKLOARD_CR:
				break;
			case DARKLOARD_SO:
				break;
			case DARKLOARD_SI:
				break;
			case DARKLOARD_CAN:
				break;
			case DARKLOARD_SUB:
				break;
			case DARKLOARD_ESC:
				break;
			case DARKLOARD_DEL:
				break;
			case DARKLOARD_CSI:
				break;
			case DARKLOARD_NUL:
				break;
			case DARKLOARD_ENQ:
				break;
			case DARKLOARD_DC1:
				break;
			case DARKLOARD_DC3:
				break;
			default:
				break;
		}
	}
}

void draw__Darkloard(void)
{
	XClearWindow(display, window);
	XSetForeground(display, window_gc, RGB(255, 255, 255));

	XftDraw *draw = XftDrawCreate(display, window, visual, colormap);

XftColor color;
XRenderColor xrc = { .red = 0, .green = 0, .blue = 0xffff, .alpha = 0xffff };
XftColorAllocValue(display, visual, colormap, &xrc, &color);

XftDrawStringUtf8(draw, &color, font, 200, 200,
                  (FcChar8 *)"Hello, World!", 13);
}


void handle_window_resize__Darkloard(unsigned short xpixel, unsigned short ypixel)
{
	unsigned short row = (xpixel - DARKLOARD_MARGIN_LEFT - DARKLOARD_MARGIN_RIGHT) / font->max_advance_width;
	unsigned short column = (ypixel - DARKLOARD_MARGIN_TOP - DARKLOARD_MARGIN_BOTTOM) / (font->ascent + font->descent); 

	resize_pty__Darkloard(row, column, xpixel, ypixel);
}

void handle_x_events__Darkloard(void)
{
	while (XPending(display)) {
		XEvent event;

		XNextEvent(display, &event);

		switch (event.type) {
			case Expose:
				break;
			case ConfigureNotify: {
				XConfigureEvent configure_event = event.xconfigure;

				handle_window_resize__Darkloard(configure_event.width, configure_event.height);
			}
			default:
				break;
		}
	}
}

bool handle_pty_events__Darkloard(void)
{
	bool is_terminal_alive = false;

	while ((is_terminal_alive = is_terminal_alive__Darkloard())) {
		char *read_buffer;
		size_t read_buffer_len;

		if (read_pty__Darkloard(&read_buffer, &read_buffer_len)) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return true;
			}

			LOG_ERROR("failed to read on master fd (%s)", strerror(errno));

			return true;
		}
	}

	return false;
}

void poll__Darkloard(void)
{
#define FDS_LEN 2
	struct pollfd fds[FDS_LEN] = {
		{ .fd = display_fd, .events = POLLIN },
		{ .fd = pty_master_fd, .events = POLLIN }
	};

	while (true) {
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

static int load_font__Darkloard(void)
{	
	char *font_name = NULL;

	XASPRINTF(&font_name, "%s:size=%d", DARKLOARD_FONT_NAME, DARKLOARD_FONT_SIZE);

	font = XftFontOpenName(display, screen, font_name);

	free(font_name);

	if (!font) {
		return 1;
	}

	return 0;
}

void close__Darkloard(void)
{
	XftFontClose(display, font);
	XFreeGC(display, window_gc);
	XCloseDisplay(display);
	close(pty_master_fd);

	if (is_terminal_alive__Darkloard()) {
		kill(terminal_process.shell_pid, SIGTERM);
	}

	deinit__DarkloardMessageList(&message_list);
}

int main() {
	if (!(display = XOpenDisplay(NULL))) {
		LOG_ERROR("failed to open display\n");

		return 1;
	}

	display_fd = ConnectionNumber(display);

	screen = DefaultScreen(display);
	colormap = DefaultColormap(display, screen);
	visual = DefaultVisual(display, screen);

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
	window = XCreateSimpleWindow(display, window_root, 0, 0, window_width, window_height, 0, RGB(0, 0, 0), RGB(255, 255, 255));
	window_gc = XCreateGC(display, window, 0, NULL);

	XStoreName(display, window, "Darkloard");
	XSelectInput(display, window, ExposureMask | KeyPress | StructureNotifyMask);
	XMapWindow(display, window);

	open_pty__Darkloard();
	handle_window_resize__Darkloard(window_width, window_height);
	handle_x_events__Darkloard();
	poll__Darkloard();
	close__Darkloard();
}
