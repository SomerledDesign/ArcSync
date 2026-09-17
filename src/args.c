#include "arcsync.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
default_out_path(char *buf, size_t buflen)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(buf, buflen, "./arcsync-%04d%02d%02d.iso",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static void
default_volume_name(char *buf, size_t buflen)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(buf, buflen, "ARCSYNC_%04d%02d%02d",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

void
arcsync_opts_init(arcsync_opts_t *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->media = ARCSYNC_MEDIA_DVD;
	opts->cloud = ARCSYNC_CLOUD_SKIP;
	opts->thumb_size = 240;
	opts->prefer_edited = 0;
}

static arcsync_media_t
parse_media(const char *s)
{
	if (strcmp(s, "cd") == 0)
		return ARCSYNC_MEDIA_CD;
	if (strcmp(s, "dvd") == 0)
		return ARCSYNC_MEDIA_DVD;
	if (strcmp(s, "dvd-dl") == 0)
		return ARCSYNC_MEDIA_DVD_DL;
	if (strcmp(s, "bd") == 0)
		return ARCSYNC_MEDIA_BD;
	if (strcmp(s, "none") == 0)
		return ARCSYNC_MEDIA_NONE;
	return (arcsync_media_t)-1;
}

static arcsync_cloud_t
parse_cloud(const char *s)
{
	if (strcmp(s, "skip") == 0)
		return ARCSYNC_CLOUD_SKIP;
	if (strcmp(s, "derivative") == 0)
		return ARCSYNC_CLOUD_DERIVATIVE;
	if (strcmp(s, "fail") == 0)
		return ARCSYNC_CLOUD_FAIL;
	return (arcsync_cloud_t)-1;
}

void
arcsync_print_help(void)
{
	fputs(
"arcsync 1.0.0 — archive a photo library to a hybrid CD/DVD ISO\n"
"\n"
"usage: arcsync [options]\n"
"\n"
"  --library PATH     Photos library (.photoslibrary)\n"
"  --dir PATH         plain photo folder instead of Photos\n"
"  --out PATH         output .iso (default ./arcsync-YYYYMMDD.iso)\n"
"  --media TYPE       cd | dvd | dvd-dl | bd | none   (default dvd)\n"
"  --split            emit multiple volumes if needed\n"
"  --burn             after writing ISO, burn with hdiutil (optical drive)\n"
"  --volume-name N    disc label\n"
"  --from DATE        include capture dates on/after YYYY-MM-DD\n"
"  --to DATE          include capture dates on/before YYYY-MM-DD\n"
"  --thumb-size PX    thumbnail long edge (default 240)\n"
"  --title TEXT       gallery title\n"
"  --edited           copy edited derivatives when present\n"
"  --cloud MODE       skip | derivative | fail   (default skip)\n"
"  --include-hidden\n"
"  --include-trashed\n"
"  -n, --dry-run\n"
"  -v, --json, -q, --force\n"
"  -h, --version\n"
"\n"
"See arcsync(1).\n",
	    stdout);
}

int
arcsync_parse_args(int argc, char **argv, arcsync_opts_t *opts)
{
	static char out_buf[512];
	static char vol_buf[64];
	int c;

	static struct option longopts[] = {
		{ "library",         required_argument, NULL, 1000 },
		{ "dir",             required_argument, NULL, 1001 },
		{ "out",             required_argument, NULL, 1002 },
		{ "media",           required_argument, NULL, 1003 },
		{ "split",           no_argument,       NULL, 1004 },
		{ "volume-name",     required_argument, NULL, 1005 },
		{ "from",            required_argument, NULL, 1006 },
		{ "to",              required_argument, NULL, 1007 },
		{ "include-hidden",  no_argument,       NULL, 1008 },
		{ "include-trashed", no_argument,       NULL, 1009 },
		{ "edited",          no_argument,       NULL, 1010 },
		{ "originals",       no_argument,       NULL, 1011 },
		{ "cloud",           required_argument, NULL, 1012 },
		{ "thumb-size",      required_argument, NULL, 1013 },
		{ "title",           required_argument, NULL, 1014 },
		{ "dry-run",         no_argument,       NULL, 'n' },
		{ "json",            no_argument,       NULL, 1015 },
		{ "force",           no_argument,       NULL, 1016 },
		{ "burn",            no_argument,       NULL, 1017 },
		{ "help",            no_argument,       NULL, 'h' },
		{ "version",         no_argument,       NULL, 1018 },
		{ NULL, 0, NULL, 0 }
	};

	arcsync_opts_init(opts);

	while ((c = getopt_long(argc, argv, "nvqh", longopts, NULL)) != -1) {
		switch (c) {
		case 1000: opts->library = optarg; break;
		case 1001: opts->dir = optarg; break;
		case 1002: opts->out = optarg; break;
		case 1003: {
			arcsync_media_t m = parse_media(optarg);
			if ((int)m < 0) {
				fprintf(stderr, "arcsync: unknown --media %s\n", optarg);
				return 1;
			}
			opts->media = m;
			break;
		}
		case 1004: opts->split = 1; break;
		case 1005: opts->volume_name = optarg; break;
		case 1006: opts->from_date = optarg; break;
		case 1007: opts->to_date = optarg; break;
		case 1008: opts->include_hidden = 1; break;
		case 1009: opts->include_trashed = 1; break;
		case 1010: opts->prefer_edited = 1; break;
		case 1011: opts->prefer_edited = 0; break;
		case 1012: {
			arcsync_cloud_t cl = parse_cloud(optarg);
			if ((int)cl < 0) {
				fprintf(stderr, "arcsync: unknown --cloud %s\n", optarg);
				return 1;
			}
			opts->cloud = cl;
			break;
		}
		case 1013: {
			int px = atoi(optarg);
			if (px < 96 || px > 640) {
				fprintf(stderr, "arcsync: --thumb-size must be 96-640\n");
				return 1;
			}
			opts->thumb_size = px;
			break;
		}
		case 1014: opts->title = optarg; break;
		case 1015: opts->json = 1; break;
		case 1016: opts->force = 1; break;
		case 1017: opts->burn = 1; break;
		case 1018: opts->version = 1; break;
		case 'n': opts->dry_run = 1; break;
		case 'v': opts->verbose = 1; break;
		case 'q': opts->quiet = 1; break;
		case 'h': opts->help = 1; break;
		default:
			return 1;
		}
	}

	if (opts->help || opts->version)
		return 0;

	if (opts->library && opts->dir) {
		fprintf(stderr, "arcsync: --library and --dir are mutually exclusive\n");
		return 1;
	}

	if (!opts->out) {
		default_out_path(out_buf, sizeof(out_buf));
		opts->out = out_buf;
	}
	if (!opts->volume_name) {
		default_volume_name(vol_buf, sizeof(vol_buf));
		opts->volume_name = vol_buf;
	}
	if (!opts->title)
		opts->title = opts->volume_name;

	return 0;
}
