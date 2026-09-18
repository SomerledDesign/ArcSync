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

#define STAR_N 96
#define MAX_FACE 4096
#define MAX_VERT 12288
#define FB_W 200
#define FB_H 80

static volatile sig_atomic_t g_stop;

static void
on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

typedef struct { float x, y, z; } vec3;
typedef struct {
	int i0, i1, i2, i3; /* quad */
	float nx, ny, nz;
} face_t;
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

static int
add_vert(vec3 *v, int *nv, float x, float y, float z)
{
	int i = *nv;
	if (*nv >= MAX_VERT)
		return -1;
	v[i].x = x;
	v[i].y = y;
	v[i].z = z;
	(*nv)++;
	return i;
}

static void
face_normal(face_t *f, const vec3 *v)
{
	vec3 a, b;
	float lx, ly, lz, inv;
	a.x = v[f->i1].x - v[f->i0].x;
	a.y = v[f->i1].y - v[f->i0].y;
	a.z = v[f->i1].z - v[f->i0].z;
	b.x = v[f->i3].x - v[f->i0].x;
	b.y = v[f->i3].y - v[f->i0].y;
	b.z = v[f->i3].z - v[f->i0].z;
	lx = a.y * b.z - a.z * b.y;
	ly = a.z * b.x - a.x * b.z;
	lz = a.x * b.y - a.y * b.x;
	inv = sqrtf(lx * lx + ly * ly + lz * lz);
	if (inv < 1e-6f) {
		f->nx = 0;
		f->ny = 0;
		f->nz = 1;
		return;
	}
	inv = 1.0f / inv;
	f->nx = lx * inv;
	f->ny = ly * inv;
	f->nz = lz * inv;
}

static void
add_quad(face_t *faces, int *nf, vec3 *verts, int *nv,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    float x3, float y3, float z3)
{
	face_t *f;
	int a, b, c, d;
	if (*nf >= MAX_FACE)
		return;
	a = add_vert(verts, nv, x0, y0, z0);
	b = add_vert(verts, nv, x1, y1, z1);
	c = add_vert(verts, nv, x2, y2, z2);
	d = add_vert(verts, nv, x3, y3, z3);
	if (a < 0 || b < 0 || c < 0 || d < 0)
		return;
	f = &faces[*nf];
	f->i0 = a;
	f->i1 = b;
	f->i2 = c;
	f->i3 = d;
	face_normal(f, verts);
	(*nf)++;
}

/* Axis-aligned box centered at (cx,cy,cz) with half-sizes hx,hy,hz. */
static void
add_box(face_t *faces, int *nf, vec3 *verts, int *nv,
    float cx, float cy, float cz, float hx, float hy, float hz)
{
	float x0 = cx - hx, x1 = cx + hx;
	float y0 = cy - hy, y1 = cy + hy;
	float z0 = cz - hz, z1 = cz + hz;
	/* +Z (front) */
	add_quad(faces, nf, verts, nv, x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1);
	/* -Z (back) */
	add_quad(faces, nf, verts, nv, x1, y0, z0, x0, y0, z0, x0, y1, z0, x1, y1, z0);
	/* +Y (top) */
	add_quad(faces, nf, verts, nv, x0, y1, z1, x1, y1, z1, x1, y1, z0, x0, y1, z0);
	/* -Y (bottom) */
	add_quad(faces, nf, verts, nv, x0, y0, z0, x1, y0, z0, x1, y0, z1, x0, y0, z1);
	/* +X */
	add_quad(faces, nf, verts, nv, x1, y0, z1, x1, y0, z0, x1, y1, z0, x1, y1, z1);
	/* -X */
	add_quad(faces, nf, verts, nv, x0, y0, z0, x0, y0, z1, x0, y1, z1, x0, y1, z0);
}

/*
 * 11x15 block glyphs — denser voxels so shaded faces still read as letters.
 * Rows top→bottom; columns left→right. '#' = solid cube.
 */
#define GLYPH_W 11
#define GLYPH_H 15

