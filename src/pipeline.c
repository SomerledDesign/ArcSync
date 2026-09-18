#include "arcsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *
out_path_for_volume(const arcsync_opts_t *opts, int vol, int nvols)
{
	if (nvols <= 1)
		return arcsync_xstrdup(opts->out);
	{
		const char *out = opts->out;
		const char *dot = strrchr(out, '.');
		if (dot && strcasecmp(dot, ".iso") == 0) {
			size_t n = (size_t)(dot - out);
			char *stem = arcsync_xmalloc(n + 1);
			memcpy(stem, out, n);
			stem[n] = '\0';
			{
				char *p = arcsync_aprintf("%s-%d.iso", stem, vol);
				free(stem);
				return p;
			}
		}
		return arcsync_aprintf("%s-%d.iso", out, vol);
	}
}

static int
volume_count(const arcsync_catalog_t *cat, uint64_t budget, uint64_t *largest)
{
	uint64_t used = 0;
	int vols = 1;
	size_t i;
	*largest = 0;
	if (budget == UINT64_MAX)
		return 1;
	for (i = 0; i < cat->n_assets; i++) {
		uint64_t b = cat->assets[i].bytes;
		if (b > *largest)
			*largest = b;
		if (b > budget)
			return -1; /* single file too big */
		if (used + b > budget) {
			vols++;
			used = 0;
		}
		used += b;
	}
	return vols;
}

/* Build a sub-catalog for assets [start, end) by index list — simple: filter by assigned volume */
typedef struct {
	size_t *idx;
	size_t n;
	uint64_t bytes;
} vol_plan_t;

static int
plan_volumes(const arcsync_catalog_t *cat, uint64_t budget, vol_plan_t **out_plans, int *out_n)
{
	int nvols, v;
	size_t i;
	uint64_t used = 0;
	vol_plan_t *plans;
	int cur = 0;

	if (budget == UINT64_MAX) {
		plans = arcsync_xmalloc(sizeof(*plans));
		plans[0].idx = arcsync_xmalloc(cat->n_assets * sizeof(size_t));
		plans[0].n = cat->n_assets;
		plans[0].bytes = cat->total_bytes;
		for (i = 0; i < cat->n_assets; i++)
			plans[0].idx[i] = i;
		*out_plans = plans;
		*out_n = 1;
		return 0;
	}

	nvols = volume_count(cat, budget, &used);
	if (nvols < 0)
		return 5;
	plans = arcsync_xmalloc((size_t)nvols * sizeof(*plans));
	memset(plans, 0, (size_t)nvols * sizeof(*plans));
	for (v = 0; v < nvols; v++) {
		plans[v].idx = arcsync_xmalloc(cat->n_assets * sizeof(size_t));
		plans[v].n = 0;
		plans[v].bytes = 0;
	}
	used = 0;
	cur = 0;
	/* oldest-first (assets roughly in walk order; sort by captured) */
	{
		size_t *order = arcsync_xmalloc(cat->n_assets * sizeof(size_t));
		for (i = 0; i < cat->n_assets; i++)
			order[i] = i;
		/* simple insertion sort by captured */
		for (i = 1; i < cat->n_assets; i++) {
			size_t key = order[i];
			size_t j = i;
			while (j > 0 && cat->assets[order[j - 1]].captured > cat->assets[key].captured) {
				order[j] = order[j - 1];
				j--;
			}
			order[j] = key;
		}
		for (i = 0; i < cat->n_assets; i++) {
			size_t ai = order[i];
			uint64_t b = cat->assets[ai].bytes;
			if (used + b > budget && plans[cur].n > 0) {
				cur++;
				used = 0;
				if (cur >= nvols) {
					free(order);
					return 5;
				}
			}
			plans[cur].idx[plans[cur].n++] = ai;
			plans[cur].bytes += b;
			used += b;
		}
		free(order);
	}
	*out_plans = plans;
	*out_n = nvols;
	return 0;
}

