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

#define STAR_N 72
#define MAX_EDGES 256
#define FB_W 160
#define FB_H 60

static volatile sig_atomic_t g_stop;

static void
on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

typedef struct { float x, y, z; } vec3;
typedef struct { int a, b; } edge_t;
typedef struct { float x, y, z; } star_t;

static unsigned
xrnd(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s;
}

static void
star_reset(star_t *st, unsigned *rng, int near)
{
	st->x = ((int)(xrnd(rng) % 200) - 100) / 40.0f;
	st->y = ((int)(xrnd(rng) % 120) - 60) / 40.0f;
	st->z = near ? (0.25f + (xrnd(rng) % 60) / 100.0f)
	             : (0.6f + (xrnd(rng) % 280) / 100.0f);
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

/* Extrude a 2D polyline into front+back edges + connectors. */
static void
add_edge(edge_t *edges, int *ne, int a, int b)
{
	int i;
	if (*ne >= MAX_EDGES)
		return;
	for (i = 0; i < *ne; i++) {
		if ((edges[i].a == a && edges[i].b == b) ||
		    (edges[i].a == b && edges[i].b == a))
			return;
	}
	edges[*ne].a = a;
	edges[*ne].b = b;
	(*ne)++;
}

static int
add_vert(vec3 *v, int *nv, float x, float y, float z)
{
	int i = *nv;
	if (*nv >= 512)
		return 0;
	v[i].x = x;
	v[i].y = y;
	v[i].z = z;
	(*nv)++;
	return i;
}

/*
 * Build 3D wireframe for "Win32" from simple block-letter outlines,
 * extruded in Z for thickness. Letter cells are ~1.0 wide with gaps.
 */
static void
build_win32(vec3 *verts, int *nv, edge_t *edges, int *ne)
{
	/* Each letter: list of 2D point rings / polylines as open paths */
	typedef struct { float x, y; } p2;
	/* Paths for W i n 3 2 — coordinates in [0..1]x[0..1] per cell */
	static const p2 W[] = {
		{0.05f,0.95f},{0.2f,0.05f},{0.35f,0.55f},{0.5f,0.05f},{0.65f,0.55f},{0.8f,0.05f},{0.95f,0.95f}
	};
	static const p2 i_dot[] = { {0.5f,0.85f},{0.5f,0.75f} };
	static const p2 i_stem[] = { {0.5f,0.55f},{0.5f,0.05f} };
	static const p2 n[] = {
		{0.15f,0.05f},{0.15f,0.95f},{0.85f,0.05f},{0.85f,0.95f}
	};
	static const p2 three[] = {
		{0.2f,0.95f},{0.85f,0.95f},{0.85f,0.55f},{0.35f,0.55f},{0.85f,0.55f},{0.85f,0.05f},{0.2f,0.05f}
	};
	static const p2 two[] = {
		{0.15f,0.85f},{0.15f,0.95f},{0.85f,0.95f},{0.85f,0.55f},{0.15f,0.55f},{0.15f,0.05f},{0.85f,0.05f}
	};

	struct { const p2 *pts; int n; float ox; } letters[] = {
		{ W, (int)(sizeof(W)/sizeof(W[0])), -2.4f },
		{ i_dot, 2, -1.2f },
		{ i_stem, 2, -1.2f },
		{ n, 4, -0.2f },
		{ three, 7, 1.0f },
		{ two, 7, 2.2f },
	};
	float thick = 0.22f;
	int li, pi;

	*nv = 0;
	*ne = 0;

	for (li = 0; li < (int)(sizeof(letters)/sizeof(letters[0])); li++) {
		float ox = letters[li].ox;
		const p2 *pts = letters[li].pts;
		int n = letters[li].n;
		int base = *nv;
		/* front and back verts */
		for (pi = 0; pi < n; pi++) {
			float x = ox + pts[pi].x * 0.9f;
			float y = pts[pi].y - 0.5f;
			add_vert(verts, nv, x, y, thick * 0.5f);
		}
		for (pi = 0; pi < n; pi++) {
			float x = ox + pts[pi].x * 0.9f;
			float y = pts[pi].y - 0.5f;
			add_vert(verts, nv, x, y, -thick * 0.5f);
		}
		/* front edges, back edges, ribs */
		for (pi = 0; pi < n - 1; pi++) {
			add_edge(edges, ne, base + pi, base + pi + 1);
			add_edge(edges, ne, base + n + pi, base + n + pi + 1);
			add_edge(edges, ne, base + pi, base + n + pi);
		}
		add_edge(edges, ne, base + n - 1, base + n + n - 1);
	}
}

static void
rot_vec(vec3 *o, const vec3 *in, float ax, float ay, float az)
{
	float cx = cosf(ax), sx = sinf(ax);
	float cy = cosf(ay), sy = sinf(ay);
	float cz = cosf(az), sz = sinf(az);
	float x = in->x, y = in->y, z = in->z;
	float x1, y1, z1;

	/* Rx */
	y1 = y * cx - z * sx;
	z1 = y * sx + z * cx;
	y = y1; z = z1;
	/* Ry */
	x1 = x * cy + z * sy;
	z1 = -x * sy + z * cy;
	x = x1; z = z1;
	/* Rz */
	x1 = x * cz - y * sz;
	y1 = x * sz + y * cz;
	o->x = x1;
	o->y = y1;
	o->z = z;
}

static int
project(const vec3 *v, int cols, int rows, int *sx, int *sy, float *depth)
{
	float z = v->z + 4.5f; /* camera distance */
	float f;
	if (z < 0.35f)
		return 0;
	f = 18.0f / z;
	*sx = cols / 2 + (int)(v->x * f * (cols * 0.045f));
	*sy = rows / 2 - (int)(v->y * f * (rows * 0.09f));
	*depth = z;
	return (*sx >= 0 && *sx < cols && *sy >= 0 && *sy < rows);
}

static void
plot(char *fb, float *zb, int cols, int rows, int x, int y, float z, char ch)
{
	int i;
	if (x < 0 || x >= cols || y < 0 || y >= rows)
		return;
	i = y * cols + x;
	if (z < zb[i]) {
		zb[i] = z;
		fb[i] = ch;
	}
}

static void
line3(char *fb, float *zb, int cols, int rows,
    int x0, int y0, float z0, int x1, int y1, float z1)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int n = (dx > -dy ? dx : -dy) + 1;
	int step = 0;
	for (;;) {
		float t = n > 1 ? (float)step / (float)(n - 1) : 0.0f;
		float z = z0 + (z1 - z0) * t;
		char ch = (z < 3.8f) ? '#' : (z < 4.6f) ? '*' : '+';
		plot(fb, zb, cols, rows, x0, y0, z, ch);
		if (x0 == x1 && y0 == y1)
			break;
		{
			int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
		step++;
		if (step > 400)
			break;
	}
}

int
arcsync_demo(void)
{
	star_t stars[STAR_N];
	vec3 verts[512], rverts[512];
	edge_t edges[MAX_EDGES];
	int nv = 0, ne = 0;
	unsigned rng = (unsigned)time(NULL) ^ (unsigned)getpid();
	struct termios old;
	struct winsize ws;
	int cols = 80, rows = 24;
	int i, frame;
	float ax = 0.4f, ay = 0.2f, az = 0.0f;
	int have_tty = isatty(STDOUT_FILENO);
	char *fb = NULL;
	float *zb = NULL;

	build_win32(verts, &nv, edges, &ne);

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 40 && ws.ws_row > 12) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}
	if (cols > FB_W) cols = FB_W;
	if (rows > FB_H) rows = FB_H;

	fb = arcsync_xmalloc((size_t)cols * (size_t)rows);
	zb = arcsync_xmalloc((size_t)cols * (size_t)rows * sizeof(float));

	for (i = 0; i < STAR_N; i++)
		star_reset(&stars[i], &rng, 0);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	memset(&old, 0, sizeof(old));
	if (have_tty && isatty(STDIN_FILENO))
		term_raw(&old);
	if (have_tty) {
		fputs("\033[?25l\033[2J", stdout);
		fflush(stdout);
	}

	for (frame = 0; !g_stop && frame < 90 * 20; frame++) {
		struct pollfd pfd;
		int e;

		for (i = 0; i < STAR_N; i++) {
			stars[i].z -= 0.04f;
			if (stars[i].z < 0.08f)
				star_reset(&stars[i], &rng, 1);
		}
		/* whole-hog tumble — incommensurate rates */
		ax += 0.028f;
		ay += 0.041f;
		az += 0.019f;

		memset(fb, ' ', (size_t)cols * (size_t)rows);
		for (i = 0; i < cols * rows; i++)
			zb[i] = 1e9f;

		/* stars into fb */
		for (i = 0; i < STAR_N; i++) {
			float z = stars[i].z;
			int sx, sy;
			char g;
			if (z < 0.08f)
				continue;
			sx = cols / 2 + (int)(stars[i].x / z * (cols * 0.2f));
			sy = rows / 2 + (int)(stars[i].y / z * (rows * 0.2f));
			g = z < 0.3f ? '*' : (z < 0.65f ? '+' : '.');
			plot(fb, zb, cols, rows, sx, sy, 8.0f + z, g);
		}

		for (i = 0; i < nv; i++)
			rot_vec(&rverts[i], &verts[i], ax, ay, az);

		for (e = 0; e < ne; e++) {
			int x0, y0, x1, y1;
			float z0, z1;
			if (!project(&rverts[edges[e].a], cols, rows, &x0, &y0, &z0))
				continue;
			if (!project(&rverts[edges[e].b], cols, rows, &x1, &y1, &z1))
				continue;
			line3(fb, zb, cols, rows, x0, y0, z0, x1, y1, z1);
		}

		if (have_tty)
			fputs("\033[H", stdout);
		for (i = 0; i < rows; i++) {
			int c;
			if (i == rows - 1) {
				const char *msg =
				    " 1998 · arcsync — Win32 asm photo-CD · q/esc ";
				int len = (int)strlen(msg);
				int off = (frame / 2) % (len + cols);
				fputs("\033[36m", stdout);
				for (c = 0; c < cols; c++) {
					int idx = c + off - cols;
					fputc((idx >= 0 && idx < len) ? msg[idx] : ' ', stdout);
				}
				fputs("\033[0m\n", stdout);
				continue;
			}
			fputs("\033[37m", stdout);
			fwrite(fb + i * cols, 1, (size_t)cols, stdout);
			fputs("\033[0m\n", stdout);
		}
		fflush(stdout);

		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 0) > 0) {
			char ch = 0;
			if (read(STDIN_FILENO, &ch, 1) == 1 &&
			    (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3))
				g_stop = 1;
		}
		usleep(50000);
	}

	free(fb);
	free(zb);
	if (have_tty) {
		printf("\033[?25h\033[0m\n");
		fflush(stdout);
	}
	if (isatty(STDIN_FILENO))
		term_restore(&old);
	arcsync_banner();
	return 0;
}
