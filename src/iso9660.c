/*
 * Minimal ISO 9660 + Joliet writer (portable / Windows).
 */
#include "arcsync.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
# include <io.h>
# define unlink _unlink
#else
# include <unistd.h>
#endif

#define SECTOR 2048
#define MAX_DEPTH 16

typedef struct iso_node {
	char *name;
	char *full;
	int is_dir;
	uint32_t size;
	uint32_t extent;
	uint32_t data_len;
	struct iso_node *parent;
	struct iso_node **kids;
	size_t n_kids, cap_kids;
	char name83[16];
} iso_node_t;

typedef struct {
	iso_node_t *node;
	uint16_t parent_idx;
} path_ent_t;

static uint32_t g_lba;

static void
wb16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v);
	p[1] = (unsigned char)(v >> 8);
}

static void
wb16_both(unsigned char *p, uint16_t v)
{
	wb16(p, v);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)(v);
}

static void
wb32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void
wb32_both(unsigned char *p, uint32_t v)
{
	wb32(p, v);
	p[4] = (unsigned char)(v >> 24);
	p[5] = (unsigned char)(v >> 16);
	p[6] = (unsigned char)(v >> 8);
	p[7] = (unsigned char)v;
}

static void
iso_datetime(unsigned char *p, time_t t)
{
	struct tm tm;
#if defined(_WIN32)
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	p[0] = (unsigned char)tm.tm_year;
	p[1] = (unsigned char)(tm.tm_mon + 1);
	p[2] = (unsigned char)tm.tm_mday;
	p[3] = (unsigned char)tm.tm_hour;
	p[4] = (unsigned char)tm.tm_min;
	p[5] = (unsigned char)tm.tm_sec;
	p[6] = 0;
}

