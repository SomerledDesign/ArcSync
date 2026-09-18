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
#define MAX_FACE 3072
#define MAX_VERT 8192
#define FB_W 200
#define FB_H 120

static volatile sig_atomic_t g_stop;

static void
on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

typedef struct { float x, y, z; } vec3;
typedef struct {
	int i0, i1, i2; /* triangle */
	float nx, ny, nz;
} face_t;
typedef struct { float x, y, z; } star_t;

static float g_cam = 8.0f;
static float g_half_w = 1.0f;
static float g_half_h = 1.0f;

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
tri_normal(face_t *f, const vec3 *v)
{
	vec3 a, b;
	float lx, ly, lz, inv;
	a.x = v[f->i1].x - v[f->i0].x;
	a.y = v[f->i1].y - v[f->i0].y;
	a.z = v[f->i1].z - v[f->i0].z;
	b.x = v[f->i2].x - v[f->i0].x;
	b.y = v[f->i2].y - v[f->i0].y;
	b.z = v[f->i2].z - v[f->i0].z;
	lx = a.y * b.z - a.z * b.y;
	ly = a.z * b.x - a.x * b.z;
	lz = a.x * b.y - a.y * b.x;
	inv = sqrtf(lx * lx + ly * ly + lz * lz);
	if (inv < 1e-8f) {
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
add_tri(face_t *faces, int *nf, vec3 *verts, int i0, int i1, int i2)
{
	face_t *f;
	if (*nf >= MAX_FACE || i0 < 0 || i1 < 0 || i2 < 0)
		return;
	f = &faces[*nf];
	f->i0 = i0;
	f->i1 = i1;
	f->i2 = i2;
	tri_normal(f, verts);
	(*nf)++;
}

/* Axis-aligned box in letter space → world, extruded in Z. Guarantees front faces. */
static void
add_box_xy(face_t *faces, int *nf, vec3 *verts, int *nv,
    float x0, float y0, float x1, float y1, float z0, float z1)
{
	int i00, i10, i11, i01, j00, j10, j11, j01;
	if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
	/* front (+Z) */
	i00 = add_vert(verts, nv, x0, y0, z1);
	i10 = add_vert(verts, nv, x1, y0, z1);
	i11 = add_vert(verts, nv, x1, y1, z1);
	i01 = add_vert(verts, nv, x0, y1, z1);
	add_tri(faces, nf, verts, i00, i10, i11);
	add_tri(faces, nf, verts, i00, i11, i01);
	/* back (-Z) */
	j00 = add_vert(verts, nv, x0, y0, z0);
	j10 = add_vert(verts, nv, x1, y0, z0);
	j11 = add_vert(verts, nv, x1, y1, z0);
	j01 = add_vert(verts, nv, x0, y1, z0);
	add_tri(faces, nf, verts, j00, j11, j10);
	add_tri(faces, nf, verts, j00, j01, j11);
	/* +X */
	add_tri(faces, nf, verts, i10, j10, j11);
	add_tri(faces, nf, verts, i10, j11, i11);
	/* -X */
	add_tri(faces, nf, verts, i00, i01, j01);
	add_tri(faces, nf, verts, i00, j01, j00);
	/* +Y */
	add_tri(faces, nf, verts, i01, i11, j11);
	add_tri(faces, nf, verts, i01, j11, j01);
	/* -Y */
	add_tri(faces, nf, verts, i00, j00, j10);
	add_tri(faces, nf, verts, i00, j10, i10);
}

static void
letter_box(face_t *faces, int *nf, vec3 *verts, int *nv,
    float ox, float oy, float sx, float sy,
    float u0, float v0, float u1, float v1, float z0, float z1)
{
	add_box_xy(faces, nf, verts, nv,
	    ox + u0 * sx, oy + v0 * sy,
	    ox + u1 * sx, oy + v1 * sy,
	    z0, z1);
}

static void
build_win32(vec3 *verts, int *nv, face_t *faces, int *nf)
{
	float z0 = -0.22f, z1 = 0.22f;
	float cell_w = 1.05f, gap = 0.14f;
	float cell_h = 1.22f; /* a little more height */
	float total = 5.0f * cell_w + 4.0f * gap;
	float ox = -0.5f * total;
	float oy = -0.5f * cell_h;
	float sx = cell_w, sy = cell_h;

	*nv = 0;
	*nf = 0;

	/* W — left stem, mid-left diag bar as stepped boxes, mid-right, right stem */
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.00f, 0.00f, 0.16f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.84f, 0.00f, 1.00f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.16f, 0.00f, 0.36f, 0.22f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.28f, 0.18f, 0.48f, 0.48f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.40f, 0.42f, 0.60f, 0.72f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.52f, 0.18f, 0.72f, 0.48f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.64f, 0.00f, 0.84f, 0.22f, z0, z1);
	ox += cell_w + gap;

	/* i — stem + dot */
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.32f, 0.00f, 0.68f, 0.64f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.32f, 0.76f, 0.68f, 1.00f, z0, z1);
	ox += cell_w + gap;

	/* n — left stem, top bar, right stem, small shoulder */
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.08f, 0.00f, 0.32f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.32f, 0.72f, 0.68f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.68f, 0.00f, 0.92f, 1.00f, z0, z1);
	ox += cell_w + gap;

	/* 3 — top bar, mid bar, bottom bar, right column (with gaps) */
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.82f, 0.90f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.42f, 0.78f, 0.58f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.00f, 0.90f, 0.18f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.72f, 0.58f, 0.92f, 0.82f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.72f, 0.18f, 0.92f, 0.42f, z0, z1);
	ox += cell_w + gap;

	/* 2 — top bar, upper-right, diagonal steps, bottom bar */
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.82f, 0.90f, 1.00f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.70f, 0.58f, 0.90f, 0.82f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.48f, 0.42f, 0.78f, 0.62f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.26f, 0.26f, 0.56f, 0.46f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.18f, 0.40f, 0.34f, z0, z1);
	letter_box(faces, nf, verts, nv, ox, oy, sx, sy, 0.10f, 0.00f, 0.90f, 0.18f, z0, z1);
}

