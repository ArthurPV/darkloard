#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>

#include <linux/limits.h>

#define RGB(r, g, b) ((r) << 16 | (g) << 8 | (b))
#define LOG_ERROR(fmt, ...) fprintf(stderr, "Error: "fmt"\n", ##__VA_ARGS__);
#define XMALLOC(size) xmalloc__Darkloard(size)

struct DarkloardTerminal {
	char *shell;
	pid_t shell_pid;
};

static Display *display = NULL;
static int display_fd = -1;
static int window_width = 0;
static int window_height = 0;
static Window window = 0;
static GC window_gc = {0};
static int pty_master_fd = -1; 
static struct DarkloardTerminal terminal = {0};

static void *xmalloc__Darkloard(size_t size);

static int configure_terminal__Darkloard(int slave_fd);

static bool is_terminal_alive__Darkloard(void);

static int open_terminal__Darkloard(int slave_fd, char *slave_filename);

static int open_pty__Darkloard(void);

static int resize_pty__Darkloard(unsigned short row, unsigned short col, unsigned short xpixel, unsigned short ypixel);

static int read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read);

static int write_pty__Darkloard(char *buffer, size_t buffer_len);

static void handle_x_events__Darkloard(void);

static bool handle_pty_events__Darkloard(void);

static void poll__Darkloard(void);

static void close__Darkloard(void);

void *xmalloc__Darkloard(size_t size)
{
	void *ptr = malloc(size);

	if (!ptr) {
		LOG_ERROR("out of memory");
		exit(1);
	}

	return ptr;
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
	if (terminal.shell_pid == 0 || waitpid(terminal.shell_pid, NULL, WNOHANG) != 0) {
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
			terminal.shell_pid = pid;
			terminal.shell = shell;

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

	return 0;
}

int read_pty__Darkloard(char **read_buffer_ptr, size_t *nbytes_read)
{
	char *read_buffer = XMALLOC(BUFSIZ);
	ssize_t res = read(pty_master_fd, read_buffer, BUFSIZ - 1);

	if (res == -1) {
		*nbytes_read = 0;
		*read_buffer_ptr = NULL;

		return 1;
	}

	read_buffer[res] = 0;

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

void close__Darkloard(void)
{
	XFreeGC(display, window_gc);
	XCloseDisplay(display);
	close(pty_master_fd);

	if (is_terminal_alive__Darkloard()) {
		kill(terminal.shell_pid, SIGTERM);
	}
}

void handle_x_events__Darkloard(void)
{
	while (XPending(display)) {
		XEvent event;

		XNextEvent(display, &event);

		switch (event.type) {
			case Expose:
				break;
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
	}
#undef FDS_LEN
}

int main() {
	if (!(display = XOpenDisplay(NULL))) {
		LOG_ERROR("failed to open display\n");
	}

	display_fd = ConnectionNumber(display);

	Window window_root = XDefaultRootWindow(display);
	XWindowAttributes window_root_attr;

	if (XGetWindowAttributes(display, window_root, &window_root_attr) == 0) {
		LOG_ERROR("unable to get window attributes\n");
	}

	window_width = window_root_attr.width;
	window_height = window_root_attr.height;
	window = XCreateSimpleWindow(display, window_root, 0, 0, window_width, window_height, 0, RGB(0, 0, 0), RGB(255, 255, 255));
	window_gc = XCreateGC(display, window, 0, NULL);

	XStoreName(display, window, "Darkloard");
	XSelectInput(display, window, ExposureMask);
	XMapWindow(display, window);

	open_pty__Darkloard();
	handle_x_events__Darkloard();
	poll__Darkloard();
	close__Darkloard();
}
