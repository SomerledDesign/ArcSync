#include "arcsync.h"

#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void *
arcsync_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fprintf(stderr, "arcsync: out of memory\n");
		exit(4);
	}
	return p;
}

char *
arcsync_xstrdup(const char *s)
{
	size_t n;
	char *p;
	if (!s)
		return NULL;
	n = strlen(s) + 1;
	p = arcsync_xmalloc(n);
	memcpy(p, s, n);
	return p;
}

char *
arcsync_aprintf(const char *fmt, ...)
{
	va_list ap, ap2;
	int n;
	char *buf;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return NULL;
	}
	buf = arcsync_xmalloc((size_t)n + 1);
	vsnprintf(buf, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return buf;
}

int
arcsync_mkdir_p(const char *path)
{
	char tmp[4096];
	size_t len;
	char *p;
	if (!path || !*path)
		return -1;
	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int
arcsync_path_join(char *out, size_t outsz, const char *a, const char *b)
{
	int n;
	if (!a || !*a)
		n = snprintf(out, outsz, "%s", b ? b : "");
	else if (!b || !*b)
		n = snprintf(out, outsz, "%s", a);
	else if (a[strlen(a) - 1] == '/')
		n = snprintf(out, outsz, "%s%s", a, b);
	else
		n = snprintf(out, outsz, "%s/%s", a, b);
	return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

void
arcsync_html_escape(FILE *fp, const char *s)
{
	if (!s)
		return;
	for (; *s; s++) {
		switch (*s) {
		case '&': fputs("&amp;", fp); break;
		case '<': fputs("&lt;", fp); break;
		case '>': fputs("&gt;", fp); break;
		case '"': fputs("&quot;", fp); break;
		default: fputc(*s, fp); break;
		}
	}
}

void
arcsync_slugify(const char *in, char *out, size_t outsz)
{
	size_t j = 0;
	int last_dash = 0;
	if (!in || outsz == 0)
		return;
	for (; *in && j + 1 < outsz; in++) {
		unsigned char c = (unsigned char)*in;
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			out[j++] = (char)c;
			last_dash = 0;
		} else if (c >= 'A' && c <= 'Z') {
			out[j++] = (char)(c - 'A' + 'a');
			last_dash = 0;
		} else if (!last_dash && j > 0) {
			out[j++] = '-';
			last_dash = 1;
		}
	}
	while (j > 0 && out[j - 1] == '-')
		j--;
	if (j == 0) {
		snprintf(out, outsz, "album");
		return;
	}
	out[j] = '\0';
}

uint64_t
arcsync_media_budget(arcsync_media_t m)
{
	switch (m) {
	case ARCSYNC_MEDIA_CD: return 680ULL * 1024 * 1024;
	case ARCSYNC_MEDIA_DVD: return 4300ULL * 1024 * 1024;
	case ARCSYNC_MEDIA_DVD_DL: return 7900ULL * 1024 * 1024;
	case ARCSYNC_MEDIA_BD: return 23000ULL * 1024 * 1024;
	case ARCSYNC_MEDIA_NONE: return UINT64_MAX;
	}
	return UINT64_MAX;
}

const char *
arcsync_media_name(arcsync_media_t m)
{
	switch (m) {
	case ARCSYNC_MEDIA_CD: return "cd";
	case ARCSYNC_MEDIA_DVD: return "dvd";
	case ARCSYNC_MEDIA_DVD_DL: return "dvd-dl";
	case ARCSYNC_MEDIA_BD: return "bd";
	case ARCSYNC_MEDIA_NONE: return "none";
	}
	return "?";
}

int
arcsync_parse_ymd(const char *s, struct tm *out)
{
	int y, m, d;
	if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
		return -1;
	memset(out, 0, sizeof(*out));
	out->tm_year = y - 1900;
	out->tm_mon = m - 1;
	out->tm_mday = d;
	out->tm_isdst = -1;
	return 0;
}

int
arcsync_date_in_range(time_t t, const char *from, const char *to)
{
	struct tm tm, lim;
	time_t bound;
	localtime_r(&t, &tm);
	tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
	if (from) {
		if (arcsync_parse_ymd(from, &lim) != 0)
			return 0;
		bound = mktime(&lim);
		if (t < bound)
			return 0;
	}
	if (to) {
		if (arcsync_parse_ymd(to, &lim) != 0)
			return 0;
		lim.tm_hour = 23; lim.tm_min = 59; lim.tm_sec = 59;
		bound = mktime(&lim);
		if (t > bound)
			return 0;
	}
	return 1;
}

/* FNV-1a 64 → 12 hex chars */
char *
arcsync_sha12_hex(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	char *out;
	size_t i;
	for (; s && *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	out = arcsync_xmalloc(13);
	for (i = 0; i < 12; i++) {
		unsigned v = (unsigned)((h >> (60 - 4 * (int)i)) & 0xf);
		out[i] = (char)(v < 10 ? '0' + v : 'a' + (v - 10));
	}
	out[12] = '\0';
	return out;
}

static int
rm_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)sb; (void)typeflag; (void)ftwbuf;
	return remove(fpath);
}