static void
catalog_from_plan(const arcsync_catalog_t *src, const vol_plan_t *plan, arcsync_catalog_t *dst)
{
	size_t i, j;
	arcsync_catalog_init(dst);
	for (i = 0; i < plan->n; i++) {
		arcsync_asset_t *a = arcsync_catalog_add_asset(dst);
		const arcsync_asset_t *s = &src->assets[plan->idx[i]];
		*a = *s;
		/* shallow copy pointers — do not free via catalog_free on dst */
		a->id = s->id;
		a->src_path = s->src_path;
		a->orig_name = s->orig_name;
		a->album_title = s->album_title;
		a->rel_album = s->rel_album;
		a->dest_rel = s->dest_rel;
		a->thumb_rel = s->thumb_rel;
		if (a->kind == ARCSYNC_KIND_PHOTO)
			dst->n_photos++;
		else if (a->kind == ARCSYNC_KIND_VIDEO)
			dst->n_videos++;
		dst->total_bytes += a->bytes;
	}
	/* rebuild albums for this volume */
	for (i = 0; i < src->n_albums; i++) {
		const arcsync_album_t *sa = &src->albums[i];
		arcsync_album_t *da = NULL;
		for (j = 0; j < sa->n_assets; j++) {
			size_t want = sa->asset_idx[j];
			size_t k;
			int found = -1;
			for (k = 0; k < plan->n; k++) {
				if (plan->idx[k] == want) {
					found = (int)k;
					break;
				}
			}
			if (found < 0)
				continue;
			if (!da)
				da = arcsync_catalog_find_or_add_album(dst, sa->title, sa->slug, sa->parent_slug);
			arcsync_album_add_asset(da, (size_t)found);
		}
	}
}

static void
catalog_from_plan_free_shallow(arcsync_catalog_t *dst)
{
	size_t i;
	/* assets point into parent — only free album index arrays and album strings we strdup'd */
	for (i = 0; i < dst->n_albums; i++) {
		free(dst->albums[i].title);
		free(dst->albums[i].slug);
		free(dst->albums[i].parent_slug);
		free(dst->albums[i].asset_idx);
	}
	free(dst->albums);
	free(dst->assets);
	memset(dst, 0, sizeof(*dst));
}

