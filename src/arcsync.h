#ifndef ARCSYNC_H
#define ARCSYNC_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define ARCSYNC_VERSION "1.0.0"
#define ARCSYNC_MAX_SIZE_BYTES (233 * 1024)

typedef enum {
	ARCSYNC_MEDIA_CD = 0,
	ARCSYNC_MEDIA_DVD,
	ARCSYNC_MEDIA_DVD_DL,
	ARCSYNC_MEDIA_BD,
	ARCSYNC_MEDIA_NONE
} arcsync_media_t;

typedef enum {
	ARCSYNC_CLOUD_SKIP = 0,
	ARCSYNC_CLOUD_DERIVATIVE,
	ARCSYNC_CLOUD_FAIL
} arcsync_cloud_t;

typedef enum {
	ARCSYNC_KIND_PHOTO = 0,
	ARCSYNC_KIND_VIDEO,
	ARCSYNC_KIND_OTHER
} arcsync_kind_t;

typedef struct {
	const char *library;
	const char *dir;
	const char *out;
	arcsync_media_t media;
	int split;
	const char *volume_name;
	const char *from_date;
	const char *to_date;
	int include_hidden;
	int include_trashed;
	int prefer_edited;
	arcsync_cloud_t cloud;
	int thumb_size;
	const char *title;
	int dry_run;
	int verbose;
	int quiet;
	int json;
	int force;
	int burn;
	int help;
	int version;
	int demo;
} arcsync_opts_t;

typedef struct arcsync_asset {
	char *id;           /* stable id (path hash or uuid) */
	char *src_path;
	char *orig_name;
	char *album_title;  /* primary album for --dir (subdir name) */
	char *rel_album;    /* album slug path */
	time_t captured;
	uint64_t bytes;
	arcsync_kind_t kind;
	char *dest_rel;     /* media/YYYY/MM/DD/name */
	char *thumb_rel;    /* thumbs/<id>.jpg */
	int missing;
} arcsync_asset_t;

typedef struct arcsync_album {
	char *title;
	char *slug;
	char *parent_slug;
	size_t *asset_idx;  /* indices into catalog assets */
	size_t n_assets;
	size_t cap_assets;
} arcsync_album_t;

typedef struct {
	arcsync_asset_t *assets;
	size_t n_assets;
	size_t cap_assets;
	arcsync_album_t *albums;
	size_t n_albums;
	size_t cap_albums;
	uint64_t total_bytes;
	size_t n_photos;
	size_t n_videos;
	size_t n_missing;
	size_t n_skipped;
} arcsync_catalog_t;

typedef struct {
	char *stage_root;   /* .../stage */
	char *work_root;    /* $TMPDIR/arcsync-<pid> */
} arcsync_stage_t;

void arcsync_banner(void);
int arcsync_parse_args(int argc, char **argv, arcsync_opts_t *opts);
void arcsync_print_help(void);
void arcsync_opts_init(arcsync_opts_t *opts);

/* util */
void *arcsync_xmalloc(size_t n);
char *arcsync_xstrdup(const char *s);
char *arcsync_aprintf(const char *fmt, ...);
int arcsync_mkdir_p(const char *path);
int arcsync_path_join(char *out, size_t outsz, const char *a, const char *b);
void arcsync_html_escape(FILE *fp, const char *s);
void arcsync_slugify(const char *in, char *out, size_t outsz);
uint64_t arcsync_media_budget(arcsync_media_t m);
const char *arcsync_media_name(arcsync_media_t m);
int arcsync_parse_ymd(const char *s, struct tm *out);
int arcsync_date_in_range(time_t t, const char *from, const char *to);
char *arcsync_sha12_hex(const char *s);
void arcsync_rm_rf(const char *path);

void arcsync_catalog_init(arcsync_catalog_t *c);
void arcsync_catalog_free(arcsync_catalog_t *c);
arcsync_asset_t *arcsync_catalog_add_asset(arcsync_catalog_t *c);
arcsync_album_t *arcsync_catalog_find_or_add_album(arcsync_catalog_t *c,
    const char *title, const char *slug, const char *parent_slug);
void arcsync_album_add_asset(arcsync_album_t *a, size_t asset_index);

/* fs_dir */
int arcsync_scan_dir(const char *root, const arcsync_opts_t *opts,
    arcsync_catalog_t *cat);
int arcsync_scan_photos(const char *library, const arcsync_opts_t *opts,
    arcsync_catalog_t *cat);

/* assign dest paths */
int arcsync_assign_dests(arcsync_catalog_t *cat);

/* copy + thumbs + html + iso */
int arcsync_stage_begin(arcsync_stage_t *st);
void arcsync_stage_end(arcsync_stage_t *st, int keep);
int arcsync_copy_assets(const arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts);
int arcsync_make_thumbs(arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts);
int arcsync_write_html(const arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts, int disc_n, int disc_m, uint64_t vol_bytes);
int arcsync_write_sidecar(const arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts);
int arcsync_make_iso(const arcsync_stage_t *st, const arcsync_opts_t *opts,
    const char *out_path);
int arcsync_burn_iso(const char *iso_path, const arcsync_opts_t *opts);
int arcsync_run_pipeline(arcsync_opts_t *opts);
int arcsync_demo(void);

#endif
