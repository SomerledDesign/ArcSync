#include "arcsync.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
		if (!(stat(full, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0)) {
			missing_selected++;
			cat->n_missing++;
			continue;
		}

		a = arcsync_catalog_add_asset(cat);
		a->id = arcsync_xstrdup(uuid ? uuid : "unknown");
		a->src_path = arcsync_xstrdup(full);
		a->orig_name = arcsync_xstrdup(file);
		a->captured = captured;
		a->bytes = (uint64_t)sb.st_size;
		a->kind = kind;
		a->thumb_rel = arcsync_aprintf("thumbs/%s.jpg", a->id);
		a->album_title = arcsync_xstrdup("Library");
		a->rel_album = arcsync_xstrdup("library");
		cat->total_bytes += a->bytes;
		if (kind == ARCSYNC_KIND_PHOTO)
			cat->n_photos++;
		else
			cat->n_videos++;
		alb = arcsync_catalog_find_or_add_album(cat, "Library", "library", NULL);
		arcsync_album_add_asset(alb, cat->n_assets - 1);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	arcsync_rm_rf(tmpdir);

	if (opts->cloud == ARCSYNC_CLOUD_FAIL && missing_selected > 0) {
		fprintf(stderr, "arcsync: --cloud fail: %zu assets not local originals. "
		    "Photos → Settings → iCloud → Download Originals, then retry.\n",
		    missing_selected);
		return 8;
	}
	if (!opts->quiet && cat->n_missing > 0)
		fprintf(stderr, "arcsync: missing  %zu not on disk (see --cloud)\n",
		    cat->n_missing);
	(void)rc;
	return 0;
}