int
arcsync_run_pipeline(arcsync_opts_t *opts)
{
	arcsync_catalog_t cat;
	arcsync_stage_t stage;
	const char *source;
	int is_dir;
	uint64_t budget, largest = 0;
	int nvols, vi, rc;
	vol_plan_t *plans = NULL;
	time_t t0 = time(NULL);

	arcsync_catalog_init(&cat);
	memset(&stage, 0, sizeof(stage));

	{
		static char deflib[512];
		if (opts->dir) {
			source = opts->dir;
			is_dir = 1;
		} else if (opts->library) {
			source = opts->library;
			is_dir = 0;
		} else {
			const char *home = getenv("HOME");
			snprintf(deflib, sizeof(deflib),
			    "%s/Pictures/Photos Library.photoslibrary", home ? home : "");
			if (access(deflib, R_OK) != 0) {
				#ifdef _WIN32
				fprintf(stderr, "arcsync: on Windows pass --dir PATH (Photos libraries are macOS-only)\n");
#else
				fprintf(stderr, "arcsync: no default Photos library; pass --library or --dir\n");
#endif
				return 2;
			}
			source = deflib;
			is_dir = 0;
		}
	}

	if (!opts->quiet)
		fprintf(stderr, "arcsync: library  %s\n", source);

	if (is_dir)
		rc = arcsync_scan_dir(source, opts, &cat);
	else
		rc = arcsync_scan_photos(source, opts, &cat);
	if (rc != 0)
		return rc;

	if (cat.n_assets == 0) {
		fprintf(stderr, "arcsync: no assets matched filters\n");
		arcsync_catalog_free(&cat);
		return 3;
	}

	arcsync_assign_dests(&cat);

	if (!opts->quiet)
		fprintf(stderr, "arcsync: assets   %zu photos, %zu videos (%zu skipped unknown)\n",
		    cat.n_photos, cat.n_videos, cat.n_skipped);

	budget = arcsync_media_budget(opts->media);
	nvols = volume_count(&cat, budget, &largest);
	if (nvols < 0) {
		fprintf(stderr, "arcsync: a single file (%.1f MiB) exceeds --media %s budget\n",
		    largest / (1024.0 * 1024.0), arcsync_media_name(opts->media));
		arcsync_catalog_free(&cat);
		return 5;
	}
	if (nvols > 1 && !opts->split) {
		fprintf(stderr, "arcsync: staged %.1f GiB needs %d volumes (--media %s); pass --split\n",
		    cat.total_bytes / (1024.0 * 1024.0 * 1024.0), nvols,
		    arcsync_media_name(opts->media));
		arcsync_catalog_free(&cat);
		return 5;
	}

	if (opts->dry_run) {
		if (!opts->quiet) {
			fprintf(stderr, "arcsync: dry-run would write %d ISO(s), %.1f MiB payload\n",
			    nvols, cat.total_bytes / (1024.0 * 1024.0));
		}
		if (opts->json) {
			printf("{\"version\":\"%s\",\"dry_run\":true,\"photos\":%zu,\"videos\":%zu,"
			    "\"bytes\":%llu,\"volumes\":%d,\"out\":\"%s\"}\n",
			    ARCSYNC_VERSION, cat.n_photos, cat.n_videos,
			    (unsigned long long)cat.total_bytes, nvols, opts->out);
		}
		arcsync_catalog_free(&cat);
		return 0;
	}

	rc = plan_volumes(&cat, budget, &plans, &nvols);
	if (rc != 0) {
		arcsync_catalog_free(&cat);
		return rc;
	}

	if (!opts->quiet)
		fprintf(stderr, "arcsync: staged   %.1f MiB → %d volume(s) (--media %s%s)\n",
		    cat.total_bytes / (1024.0 * 1024.0), nvols, arcsync_media_name(opts->media),
		    opts->split ? " --split" : "");

	for (vi = 0; vi < nvols; vi++) {
		arcsync_catalog_t sub;
		char *outp;
		catalog_from_plan(&cat, &plans[vi], &sub);
		outp = out_path_for_volume(opts, vi + 1, nvols);

		rc = arcsync_stage_begin(&stage);
		if (rc != 0) {
			free(outp);
			catalog_from_plan_free_shallow(&sub);
			goto fail;
		}
		rc = arcsync_copy_assets(&sub, &stage, opts);
		if (rc != 0)
			goto volfail;
		rc = arcsync_make_thumbs(&sub, &stage, opts);
		if (rc != 0)
			goto volfail;
		rc = arcsync_write_html(&sub, &stage, opts, vi + 1, nvols, plans[vi].bytes);
		if (rc != 0)
			goto volfail;
		rc = arcsync_write_sidecar(&sub, &stage, opts);
		if (rc != 0)
			goto volfail;
		rc = arcsync_make_iso(&stage, opts, outp);
		if (rc != 0)
			goto volfail;
		if (opts->burn) {
			rc = arcsync_burn_iso(outp, opts);
			if (rc != 0)
				goto volfail;
		}
		arcsync_stage_end(&stage, 0);
		catalog_from_plan_free_shallow(&sub);
		free(outp);
		continue;
volfail:
		arcsync_stage_end(&stage, 1);
		catalog_from_plan_free_shallow(&sub);
		free(outp);
		goto fail;
	}

	if (!opts->quiet)
		fprintf(stderr, "arcsync: done     %d volume(s), %lds\n", nvols,
		    (long)(time(NULL) - t0));
	if (opts->json) {
		printf("{\"version\":\"%s\",\"photos\":%zu,\"videos\":%zu,\"bytes\":%llu,"
		    "\"volumes\":%d,\"out\":\"%s\",\"burn\":%s}\n",
		    ARCSYNC_VERSION, cat.n_photos, cat.n_videos,
		    (unsigned long long)cat.total_bytes, nvols, opts->out,
		    opts->burn ? "true" : "false");
	}

	for (vi = 0; vi < nvols; vi++)
		free(plans[vi].idx);
	free(plans);
	arcsync_catalog_free(&cat);
	return 0;

fail:
	if (plans) {
		for (vi = 0; vi < nvols; vi++)
			free(plans[vi].idx);
		free(plans);
	}
	arcsync_catalog_free(&cat);
	return rc ? rc : 4;
}