static void
mesh_center_and_fit(vec3 *verts, int nv)
{
	int i;
	float minx, maxx, miny, maxy, minz, maxz;
	float cx, cy, cz;
	if (nv <= 0)
		return;
	minx = maxx = verts[0].x;
	miny = maxy = verts[0].y;
	minz = maxz = verts[0].z;
	for (i = 1; i < nv; i++) {
		if (verts[i].x < minx) minx = verts[i].x;
		if (verts[i].x > maxx) maxx = verts[i].x;
		if (verts[i].y < miny) miny = verts[i].y;
		if (verts[i].y > maxy) maxy = verts[i].y;
		if (verts[i].z < minz) minz = verts[i].z;
		if (verts[i].z > maxz) maxz = verts[i].z;
	}
	cx = 0.5f * (minx + maxx);
	cy = 0.5f * (miny + maxy);
	cz = 0.5f * (minz + maxz);
	for (i = 0; i < nv; i++) {
		verts[i].x -= cx;
		verts[i].y -= cy;
		verts[i].z -= cz;
	}
	g_half_w = 0.5f * (maxx - minx);
	g_half_h = 0.5f * (maxy - miny);
	if (g_half_w < 0.05f) g_half_w = 0.05f;
	if (g_half_h < 0.05f) g_half_h = 0.05f;
	{
		float extent = g_half_w > g_half_h ? g_half_w : g_half_h;
		g_cam = extent * 4.2f + (maxz - minz) * 1.5f;
		if (g_cam < 6.0f) g_cam = 6.0f;
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
project(const vec3 *v, int cols, int rows2, int *sx, int *sy, float *depth)
{
	float z = v->z + g_cam;
	float px, py;
	if (z < 0.5f)
		return 0;
	{
		float kx = (0.72f * (float)cols) / (2.0f * g_half_w);
		float ky = (0.72f * (float)rows2) / (2.0f * g_half_h);
		float k = kx < ky ? kx : ky;
		px = v->x * k * (g_cam / z);
		py = v->y * k * (g_cam / z);
	}
	*sx = cols / 2 + (int)(px + (px >= 0 ? 0.5f : -0.5f));
	*sy = rows2 / 2 - (int)(py + (py >= 0 ? 0.5f : -0.5f));
	*depth = z;
	return 1;
}

static void
plot(unsigned char *fb, float *zb, int cols, int rows2,
    int x, int y, float z, unsigned char shade)
{
	int i;
	if (x < 0 || x >= cols || y < 0 || y >= rows2)
		return;
	i = y * cols + x;
	if (z < zb[i]) {
		zb[i] = z;
		fb[i] = shade;
	}
}

static void
fill_tri(unsigned char *fb, float *zb, int cols, int rows2,
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
	if (maxx < 0 || maxy < 0 || minx >= cols || miny >= rows2)
		return;
	if (minx < 0) minx = 0;
	if (miny < 0) miny = 0;
	if (maxx >= cols) maxx = cols - 1;
	if (maxy >= rows2) maxy = rows2 - 1;

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
			plot(fb, zb, cols, rows2, x, y, z, shade);
		}
	}
}

static unsigned char
blue_shade(float lambert)
{
	int s;
	if (lambert < 0.0f) lambert = 0.0f;
	if (lambert > 1.0f) lambert = 1.0f;
	s = 1 + (int)(lambert * 7.0f + 0.5f);
	if (s < 1) s = 1;
	if (s > 8) s = 8;
	return (unsigned char)s;
}

static void
shade_rgb(unsigned char shade, int *r, int *g, int *b)
{
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
	*r = rgb[i][0];
	*g = rgb[i][1];
	*b = rgb[i][2];
}

