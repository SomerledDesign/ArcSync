#include "arcsync.h"

#ifdef ARCSYNC_NO_SQLITE

#include <stdio.h>

int
arcsync_scan_photos(const char *library, const arcsync_opts_t *opts,
    arcsync_catalog_t *cat)
{
	(void)library;
	(void)opts;
	(void)cat;
	fprintf(stderr, "arcsync: Photos library mode not built (use --dir)\n");
	return 2;
}

#else
#include "arcsync.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define CD_EPOCH_OFFSET 978307200.0

static int
copy_file(const char *from, const char *to)
{
	FILE *in, *out;
	char buf[1 << 16];
	size_t n;
	in = fopen(from, "rb");
	if (!in)
		return -1;
	out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -1;
		}
	}
	fclose(in);
	fclose(out);
	return 0;
}

static int
copy_db_bundle(const char *lib, const char *tmpdir, char *out_db, size_t out_sz)
{
	char src[4096], dst[4096];
	snprintf(src, sizeof(src), "%s/database/Photos.sqlite", lib);
	snprintf(dst, sizeof(dst), "%s/Photos.sqlite", tmpdir);
	if (copy_file(src, dst) != 0) {
		fprintf(stderr, "arcsync: cannot copy Photos.sqlite (close Photos.app? "
		    "need Full Disk Access for Terminal)\n");
		return 2;
	}
	snprintf(src, sizeof(src), "%s/database/Photos.sqlite-wal", lib);
	snprintf(dst, sizeof(dst), "%s/Photos.sqlite-wal", tmpdir);
	if (access(src, R_OK) == 0)
		(void)copy_file(src, dst);
	snprintf(src, sizeof(src), "%s/database/Photos.sqlite-shm", lib);
	snprintf(dst, sizeof(dst), "%s/Photos.sqlite-shm", tmpdir);
	if (access(src, R_OK) == 0)
		(void)copy_file(src, dst);
	snprintf(out_db, out_sz, "%s/Photos.sqlite", tmpdir);
	return 0;
}

static int
table_exists(sqlite3 *db, const char *name)
{
	sqlite3_stmt *st = NULL;
	int ok = 0;
	if (sqlite3_prepare_v2(db,
	    "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
	    -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW)
		ok = 1;
	sqlite3_finalize(st);
	return ok;
}

static int
column_exists(sqlite3 *db, const char *table, const char *col)
{
	char sql[256];
	sqlite3_stmt *st = NULL;
	int ok = 0;
	snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *n = (const char *)sqlite3_column_text(st, 1);
		if (n && strcmp(n, col) == 0) {
			ok = 1;
			break;
		}
	}
	sqlite3_finalize(st);
	return ok;
}

/* Minimum size to reject tiny icon derivatives. */
#define DERIV_MIN_BYTES 8192

static int
deriv_ext_ok(const char *name)
{
	const char *dot = strrchr(name, '.');
	char ext[8];
	size_t i, n;
	if (!dot || !dot[1])
		return 0;
	n = strlen(dot + 1);
	if (n >= sizeof(ext))
		return 0;
	for (i = 0; i < n; i++) {
		char c = dot[1 + i];
		ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	ext[n] = '\0';
	return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "heic") ||
	    !strcmp(ext, "heif") || !strcmp(ext, "png") || !strcmp(ext, "mov") ||
	    !strcmp(ext, "mp4") || !strcmp(ext, "m4v");
}

/* Pick largest suitable media file under dir (one level). */
static int
best_in_dir(const char *dir, char *out, size_t outsz, off_t *best_sz)
{
	DIR *d;
	struct dirent *ent;
	off_t best = *best_sz;
	int found = 0;
	char path[4096];

	d = opendir(dir);
	if (!d)
		return 0;
	while ((ent = readdir(d)) != NULL) {
		struct stat sb;
		if (ent->d_name[0] == '.')
			continue;
		if (!deriv_ext_ok(ent->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode))
			continue;
		if (sb.st_size < DERIV_MIN_BYTES)
			continue;
		if (sb.st_size > best) {
			best = sb.st_size;
			snprintf(out, outsz, "%s", path);
			found = 1;
		}
	}
	closedir(d);
	if (found)
		*best_sz = best;
	return found;
}

/*
 * Locate a usable local Photos derivative/preview for uuid.
 * Layouts vary by Photos version; try a small set of known roots.
 */