static const char *GLYPH_W_ROWS[GLYPH_H] = {
	"#         #",
	"#         #",
	"#         #",
	"#         #",
	"#    #    #",
	"#    #    #",
	"#   # #   #",
	"#   # #   #",
	"#  #   #  #",
	"#  #   #  #",
	"# #     # #",
	"##       ##",
	"#         #",
	"#         #",
	"#         #",
};
static const char *GLYPH_I_ROWS[GLYPH_H] = {
	"  #######  ",
	"  #######  ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"    ###    ",
	"  #######  ",
	"  #######  ",
};
static const char *GLYPH_N_ROWS[GLYPH_H] = {
	"#         #",
	"##        #",
	"##        #",
	"# #       #",
	"# #       #",
	"#  #      #",
	"#  #      #",
	"#   #     #",
	"#   #     #",
	"#    #    #",
	"#    #    #",
	"#     #   #",
	"#      #  #",
	"#       # #",
	"#        ##",
};
static const char *GLYPH_3_ROWS[GLYPH_H] = {
	" ######### ",
	"###########",
	"##       ##",
	"         ##",
	"         ##",
	"        ## ",
	"   ####### ",
	"   ####### ",
	"        ## ",
	"         ##",
	"         ##",
	"##       ##",
	"##       ##",
	"###########",
	" ######### ",
};
static const char *GLYPH_2_ROWS[GLYPH_H] = {
	" ######### ",
	"###########",
	"##       ##",
	"         ##",
	"         ##",
	"        ## ",
	"       ##  ",
	"      ##   ",
	"     ##    ",
	"    ##     ",
	"   ##      ",
	"  ##       ",
	" ##      ##",
	"###########",
	"###########",
};

static void
add_glyph(face_t *faces, int *nf, vec3 *verts, int *nv,
    const char *rows[], float ox, float cell, float depth)
{
	int r, c;
	float half = cell * 0.48f;
	float hz = depth * 0.5f;
	float y_top = (GLYPH_H - 1) * 0.5f * cell;
	for (r = 0; r < GLYPH_H; r++) {
		for (c = 0; c < GLYPH_W; c++) {
			if (rows[r][c] != '#')
				continue;
			add_box(faces, nf, verts, nv,
			    ox + (c + 0.5f) * cell,
			    y_top - r * cell,
			    0.0f,
			    half, half, hz);
		}
	}
}

static void
build_win32(vec3 *verts, int *nv, face_t *faces, int *nf)
{
	/* Smaller cells + more pixels = smoother silhouette; slightly larger overall. */
	float cell = 0.155f;
	float gap = 0.20f;
	float depth = 0.34f;
	float w = (float)GLYPH_W * cell;
	float x = -(2.5f * w + 2.0f * gap);

	*nv = 0;
	*nf = 0;
	add_glyph(faces, nf, verts, nv, GLYPH_W_ROWS, x, cell, depth); x += w + gap;
	add_glyph(faces, nf, verts, nv, GLYPH_I_ROWS, x, cell, depth); x += w + gap;
	add_glyph(faces, nf, verts, nv, GLYPH_N_ROWS, x, cell, depth); x += w + gap;
	add_glyph(faces, nf, verts, nv, GLYPH_3_ROWS, x, cell, depth); x += w + gap;
	add_glyph(faces, nf, verts, nv, GLYPH_2_ROWS, x, cell, depth);
}

static void
rot_vec(vec3 *o, const vec3 *in, float ax, float ay, float az)
{
	float cx = cosf(ax), sx = sinf(ax);
	float cy = cosf(ay), sy = sinf(ay);
	float cz = cosf(az), sz = sinf(az);
	float x = in->x, y = in->y, z = in->z;
	float x1, y1, z1;

	y1 = y * cx - z * sx;
	z1 = y * sx + z * cx;
	y = y1; z = z1;
	x1 = x * cy + z * sy;
	z1 = -x * sy + z * cy;
	x = x1; z = z1;
	x1 = x * cz - y * sz;
	y1 = x * sz + y * cz;
	o->x = x1;
	o->y = y1;
	o->z = z;
}

static void
rot_normal(float *nx, float *ny, float *nz, float ax, float ay, float az)
{
	vec3 in, out;
	in.x = *nx;
	in.y = *ny;
	in.z = *nz;
	rot_vec(&out, &in, ax, ay, az);
	*nx = out.x;
	*ny = out.y;
	*nz = out.z;
}

static int
project(const vec3 *v, int cols, int rows, int *sx, int *sy, float *depth)
{
	float z = v->z + 5.2f;
	float f;
	if (z < 0.4f)
		return 0;
	f = 26.0f / z;
	*sx = cols / 2 + (int)(v->x * f * (cols * 0.042f));
	*sy = rows / 2 - (int)(v->y * f * (rows * 0.095f));
	*depth = z;
	return 1;
}

