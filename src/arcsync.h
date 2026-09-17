#ifndef ARCSYNC_H
#define ARCSYNC_H

#include <stddef.h>
#include <stdint.h>

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
} arcsync_opts_t;

void arcsync_banner(void);
int arcsync_parse_args(int argc, char **argv, arcsync_opts_t *opts);
void arcsync_print_help(void);
void arcsync_opts_init(arcsync_opts_t *opts);

#endif