void
arcsync_rm_rf(const char *path)
{
	if (!path || !*path)
		return;
	nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

void
arcsync_catalog_init(arcsync_catalog_t *c)
{
	memset(c, 0, sizeof(*c));
}

void
arcsync_catalog_free(arcsync_catalog_t *c)
{
	size_t i;
	for (i = 0; i < c->n_assets; i++) {
		free(c->assets[i].id);
		free(c->assets[i].src_path);
		free(c->assets[i].orig_name);
		free(c->assets[i].album_title);
		free(c->assets[i].rel_album);
		free(c->assets[i].dest_rel);
		free(c->assets[i].thumb_rel);
	}
	free(c->assets);
	for (i = 0; i < c->n_albums; i++) {
		free(c->albums[i].title);
		free(c->albums[i].slug);
		free(c->albums[i].parent_slug);
		free(c->albums[i].asset_idx);
	}
	free(c->albums);
	memset(c, 0, sizeof(*c));
}

arcsync_asset_t *
arcsync_catalog_add_asset(arcsync_catalog_t *c)
{
	arcsync_asset_t *a;
	if (c->n_assets == c->cap_assets) {
		size_t ncap = c->cap_assets ? c->cap_assets * 2 : 64;
		arcsync_asset_t *na = realloc(c->assets, ncap * sizeof(*na));
		if (!na) {
			fprintf(stderr, "arcsync: out of memory\n");
			exit(4);
		}
		c->assets = na;
		c->cap_assets = ncap;
	}
	a = &c->assets[c->n_assets++];
	memset(a, 0, sizeof(*a));
	return a;
}

arcsync_album_t *
arcsync_catalog_find_or_add_album(arcsync_catalog_t *c, const char *title,
    const char *slug, const char *parent_slug)
{
	size_t i;
	arcsync_album_t *a;
	for (i = 0; i < c->n_albums; i++) {
		if (strcmp(c->albums[i].slug, slug) == 0)
			return &c->albums[i];
	}
	if (c->n_albums == c->cap_albums) {
		size_t ncap = c->cap_albums ? c->cap_albums * 2 : 16;
		arcsync_album_t *na = realloc(c->albums, ncap * sizeof(*na));
		if (!na) {
			fprintf(stderr, "arcsync: out of memory\n");
			exit(4);
		}
		c->albums = na;
		c->cap_albums = ncap;
	}
	a = &c->albums[c->n_albums++];
	memset(a, 0, sizeof(*a));
	a->title = arcsync_xstrdup(title);
	a->slug = arcsync_xstrdup(slug);
	a->parent_slug = parent_slug ? arcsync_xstrdup(parent_slug) : NULL;
	return a;
}

void
arcsync_album_add_asset(arcsync_album_t *a, size_t asset_index)
{
	if (a->n_assets == a->cap_assets) {
		size_t ncap = a->cap_assets ? a->cap_assets * 2 : 16;
		size_t *ni = realloc(a->asset_idx, ncap * sizeof(*ni));
		if (!ni) {
			fprintf(stderr, "arcsync: out of memory\n");
			exit(4);
		}
		a->asset_idx = ni;
		a->cap_assets = ncap;
	}
	a->asset_idx[a->n_assets++] = asset_index;
}