/* Half-block cell: upper fb row = fg, lower = bg of ▀ */
static void
emit_half(unsigned char top, unsigned char bot)
{
	int r1, g1, b1, r2, g2, b2;
	int t_letter = (top >= 1 && top <= 8);
	int b_letter = (bot >= 1 && bot <= 8);
	int t_star = (top >= 200);
	int b_star = (bot >= 200);

	if (!t_letter && !b_letter && !t_star && !b_star) {
		fputc(' ', stdout);
		return;
	}
	if (t_letter)
		shade_rgb(top, &r1, &g1, &b1);
	else if (t_star) {
		r1 = g1 = b1 = (top == 202) ? 220 : (top == 201 ? 170 : 120);
	} else {
		r1 = g1 = b1 = 0;
	}
	if (b_letter)
		shade_rgb(bot, &r2, &g2, &b2);
	else if (b_star) {
		r2 = g2 = b2 = (bot == 202) ? 220 : (bot == 201 ? 170 : 120);
	} else {
		r2 = g2 = b2 = 0;
	}

	if (t_letter || b_letter || t_star || b_star) {
		printf("\033[38;2;%d;%d;%d;48;2;%d;%d;%dm▀\033[0m",
		    r1, g1, b1, r2, g2, b2);
	}
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
	int cols = 80, rows = 24, rows2;
	int i, frame;
	float tip = 0.0f, yaw = 0.0f; /* tip=pitch (X), yaw=heading (Y) */
	float tip_phase = 0.0f, yaw_phase = 0.0f;
	const float tip_amp = 25.0f * (float)M_PI / 180.0f;   /* ±25° */
	const float yaw_amp = 120.0f * (float)M_PI / 180.0f;  /* ±120° */
	const float tip_speed = 0.035f;
	const float yaw_speed = 0.022f;
	int have_tty = isatty(STDOUT_FILENO);
	unsigned char *fb = NULL;
	float *zb = NULL;
	const float Lx = -0.35f, Ly = 0.55f, Lz = 0.75f;

	verts = arcsync_xmalloc((size_t)MAX_VERT * sizeof(vec3));
	rverts = arcsync_xmalloc((size_t)MAX_VERT * sizeof(vec3));
	faces = arcsync_xmalloc((size_t)MAX_FACE * sizeof(face_t));
	build_win32(verts, &nv, faces, &nf);
	mesh_center_and_fit(verts, nv);

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 40 && ws.ws_row > 12) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}
	if (cols > FB_W) cols = FB_W;
	if (rows > FB_H / 2) rows = FB_H / 2;
	rows2 = rows * 2;

	fb = arcsync_xmalloc((size_t)cols * (size_t)rows2);
	zb = arcsync_xmalloc((size_t)cols * (size_t)rows2 * sizeof(float));

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
		tip_phase += tip_speed;
		yaw_phase += yaw_speed;
		tip = tip_amp * sinf(tip_phase);
		yaw = yaw_amp * sinf(yaw_phase);

		memset(fb, 0, (size_t)cols * (size_t)rows2);
		for (i = 0; i < cols * rows2; i++)
			zb[i] = 1e9f;

		for (i = 0; i < STAR_N; i++) {
			float z = stars[i].z;
			int sx, sy;
			unsigned char g;
			if (z < 0.08f)
				continue;
			sx = cols / 2 + (int)(stars[i].x / z * (cols * 0.2f));
			sy = rows2 / 2 + (int)(stars[i].y / z * (rows2 * 0.2f));
			g = (unsigned char)(200 + (z < 0.3f ? 2 : (z < 0.65f ? 1 : 0)));
			plot(fb, zb, cols, rows2, sx, sy, 9.0f + z, g);
		}

		for (i = 0; i < nv; i++)
			rot_vec(&rverts[i], &verts[i], tip, yaw, 0.0f);

		for (f = 0; f < nf; f++) {
			float nx = faces[f].nx, ny = faces[f].ny, nz = faces[f].nz;
			float ndot;
			int x0, y0, x1, y1, x2, y2;
			float z0, z1, z2;
			unsigned char shade;

			rot_normal(&nx, &ny, &nz, tip, yaw, 0.0f);
			if (nz < 0.02f)
				continue;
			ndot = nx * Lx + ny * Ly + nz * Lz;
			if (ndot < 0.0f)
				ndot = 0.0f;
			shade = blue_shade(0.32f + 0.68f * ndot);

			if (!project(&rverts[faces[f].i0], cols, rows2, &x0, &y0, &z0))
				continue;
			if (!project(&rverts[faces[f].i1], cols, rows2, &x1, &y1, &z1))
				continue;
			if (!project(&rverts[faces[f].i2], cols, rows2, &x2, &y2, &z2))
				continue;
			fill_tri(fb, zb, cols, rows2,
			    x0, y0, z0, x1, y1, z1, x2, y2, z2, shade);
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
			for (c = 0; c < cols; c++) {
				unsigned char top = fb[(i * 2) * cols + c];
				unsigned char bot = fb[(i * 2 + 1) * cols + c];
				emit_half(top, bot);
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