static int
find_derivative(const char *library, const char *uuid, char *out, size_t outsz,
    off_t *out_sz)
{
	char nodash[80], bases[8][4096];
	size_t i, nbase = 0;
	off_t best = 0;
	char bestpath[4096];
	int any = 0;

	if (!uuid || !*uuid)
		return 0;
	bestpath[0] = '\0';

	/* UUID without dashes (lowercase) */
	{
		size_t j = 0;
		const char *p;
		for (p = uuid; *p && j + 1 < sizeof(nodash); p++) {
			if (*p != '-')
				nodash[j++] = (*p >= 'A' && *p <= 'Z') ?
				    (char)(*p - 'A' + 'a') : *p;
		}
		nodash[j] = '\0';
	}

	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/derivatives/%s", library, uuid);
	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/derivatives/%s", library, nodash);
	if (nodash[0]) {
		char hex = nodash[0];
		snprintf(bases[nbase++], sizeof(bases[0]),
		    "%s/resources/derivatives/%c/%s", library, hex, nodash);
		snprintf(bases[nbase++], sizeof(bases[0]),
		    "%s/resources/derivatives/%c/%s", library, hex, uuid);
	}
	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/derivatives/masters/%s", library, uuid);
	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/derivatives/masters/%s", library, nodash);
	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/renders/%s", library, uuid);
	snprintf(bases[nbase++], sizeof(bases[0]),
	    "%s/resources/renders/%s", library, nodash);

	for (i = 0; i < nbase; i++) {
		struct stat sb;
		char cand[4096];
		if (stat(bases[i], &sb) != 0)
			continue;
		if (S_ISREG(sb.st_mode) && sb.st_size >= DERIV_MIN_BYTES &&
		    deriv_ext_ok(bases[i])) {
			if (sb.st_size > best) {
				best = sb.st_size;
				snprintf(bestpath, sizeof(bestpath), "%s", bases[i]);
				any = 1;
			}
			continue;
		}
		if (S_ISDIR(sb.st_mode)) {
			cand[0] = '\0';
			if (best_in_dir(bases[i], cand, sizeof(cand), &best) && cand[0]) {
				snprintf(bestpath, sizeof(bestpath), "%s", cand);
				any = 1;
			}
		}
	}
	if (!any)
		return 0;
	snprintf(out, outsz, "%s", bestpath);
	*out_sz = best;
	return 1;
}