/* shade 0 = empty/star path uses separate; 1..8 = blue ramp (bright) */
static void
plot(unsigned char *fb, float *zb, int cols, int rows,
    int x, int y, float z, unsigned char shade)
{
	int i;
	if (x < 0 || x >= cols || y < 0 || y >= rows)
		return;
	i = y * cols + x;
	if (z < zb[i]) {
		zb[i] = z;
		fb[i] = shade;
	}
}

static void
fill_tri(unsigned char *fb, float *zb, int cols, int rows,
    int x0, int y0, float z0,
    int x1, int y1, float z1,
    int x2, int y2, float z2,
    unsigned char shade)
{
	int minx, maxx, miny, maxy, x, y;
	float area, w0, w1, w2, z;

	minx = x0 < x1 ? x0 : x1;
	if (x2 < minx) minx = x2;
	maxx = x0 > x1 ? x0 : x1;
	if (x2 > maxx) maxx = x2;
	miny = y0 < y1 ? y0 : y1;
	if (y2 < miny) miny = y2;
	maxy = y0 > y1 ? y0 : y1;
	if (y2 > maxy) maxy = y2;
	if (maxx < 0 || maxy < 0 || minx >= cols || miny >= rows)
		return;
	if (minx < 0) minx = 0;
	if (miny < 0) miny = 0;
	if (maxx >= cols) maxx = cols - 1;
	if (maxy >= rows) maxy = rows - 1;

	area = (float)((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
	if (area > -1.0f && area < 1.0f)
		return;
	for (y = miny; y <= maxy; y++) {
		for (x = minx; x <= maxx; x++) {
			w0 = (float)((x1 - x) * (y2 - y) - (x2 - x) * (y1 - y));
			w1 = (float)((x2 - x) * (y0 - y) - (x0 - x) * (y2 - y));
			w2 = (float)((x0 - x) * (y1 - y) - (x1 - x) * (y0 - y));
			if (area > 0) {
				if (w0 < 0 || w1 < 0 || w2 < 0)
					continue;
			} else {
				if (w0 > 0 || w1 > 0 || w2 > 0)
					continue;
			}
			w0 /= area;
			w1 /= area;
			w2 /= area;
			z = w0 * z0 + w1 * z1 + w2 * z2;
			plot(fb, zb, cols, rows, x, y, z, shade);
		}
	}
}

static void
fill_quad(unsigned char *fb, float *zb, int cols, int rows,
    int x0, int y0, float z0,
    int x1, int y1, float z1,
    int x2, int y2, float z2,
    int x3, int y3, float z3,
    unsigned char shade)
{
	fill_tri(fb, zb, cols, rows, x0, y0, z0, x1, y1, z1, x2, y2, z2, shade);
	fill_tri(fb, zb, cols, rows, x0, y0, z0, x2, y2, z2, x3, y3, z3, shade);
}

/* Map Lambert [0..1] → blue shade 1..8 (8 brightest). */
static unsigned char
blue_shade(float lambert)
{
	int s;
	if (lambert < 0.0f)
		lambert = 0.0f;
	if (lambert > 1.0f)
		lambert = 1.0f;
	s = 1 + (int)(lambert * 7.0f + 0.5f);
	if (s < 1) s = 1;
	if (s > 8) s = 8;
	return (unsigned char)s;
}

static void
emit_blue_cell(unsigned char shade)
{
	/* Truecolor blues — classic Win32-ish cobalt ramp */
	static const int rgb[9][3] = {
		{0, 0, 0},
		{8, 18, 64},
		{12, 32, 110},
		{16, 48, 150},
		{20, 70, 190},
		{30, 100, 220},
		{50, 130, 240},
		{80, 170, 255},
		{140, 200, 255},
	};
	int i = shade;
	if (i < 1) i = 1;
	if (i > 8) i = 8;
	printf("\033[48;2;%d;%d;%dm \033[0m", rgb[i][0], rgb[i][1], rgb[i][2]);
}

int
arcsync_demo(void)
{
	star_t stars[STAR_N];
	vec3 *verts = NULL, *rverts = NULL;
	face_t *faces = NULL;
	int nv = 0, nf = 0;
	unsigned rng = (unsigned)time(NULL) ^ (unsigned)getpid();
	struct termios old;
	struct winsize ws;
	int cols = 80, rows = 24;
	int i, frame;
	float ax = 0.35f, ay = 0.15f, az = 0.0f;
	int have_tty = isatty(STDOUT_FILENO);
	unsigned char *fb = NULL;
	float *zb = NULL;
	/* light in camera space, slightly above-left */
	const float Lx = -0.35f, Ly = 0.55f, Lz = 0.75f;

	verts = arcsync_xmalloc((size_t)MAX_VERT * sizeof(vec3));
	rverts = arcsync_xmalloc((size_t)MAX_VERT * sizeof(vec3));
	faces = arcsync_xmalloc((size_t)MAX_FACE * sizeof(face_t));
	build_win32(verts, &nv, faces, &nf);

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
		int f;

		for (i = 0; i < STAR_N; i++) {
			stars[i].z -= 0.035f;
			if (stars[i].z < 0.08f)
				star_reset(&stars[i], &rng, 1);
		}
		ax += 0.0165f; /* 25% slower */
		ay += 0.02475f;
		az += 0.0105f;

		memset(fb, 0, (size_t)cols * (size_t)rows);
		for (i = 0; i < cols * rows; i++)
			zb[i] = 1e9f;

		/* dim starfield (shade 0 kept; draw as fg dots later) — use zb only */
		for (i = 0; i < STAR_N; i++) {
			float z = stars[i].z;
			int sx, sy;
			unsigned char g;
			if (z < 0.08f)
				continue;
			sx = cols / 2 + (int)(stars[i].x / z * (cols * 0.2f));
			sy = rows / 2 + (int)(stars[i].y / z * (rows * 0.2f));
			/* encode stars as 200+brightness in fb when empty */
			g = (unsigned char)(200 + (z < 0.3f ? 2 : (z < 0.65f ? 1 : 0)));
			plot(fb, zb, cols, rows, sx, sy, 9.0f + z, g);
		}

		for (i = 0; i < nv; i++)
			rot_vec(&rverts[i], &verts[i], ax, ay, az);

		for (f = 0; f < nf; f++) {
			float nx = faces[f].nx, ny = faces[f].ny, nz = faces[f].nz;
			float ndot;
			int x0, y0, x1, y1, x2, y2, x3, y3;
			float z0, z1, z2, z3;
			unsigned char shade;

			rot_normal(&nx, &ny, &nz, ax, ay, az);
			/* back-face cull in view space (camera looks -Z toward +Z objects) */
			if (nz < 0.05f)
				continue;
			ndot = nx * Lx + ny * Ly + nz * Lz;
			if (ndot < 0.0f)
				ndot = 0.0f;
			/* ambient + diffuse */
			shade = blue_shade(0.22f + 0.78f * ndot);

			if (!project(&rverts[faces[f].i0], cols, rows, &x0, &y0, &z0))
				continue;
			if (!project(&rverts[faces[f].i1], cols, rows, &x1, &y1, &z1))
				continue;
			if (!project(&rverts[faces[f].i2], cols, rows, &x2, &y2, &z2))
				continue;
			if (!project(&rverts[faces[f].i3], cols, rows, &x3, &y3, &z3))
				continue;
			fill_quad(fb, zb, cols, rows,
			    x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3, shade);
		}

		if (have_tty)
			fputs("\033[H", stdout);
		for (i = 0; i < rows; i++) {
			int c;
			if (i == rows - 1) {
				const char *msg =
				    " 1998 · arcsync — solid Win32 · q/esc ";
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
			for (c = 0; c < cols; c++) {
				unsigned char s = fb[i * cols + c];
				if (s >= 1 && s <= 8) {
					emit_blue_cell(s);
				} else if (s >= 200) {
					char ch = (s == 202) ? '*' : (s == 201 ? '+' : '.');
					fputs("\033[38;5;250m", stdout);
					fputc(ch, stdout);
					fputs("\033[0m", stdout);
				} else {
					fputc(' ', stdout);
				}
			}
			fputc('\n', stdout);
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
		usleep(45000);
	}

	free(fb);
	free(zb);
	free(verts);
	free(rverts);
	free(faces);
	if (have_tty) {
		printf("\033[?25h\033[0m\n");
		fflush(stdout);
	}
	if (isatty(STDIN_FILENO))
		term_restore(&old);
	arcsync_banner();
	return 0;
}