static void
ascii_datetime(char *p17, time_t t)
{
	struct tm tm;
#if defined(_WIN32)
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	snprintf(p17, 18, "%04d%02d%02d%02d%02d%02d00",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void
make_name83(const char *name, int is_dir, char *out, size_t outsz)
{
	char base[9];
	size_t i, n;
	int c;
	const char *dot;

	memset(base, 0, sizeof(base));
	dot = (!is_dir) ? strrchr(name, '.') : NULL;
	if (dot && dot != name && strlen(dot + 1) > 0 && strlen(dot + 1) <= 3) {
		char ext[4];
		memset(ext, 0, sizeof(ext));
		n = (size_t)(dot - name);
		if (n > 8)
			n = 8;
		for (i = 0; i < n; i++) {
			c = (unsigned char)name[i];
			base[i] = isalnum(c) ? (char)toupper(c) : '_';
		}
		n = strlen(dot + 1);
		if (n > 3)
			n = 3;
		for (i = 0; i < n; i++) {
			c = (unsigned char)dot[1 + i];
			ext[i] = isalnum(c) ? (char)toupper(c) : '_';
		}
		snprintf(out, outsz, "%s.%s;1", base, ext);
	} else {
		n = strlen(name);
		if (n > 8)
			n = 8;
		for (i = 0; i < n; i++) {
			c = (unsigned char)name[i];
			base[i] = isalnum(c) ? (char)toupper(c) : '_';
		}
		if (is_dir)
			snprintf(out, outsz, "%s", base);
		else
			snprintf(out, outsz, "%s.;1", base);
	}
}

static int
cmp_kids(const void *a, const void *b)
{
	return strcmp((*(iso_node_t *const *)a)->name83,
	    (*(iso_node_t *const *)b)->name83);
}

static iso_node_t *
node_new(const char *name, const char *full, int is_dir, iso_node_t *parent)
{
	iso_node_t *n = arcsync_xmalloc(sizeof(*n));
	memset(n, 0, sizeof(*n));
	n->name = arcsync_xstrdup(name);
	n->full = arcsync_xstrdup(full);
	n->is_dir = is_dir;
	n->parent = parent;
	make_name83(name, is_dir, n->name83, sizeof(n->name83));
	return n;
}

static void
node_add_kid(iso_node_t *p, iso_node_t *k)
{
	if (p->n_kids + 1 > p->cap_kids) {
		p->cap_kids = p->cap_kids ? p->cap_kids * 2 : 8;
		p->kids = realloc(p->kids, p->cap_kids * sizeof(*p->kids));
		if (!p->kids)
			abort();
	}
	p->kids[p->n_kids++] = k;
}

static int
scan_tree(iso_node_t *dir, int depth)
{
	DIR *d;
	struct dirent *de;
	char path[4096];
	struct stat st;

	if (depth > MAX_DEPTH)
		return 0;
	d = opendir(dir->full);
	if (!d) {
		fprintf(stderr, "arcsync: cannot open %s: %s\n", dir->full, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		iso_node_t *kid;
		if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
		    (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir->full, de->d_name) >= (int)sizeof(path))
			continue;
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			kid = node_new(de->d_name, path, 1, dir);
			node_add_kid(dir, kid);
			if (scan_tree(kid, depth + 1) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			kid = node_new(de->d_name, path, 0, dir);
			kid->size = (uint32_t)st.st_size;
			node_add_kid(dir, kid);
		}
	}
	closedir(d);
	qsort(dir->kids, dir->n_kids, sizeof(*dir->kids), cmp_kids);
	return 0;
}

static void
dedupe83(iso_node_t *dir)
{
	size_t i, j;
	for (i = 0; i < dir->n_kids; i++) {
		for (j = 0; j < i; j++) {
			if (strcmp(dir->kids[i]->name83, dir->kids[j]->name83) == 0) {
				char bump[16];
				snprintf(bump, sizeof(bump), "%07u", (unsigned)i);
				memcpy(dir->kids[i]->name83, bump, 7);
				break;
			}
		}
		if (dir->kids[i]->is_dir)
			dedupe83(dir->kids[i]);
	}
}

static void
utf8_to_ucs2be(const char *utf8, unsigned char *out, size_t *out_units)
{
	const unsigned char *s = (const unsigned char *)utf8;
	size_t n = 0;
	while (*s && n + 1 < 128) {
		unsigned cp;
		if (*s < 0x80) {
			cp = *s++;
		} else if ((*s & 0xE0) == 0xC0 && s[1]) {
			cp = ((*s & 0x1F) << 6) | (s[1] & 0x3F);
			s += 2;
		} else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) {
			cp = ((*s & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
			s += 3;
		} else {
			cp = '?';
			s++;
		}
		if (cp > 0xFFFF)
			cp = '?';
		out[n * 2] = (unsigned char)(cp >> 8);
		out[n * 2 + 1] = (unsigned char)(cp);
		n++;
	}
	*out_units = n;
}

static uint32_t
dirent_len_joliet(const char *utf8)
{
	unsigned char ucs[256];
	size_t nu = 0;
	uint32_t len;
	utf8_to_ucs2be(utf8, ucs, &nu);
	if (nu < 1)
		nu = 1;
	len = 33 + (uint32_t)nu * 2;
	if (len & 1)
		len++;
	return len;
}

static uint32_t
dir_data_size(iso_node_t *dir)
{
	uint32_t sz = 34 + 34;
	size_t i;
	for (i = 0; i < dir->n_kids; i++)
		sz += dirent_len_joliet(dir->kids[i]->name);
	return (sz + SECTOR - 1) / SECTOR * SECTOR;
}

static void
assign_extents(iso_node_t *dir)
{
	size_t i;
	dir->data_len = dir_data_size(dir);
	dir->extent = g_lba;
	g_lba += dir->data_len / SECTOR;
	for (i = 0; i < dir->n_kids; i++) {
		if (dir->kids[i]->is_dir) {
			assign_extents(dir->kids[i]);
		} else {
			uint32_t sectors = (dir->kids[i]->size + SECTOR - 1) / SECTOR;
			dir->kids[i]->extent = g_lba;
			dir->kids[i]->data_len = dir->kids[i]->size;
			g_lba += sectors;
		}
	}
}

static void
write_dirent(unsigned char *buf, size_t *off, uint32_t extent, uint32_t size,
    int is_dir, const unsigned char *name, size_t namelen, time_t t)
{
	uint32_t len = 33 + (uint32_t)namelen;
	unsigned char *p;
	if (len & 1)
		len++;
	p = buf + *off;
	memset(p, 0, len);
	p[0] = (unsigned char)len;
	wb32_both(p + 2, extent);
	wb32_both(p + 10, size);
	iso_datetime(p + 18, t);
	p[25] = is_dir ? 0x02 : 0x00;
	p[32] = (unsigned char)namelen;
	memcpy(p + 33, name, namelen);
	*off += len;
}

static int
write_directory(FILE *fp, iso_node_t *dir, time_t now)
{
	unsigned char *buf = arcsync_xmalloc(dir->data_len);
	size_t off = 0, i;
	unsigned char dot = 0x00, dotdot = 0x01;
	iso_node_t *par;

	memset(buf, 0, dir->data_len);
	write_dirent(buf, &off, dir->extent, dir->data_len, 1, &dot, 1, now);
	par = dir->parent ? dir->parent : dir;
	write_dirent(buf, &off, par->extent, par->data_len, 1, &dotdot, 1, now);
	for (i = 0; i < dir->n_kids; i++) {
		iso_node_t *k = dir->kids[i];
		unsigned char ucs[256];
		size_t nu = 0;
		utf8_to_ucs2be(k->name, ucs, &nu);
		write_dirent(buf, &off, k->extent, k->is_dir ? k->data_len : k->size,
		    k->is_dir, ucs, nu * 2, now);
	}
	if (fseek(fp, (long)dir->extent * SECTOR, SEEK_SET) != 0 ||
	    fwrite(buf, 1, dir->data_len, fp) != dir->data_len) {
		free(buf);
		return -1;
	}
	free(buf);
	for (i = 0; i < dir->n_kids; i++) {
		if (dir->kids[i]->is_dir && write_directory(fp, dir->kids[i], now) != 0)
			return -1;
	}
	return 0;
}

static int
write_file_data(FILE *fp, iso_node_t *dir)
{
	size_t i;
	for (i = 0; i < dir->n_kids; i++) {
		iso_node_t *k = dir->kids[i];
		if (k->is_dir) {
			if (write_file_data(fp, k) != 0)
				return -1;
		} else if (k->size > 0) {
			FILE *in;
			unsigned char buf[SECTOR];
			uint32_t left = k->size;
			if (fseek(fp, (long)k->extent * SECTOR, SEEK_SET) != 0)
				return -1;
			in = fopen(k->full, "rb");
			if (!in) {
				fprintf(stderr, "arcsync: cannot read %s\n", k->full);
				return -1;
			}
			while (left) {
				size_t chunk = left > SECTOR ? SECTOR : left;
				memset(buf, 0, SECTOR);
				if (fread(buf, 1, chunk, in) != chunk) {
					fclose(in);
					return -1;
				}
				if (fwrite(buf, 1, SECTOR, fp) != SECTOR) {
					fclose(in);
					return -1;
				}
				left -= (uint32_t)chunk;
			}
			fclose(in);
		}
	}
	return 0;
}

static void
collect_paths(iso_node_t *dir, path_ent_t *ents, size_t *n, size_t parent_idx)
{
	size_t my = (*n)++;
	size_t i;
	ents[my].node = dir;
	ents[my].parent_idx = (uint16_t)parent_idx;
	for (i = 0; i < dir->n_kids; i++) {
		if (dir->kids[i]->is_dir)
			collect_paths(dir->kids[i], ents, n, my);
	}
}

static uint32_t
path_table_size(path_ent_t *ents, size_t n)
{
	uint32_t sz = 0;
	size_t i;
	for (i = 0; i < n; i++) {
		uint32_t nl;
		unsigned char ucs[256];
		size_t nu = 0;
		if (i == 0)
			nl = 1;
		else {
			utf8_to_ucs2be(ents[i].node->name, ucs, &nu);
			nl = (uint32_t)nu * 2;
		}
		sz += 8 + nl;
		if (nl & 1)
			sz++;
	}
	return sz;
}

static int
write_path_table(FILE *fp, uint32_t lba, path_ent_t *ents, size_t n)
{
	uint32_t sz = path_table_size(ents, n);
	uint32_t padded = (sz + SECTOR - 1) / SECTOR * SECTOR;
	unsigned char *buf = arcsync_xmalloc(padded);
	size_t off = 0, i;
	memset(buf, 0, padded);
	for (i = 0; i < n; i++) {
		unsigned char name[256];
		size_t nl, nu = 0;
		if (i == 0) {
			name[0] = 0;
			nl = 1;
		} else {
			utf8_to_ucs2be(ents[i].node->name, name, &nu);
			nl = nu * 2;
		}
		buf[off++] = (unsigned char)nl;
		buf[off++] = 0;
		wb32(buf + off, ents[i].node->extent);
		off += 4;
		wb16(buf + off, (uint16_t)(ents[i].parent_idx + 1));
		off += 2;
		memcpy(buf + off, name, nl);
		off += nl;
		if (nl & 1)
			buf[off++] = 0;
	}
	if (fseek(fp, (long)lba * SECTOR, SEEK_SET) != 0 ||
	    fwrite(buf, 1, padded, fp) != padded) {
		free(buf);
		return -1;
	}
	free(buf);
	return 0;
}

static void
fill_descriptors(unsigned char *pvd, unsigned char *svd, unsigned char *term,
    const char *vol, iso_node_t *root, uint32_t path_lba, uint32_t path_sz,
    uint32_t volume_space, time_t now)
{
	char dt[18];
	size_t i;
	unsigned char ucs[64];
	size_t nu = 0;
	size_t off;

	memset(pvd, 0, SECTOR);
	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	memset(pvd + 8, ' ', 32);
	memset(pvd + 40, ' ', 32);
	for (i = 0; i < 32 && vol[i]; i++)
		pvd[40 + i] = (unsigned char)toupper((unsigned char)vol[i]);
	wb32_both(pvd + 80, volume_space);
	wb16_both(pvd + 120, 1);
	wb16_both(pvd + 124, 1);
	wb16_both(pvd + 128, 2048);
	wb32_both(pvd + 132, path_sz);
	wb32(pvd + 140, path_lba);
	off = 0;
	{
		unsigned char name = 0;
		write_dirent(pvd + 156, &off, root->extent, root->data_len, 1, &name, 1, now);
	}
	ascii_datetime(dt, now);
	memcpy(pvd + 813, dt, 17);
	pvd[829] = 0;
	memcpy(pvd + 830, dt, 17);
	pvd[846] = 0;
	memset(pvd + 847, '0', 16);
	pvd[863] = 0;
	memcpy(pvd + 864, dt, 17);
	pvd[880] = 0;
	pvd[881] = 1;

	memset(svd, 0, SECTOR);
	svd[0] = 2;
	memcpy(svd + 1, "CD001", 5);
	svd[6] = 1;
	svd[88] = '%';
	svd[89] = '/';
	svd[90] = 'E';
	utf8_to_ucs2be(vol, ucs, &nu);
	memset(svd + 40, 0, 32);
	for (i = 0; i < nu && i < 16; i++) {
		svd[40 + i * 2] = ucs[i * 2];
		svd[41 + i * 2] = ucs[i * 2 + 1];
	}
	wb32_both(svd + 80, volume_space);
	wb16_both(svd + 120, 1);
	wb16_both(svd + 124, 1);
	wb16_both(svd + 128, 2048);
	wb32_both(svd + 132, path_sz);
	wb32(svd + 140, path_lba);
	off = 0;
	{
		unsigned char name = 0;
		write_dirent(svd + 156, &off, root->extent, root->data_len, 1, &name, 1, now);
	}
	memcpy(svd + 813, dt, 17);
	svd[829] = 0;
	memcpy(svd + 830, dt, 17);
	svd[846] = 0;
	memset(svd + 847, '0', 16);
	svd[863] = 0;
	memcpy(svd + 864, dt, 17);
	svd[880] = 0;
	svd[881] = 1;

	memset(term, 0, SECTOR);
	term[0] = 255;
	memcpy(term + 1, "CD001", 5);
	term[6] = 1;
}

static void
free_tree(iso_node_t *n)
{
	size_t i;
	if (!n)
		return;
	for (i = 0; i < n->n_kids; i++)
		free_tree(n->kids[i]);
	free(n->kids);
	free(n->name);
	free(n->full);
	free(n);
}

int
arcsync_write_iso9660(const char *stage_root, const char *out_path,
    const char *volume_name)
{
	iso_node_t *root;
	path_ent_t *paths;
	size_t n_paths = 0;
	uint32_t path_lba, path_sz, path_sectors, volume_space;
	unsigned char pvd[SECTOR], svd[SECTOR], term[SECTOR], z[SECTOR];
	FILE *fp;
	time_t now = time(NULL);
	const char *vol = volume_name && *volume_name ? volume_name : "ARCSYNC";
	size_t i;

	root = node_new("", stage_root, 1, NULL);
	if (scan_tree(root, 0) != 0) {
		free_tree(root);
		return 6;
	}
	dedupe83(root);
	paths = arcsync_xmalloc(8192 * sizeof(*paths));
	collect_paths(root, paths, &n_paths, 0);

	g_lba = 19;
	path_lba = g_lba;
	path_sz = path_table_size(paths, n_paths);
	path_sectors = (path_sz + SECTOR - 1) / SECTOR;
	if (!path_sectors)
		path_sectors = 1;
	g_lba += path_sectors;
	assign_extents(root);
	volume_space = g_lba;

	fp = fopen(out_path, "wb");
	if (!fp) {
		fprintf(stderr, "arcsync: cannot create %s: %s\n", out_path, strerror(errno));
		free(paths);
		free_tree(root);
		return 4;
	}
	if (fseek(fp, (long)volume_space * SECTOR - 1, SEEK_SET) != 0 || fputc(0, fp) == EOF) {
		fclose(fp);
		free(paths);
		free_tree(root);
		return 6;
	}

	memset(z, 0, SECTOR);
	if (fseek(fp, 0, SEEK_SET) != 0)
		goto fail;
	for (i = 0; i < 16; i++) {
		if (fwrite(z, 1, SECTOR, fp) != SECTOR)
			goto fail;
	}

	fill_descriptors(pvd, svd, term, vol, root, path_lba, path_sz, volume_space, now);
	if (fwrite(pvd, 1, SECTOR, fp) != SECTOR ||
	    fwrite(svd, 1, SECTOR, fp) != SECTOR ||
	    fwrite(term, 1, SECTOR, fp) != SECTOR)
		goto fail;
	if (write_path_table(fp, path_lba, paths, n_paths) != 0)
		goto fail;
	if (write_directory(fp, root, now) != 0)
		goto fail;
	if (write_file_data(fp, root) != 0)
		goto fail;

	fclose(fp);
	free(paths);
	free_tree(root);
	return 0;
fail:
	fclose(fp);
	unlink(out_path);
	free(paths);
	free_tree(root);
	return 6;
}
