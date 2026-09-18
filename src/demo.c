#include "arcsync.h"

#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define STAR_N 64
#define TEXT "Win32"

static volatile sig_atomic_t g_stop;

static void
on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

typedef struct {
	float x, y, z;
} star_t;

static unsigned
xrnd(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s;
}

static void
star_reset(star_t *st, unsigned *rng, int near)
{
	st->x = ((int)(xrnd(rng) % 200) - 100) / 50.0f;
	st->y = ((int)(xrnd(rng) % 120) - 60) / 50.0f;
	st->z = near ? (0.2f + (xrnd(rng) % 80) / 100.0f)
	             : (0.5f + (xrnd(rng) % 250) / 100.0f);
}

static void
term_raw(struct termios *old)
{
	struct termios t;
	if (tcgetattr(STDIN_FILENO, old) != 0)
		return;
	t = *old;
	t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void
term_restore(const struct termios *old)
{
	tcsetattr(STDIN_FILENO, TCSANOW, old);
}

/*
 * Tiny BBS/demoscene egg: starfield + "Win32" yaw-spinning in perspective.
 * Esc / q / Ctrl-C to exit. Not listed in --help on purpose.
 */
int
arcsync_demo(void)
{
	star_t stars[STAR_N];
	unsigned rng = (unsigned)time(NULL) ^ (unsigned)getpid();
	struct termios old;
	struct winsize ws;
	int cols = 80, rows = 24;
	int i, frame;
	float theta = 0.0f;
	int have_tty = isatty(STDOUT_FILENO);

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20 && ws.ws_row > 10) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}

	for (i = 0; i < STAR_N; i++)
		star_reset(&stars[i], &rng, 0);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	memset(&old, 0, sizeof(old));
	if (have_tty && isatty(STDIN_FILENO))
		term_raw(&old);

	if (have_tty) {
		fputs("\033[?25l\033[2J", stdout); /* hide cursor, clear */
		fflush(stdout);
	}

	for (frame = 0; !g_stop && frame < 90 * 20; frame++) { /* ~90s @ 20fps */
		int cx = cols / 2;
		int cy = rows / 2;
		char line[512];
		struct pollfd pfd;
		size_t tlen = sizeof(TEXT) - 1;

		/* advance stars */
		for (i = 0; i < STAR_N; i++) {
			stars[i].z -= 0.045f;
			if (stars[i].z < 0.08f)
				star_reset(&stars[i], &rng, 1);
		}
		theta += 0.035f; /* slow spin */

		if (have_tty)
			fputs("\033[H\033[2J", stdout);

		/* starfield */
		for (i = 0; i < STAR_N; i++) {
			float z = stars[i].z;
			int sx, sy;
			const char *glyph;
			if (z < 0.08f)
				continue;
			sx = cx + (int)(stars[i].x / z * (cols * 0.22f));
			sy = cy + (int)(stars[i].y / z * (rows * 0.22f));
			if (sx < 0 || sx >= cols || sy < 1 || sy >= rows - 1)
				continue;
			if (z < 0.35f)
				glyph = "*";
			else if (z < 0.7f)
				glyph = "+";
			else
				glyph = ".";
			printf("\033[%d;%dH%s", sy + 1, sx + 1, glyph);
		}

		/* spinning Win32 — yaw around Y, perspective */
		for (i = 0; (size_t)i < tlen; i++) {
			float x0 = ((float)i - (float)(tlen - 1) * 0.5f) * 1.15f;
			float x = x0 * cosf(theta);
			float z = x0 * sinf(theta);
			float depth = 3.2f + z;
			float scale;
			int sx, sy;
			int bright;
			char ch = TEXT[i];
			if (depth < 0.6f)
				continue;
			scale = 14.0f / depth;
			sx = cx + (int)(x * scale * 2.2f);
			sy = cy + (int)(sinf(theta * 0.5f) * 2.0f); /* slight nod */
			if (sx < 1 || sx >= cols || sy < 1 || sy >= rows - 1)
				continue;
			bright = (int)((z + 2.0f) * 3.0f);
			if (bright < 0) bright = 0;
			if (bright > 5) bright = 5;
			/* dim→bright via ANSI 90/37/97 */
			{
				const char *col = "\033[90m";
				if (bright >= 4)
					col = "\033[97;1m";
				else if (bright >= 2)
					col = "\033[37m";
				printf("\033[%d;%dH%s%c\033[0m", sy + 1, sx + 1, col, ch);
			}
		}

		/* footer scrolltext */
		snprintf(line, sizeof(line),
		    " arcsync — Win32 asm photo-CD tools, 1998 · esc/q to exit ");
		{
			int len = (int)strlen(line);
			int off = (frame / 2) % (len + cols);
			int x;
			printf("\033[%d;1H\033[K\033[36m", rows);
			for (x = 0; x < cols; x++) {
				int idx = x + off - cols;
				if (idx >= 0 && idx < len)
					fputc(line[idx], stdout);
				else
					fputc(' ', stdout);
			}
			fputs("\033[0m", stdout);
		}
		fflush(stdout);

		/* key? */
		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 0) > 0) {
			char c = 0;
			if (read(STDIN_FILENO, &c, 1) == 1) {
				if (c == 'q' || c == 'Q' || c == 27 || c == 3)
					g_stop = 1;
			}
		}
		usleep(50000); /* 20 fps */
	}

	if (have_tty) {
		printf("\033[?25h\033[0m\033[%d;1H\n", rows);
		fflush(stdout);
	}
	if (isatty(STDIN_FILENO))
		term_restore(&old);

	arcsync_banner();
	return 0;
}
