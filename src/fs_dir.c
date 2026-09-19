#include "arcsync.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

static int
is_dot(const char *n)
{
	return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}

static arcsync_kind_t
kind_from_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	char ext[16];
	size_t i, n;
	if (!dot || !dot[1])
		return ARCSYNC_KIND_OTHER;
	n = strlen(dot + 1);
	if (n >= sizeof(ext))
		return ARCSYNC_KIND_OTHER;
	for (i = 0; i < n; i++) {
		char c = dot[1 + i];
		ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	ext[n] = '\0';
	if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png") ||
	    !strcmp(ext, "gif") || !strcmp(ext, "tif") || !strcmp(ext, "tiff") ||
	    !strcmp(ext, "heic") || !strcmp(ext, "heif") || !strcmp(ext, "raw") ||
	    !strcmp(ext, "dng") || !strcmp(ext, "cr2") || !strcmp(ext, "cr3") ||
	    !strcmp(ext, "nef") || !strcmp(ext, "arw") || !strcmp(ext, "orf") ||
	    !strcmp(ext, "rw2") || !strcmp(ext, "webp") || !strcmp(ext, "bmp"))
		return ARCSYNC_KIND_PHOTO;
	if (!strcmp(ext, "mov") || !strcmp(ext, "mp4") || !strcmp(ext, "m4v") ||
	    !strcmp(ext, "mts") || !strcmp(ext, "m2ts") || !strcmp(ext, "avi") ||
	    !strcmp(ext, "mkv") || !strcmp(ext, "3gp") || !strcmp(ext, "hevc"))
		return ARCSYNC_KIND_VIDEO;
	return ARCSYNC_KIND_OTHER;
}

static time_t
fs_fallback_time(const struct stat *st)
{
	time_t t = st->st_mtime;
#if defined(__APPLE__)
	if (st->st_birthtimespec.tv_sec > 0)
		t = st->st_birthtimespec.tv_sec;
#endif
	return t;
}

static int
walk(const char *root, const char *rel_dir, const char *album_title,
    const char *album_slug, const arcsync_opts_t *opts, arcsync_catalog_t *cat)
{
	char dirpath[PATH_MAX];
	DIR *d;
	struct dirent *ent;

	if (arcsync_path_join(dirpath, sizeof(dirpath), root, rel_dir) != 0)
		return 4;
	d = opendir(dirpath);
	if (!d) {
		fprintf(stderr, "arcsync: cannot open %s\n", dirpath);
		return 4;
	}
	while ((ent = readdir(d)) != NULL) {
		char full[PATH_MAX], rel[PATH_MAX];
		struct stat st;
		arcsync_kind_t kind;
		if (is_dot(ent->d_name))
			continue;
		if (rel_dir && rel_dir[0]) {
			if (arcsync_path_join(rel, sizeof(rel), rel_dir, ent->d_name) != 0) {
				closedir(d);
				return 4;
			}
		} else {
			snprintf(rel, sizeof(rel), "%s", ent->d_name);
		}
		if (arcsync_path_join(full, sizeof(full), root, rel) != 0) {
			closedir(d);
			return 4;
		}
		if (lstat(full, &st) != 0)
			continue;
		if (S_ISLNK(st.st_mode))
			continue; /* do not follow symlinks out */
		if (S_ISDIR(st.st_mode)) {
			char child_slug[128], child_title[256];
			char slug_in[256];
			snprintf(child_title, sizeof(child_title), "%s", ent->d_name);
			snprintf(slug_in, sizeof(slug_in), "%s", ent->d_name);
			arcsync_slugify(slug_in, child_slug, sizeof(child_slug));
			if (album_slug && album_slug[0]) {
				char combined[256];
				snprintf(combined, sizeof(combined), "%s-%s", album_slug, child_slug);
				snprintf(child_slug, sizeof(child_slug), "%s", combined);
			}
			arcsync_catalog_find_or_add_album(cat, child_title, child_slug,
			    album_slug && album_slug[0] ? album_slug : NULL);
			{
				int rc = walk(root, rel, child_title, child_slug, opts, cat);
				if (rc != 0) {
					closedir(d);
					return rc;
				}
			}
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		kind = kind_from_ext(ent->d_name);
		if (kind == ARCSYNC_KIND_OTHER) {
			cat->n_skipped++;
			continue;
		}
		{
			time_t captured = arcsync_file_captured(full, fs_fallback_time(&st));
			arcsync_asset_t *a;
			arcsync_album_t *alb;
			size_t idx;
			const char *atitle = album_title;
			const char *aslug = album_slug;
			char unsorted_slug[32];
			if (!arcsync_date_in_range(captured, opts->from_date, opts->to_date))
				continue;
			if (!atitle || !atitle[0]) {
				atitle = "Unsorted";
				arcsync_slugify(atitle, unsorted_slug, sizeof(unsorted_slug));
				aslug = unsorted_slug;
			}
			a = arcsync_catalog_add_asset(cat);
			idx = cat->n_assets - 1;
			a->src_path = arcsync_xstrdup(full);
			a->orig_name = arcsync_xstrdup(ent->d_name);
			a->album_title = arcsync_xstrdup(atitle);
			a->rel_album = arcsync_xstrdup(aslug);
			a->captured = captured;
			a->bytes = (uint64_t)st.st_size;
			a->kind = kind;
			a->id = arcsync_sha12_hex(full);
			a->thumb_rel = arcsync_aprintf("thumbs/%s.jpg", a->id);
			alb = arcsync_catalog_find_or_add_album(cat, atitle, aslug, NULL);
			arcsync_album_add_asset(alb, idx);
			cat->total_bytes += a->bytes;
			if (kind == ARCSYNC_KIND_PHOTO)
				cat->n_photos++;
			else
				cat->n_videos++;
		}
	}
	closedir(d);
	return 0;
}

int
arcsync_scan_dir(const char *root, const arcsync_opts_t *opts, arcsync_catalog_t *cat)
{
	return walk(root, "", "", "", opts, cat);
}

int
arcsync_assign_dests(arcsync_catalog_t *cat)
{
	size_t i;
	for (i = 0; i < cat->n_assets; i++) {
		arcsync_asset_t *a = &cat->assets[i];
		struct tm tm;
		char daydir[64], candidate[512], base[256], ext[64];
		char *dot;
		int n = 1;
		localtime_r(&a->captured, &tm);
		snprintf(daydir, sizeof(daydir), "media/%04d/%02d/%02d",
		    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
		snprintf(base, sizeof(base), "%s", a->orig_name ? a->orig_name : "file");
		dot = strrchr(base, '.');
		if (dot) {
			snprintf(ext, sizeof(ext), "%s", dot);
			*dot = '\0';
		} else {
			ext[0] = '\0';
		}
		for (;;) {
			size_t j;
			int clash = 0;
			if (n == 1)
				snprintf(candidate, sizeof(candidate), "%s/%s%s", daydir, base, ext);
			else
				snprintf(candidate, sizeof(candidate), "%s/%s-%d%s", daydir, base, n, ext);
			for (j = 0; j < i; j++) {
				if (cat->assets[j].dest_rel &&
				    strcmp(cat->assets[j].dest_rel, candidate) == 0) {
					clash = 1;
					break;
				}
			}
			if (!clash)
				break;
			n++;
		}
		a->dest_rel = arcsync_xstrdup(candidate);
	}
	return 0;
}