int
arcsync_scan_photos(const char *library, const arcsync_opts_t *opts, arcsync_catalog_t *cat)
{
	char tmpdir[512], dbpath[512];
	char asset_table[64];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	const char *tmp = getenv("TMPDIR");
	int rc;
	char sql[1024];
	size_t missing_selected = 0;
	size_t local_selected = 0;

	if (!tmp || !*tmp)
		tmp = "/tmp";
	snprintf(tmpdir, sizeof(tmpdir), "%s/arcsync-db-%d", tmp, (int)getpid());
	arcsync_rm_rf(tmpdir);
	if (arcsync_mkdir_p(tmpdir) != 0)
		return 4;

	rc = copy_db_bundle(library, tmpdir, dbpath, sizeof(dbpath));
	if (rc != 0) {
		arcsync_rm_rf(tmpdir);
		return rc;
	}

	if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "arcsync: sqlite open failed: %s\n",
		    db ? sqlite3_errmsg(db) : "unknown");
		if (db)
			sqlite3_close(db);
		arcsync_rm_rf(tmpdir);
		return 2;
	}

	if (table_exists(db, "ZASSET"))
		snprintf(asset_table, sizeof(asset_table), "ZASSET");
	else if (table_exists(db, "ZGENERICASSET"))
		snprintf(asset_table, sizeof(asset_table), "ZGENERICASSET");
	else {
		fprintf(stderr, "arcsync: unrecognized Photos schema (no ZASSET)\n");
		sqlite3_close(db);
		arcsync_rm_rf(tmpdir);
		return 2;
	}

	if (!column_exists(db, asset_table, "ZDIRECTORY") ||
	    !column_exists(db, asset_table, "ZFILENAME") ||
	    !column_exists(db, asset_table, "ZDATECREATED")) {
		fprintf(stderr, "arcsync: Photos schema missing expected columns on %s\n",
		    asset_table);
		sqlite3_close(db);
		arcsync_rm_rf(tmpdir);
		return 2;
	}

	{
		int has_trash = column_exists(db, asset_table, "ZTRASHEDSTATE");
		int has_hidden = column_exists(db, asset_table, "ZHIDDEN");
		int has_kind = column_exists(db, asset_table, "ZKIND");
		int has_uuid = column_exists(db, asset_table, "ZUUID");
		snprintf(sql, sizeof(sql),
		    "SELECT A.Z_PK, %s, A.ZDIRECTORY, A.ZFILENAME, A.ZDATECREATED, "
		    "%s, %s, %s FROM %s A",
		    has_uuid ? "A.ZUUID" : "CAST(A.Z_PK AS TEXT)",
		    has_trash ? "A.ZTRASHEDSTATE" : "0",
		    has_hidden ? "A.ZHIDDEN" : "0",
		    has_kind ? "A.ZKIND" : "0",
		    asset_table);
	}

	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "arcsync: asset query failed: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		arcsync_rm_rf(tmpdir);
		return 2;
	}

	while (sqlite3_step(st) == SQLITE_ROW) {
		int trashed = sqlite3_column_int(st, 5);
		int hidden = sqlite3_column_int(st, 6);
		int kind_i = sqlite3_column_int(st, 7);
		const char *uuid = (const char *)sqlite3_column_text(st, 1);
		const char *dir = (const char *)sqlite3_column_text(st, 2);
		const char *file = (const char *)sqlite3_column_text(st, 3);
		double zdate = sqlite3_column_double(st, 4);
		time_t captured;
		char full[4096];
		struct stat sb;
		arcsync_asset_t *a;
		arcsync_kind_t kind;
		arcsync_album_t *alb;

		if (!opts->include_trashed && trashed)
			continue;
		if (!opts->include_hidden && hidden)
			continue;
		if (!dir || !file)
			continue;
		captured = (time_t)(zdate + CD_EPOCH_OFFSET);
		if (!arcsync_date_in_range(captured, opts->from_date, opts->to_date))
			continue;

		kind = (kind_i == 1) ? ARCSYNC_KIND_VIDEO : ARCSYNC_KIND_PHOTO;
		snprintf(full, sizeof(full), "%s/originals/%s/%s", library, dir, file);
		{
			int is_deriv = 0;
			char deriv[4096];
			off_t dsz = 0;

			if (!(stat(full, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0)) {
				missing_selected++;
				if (opts->cloud == ARCSYNC_CLOUD_DERIVATIVE &&
				    uuid && find_derivative(library, uuid, deriv, sizeof(deriv), &dsz)) {
					snprintf(full, sizeof(full), "%s", deriv);
					sb.st_size = dsz;
					is_deriv = 1;
				} else {
					cat->n_missing++;
					continue;
				}
			}

			a = arcsync_catalog_add_asset(cat);
			a->id = arcsync_xstrdup(uuid ? uuid : "unknown");
			a->src_path = arcsync_xstrdup(full);
			a->orig_name = arcsync_xstrdup(file);
			a->captured = captured;
			a->bytes = (uint64_t)sb.st_size;
			a->kind = kind;
			a->derivative = is_deriv;
			a->thumb_rel = arcsync_aprintf("thumbs/%s.jpg", a->id);
			a->album_title = arcsync_xstrdup("Library");
			a->rel_album = arcsync_xstrdup("library");
			cat->total_bytes += a->bytes;
			if (kind == ARCSYNC_KIND_PHOTO)
				cat->n_photos++;
			else
				cat->n_videos++;
			if (is_deriv)
				cat->n_derivative++;
			alb = arcsync_catalog_find_or_add_album(cat, "Library", "library", NULL);
			arcsync_album_add_asset(alb, cat->n_assets - 1);
			if (!is_deriv)
				local_selected++;
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	arcsync_rm_rf(tmpdir);

	if (opts->cloud == ARCSYNC_CLOUD_FAIL && missing_selected > 0) {
		fprintf(stderr,
		    "arcsync: refusing to write an ISO (cloud mode is fail — the safe default).\n"
		    "  %zu photo/video originals are on this Mac\n"
		    "  %zu are not on this Mac (iCloud / Optimize Mac Storage)\n"
		    "\n"
		    "A disc of only the local set would silently omit family pictures still\n"
		    "in the cloud. Download them first:\n"
		    "  Photos → Settings → iCloud → Download Originals to this Mac\n"
		    "Leave the Mac awake on power overnight, then check:\n"
		    "  arcsync -n\n"
		    "When that dry-run exits 0, run your archive command again.\n"
		    "\n"
		    "To knowingly archive only what is already local:\n"
		    "  arcsync --cloud skip ...\n"
		    "To fill holes with local previews (slideshow, not archive):\n"
		    "  arcsync --cloud derivative ...\n",
		    local_selected, missing_selected);
		return 8;
	}
	if (!opts->quiet && cat->n_derivative > 0)
		fprintf(stderr,
		    "arcsync: warning  %zu files are optimized previews, not camera originals\n",
		    cat->n_derivative);
	if (!opts->quiet && cat->n_missing > 0)
		fprintf(stderr, "arcsync: missing  %zu not on disk (see --cloud)\n",
		    cat->n_missing);
	if (!opts->quiet && opts->cloud == ARCSYNC_CLOUD_DERIVATIVE)
		fprintf(stderr,
		    "arcsync: cloud    %zu local originals, %zu derivatives, %zu still missing\n",
		    local_selected, cat->n_derivative, cat->n_missing);
	(void)rc;
	return 0;
}

#endif /* !ARCSYNC_NO_SQLITE */
