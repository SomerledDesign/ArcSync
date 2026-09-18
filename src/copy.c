#include "arcsync.h"

#ifndef _WIN32
#include <copyfile.h>
#else
#include <windows.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#define unlink _unlink
#endif

int
arcsync_stage_begin(arcsync_stage_t *st)
{
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !*tmp)
		tmp = getenv("TEMP");
	if (!tmp || !*tmp)
#ifdef _WIN32
		tmp = ".";
#else
		tmp = "/tmp";
#endif
	st->work_root = arcsync_aprintf("%s/arcsync-%d", tmp, (int)getpid());
	st->stage_root = arcsync_aprintf("%s/stage", st->work_root);
	arcsync_rm_rf(st->work_root);
	if (arcsync_mkdir_p(st->stage_root) != 0) {
		fprintf(stderr, "arcsync: cannot create staging dir %s\n", st->stage_root);
		return 4;
	}
	return 0;
}

void
arcsync_stage_end(arcsync_stage_t *st, int keep)
{
	if (!keep && st->work_root)
		arcsync_rm_rf(st->work_root);
	else if (keep && st->work_root)
		fprintf(stderr, "arcsync: staging left at %s\n", st->work_root);
	free(st->stage_root);
	free(st->work_root);
	st->stage_root = NULL;
	st->work_root = NULL;
}

int
arcsync_copy_assets(const arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts)
{
	size_t i;
	char dest[4096], parent[4096];
	char *slash;

	for (i = 0; i < cat->n_assets; i++) {
		const arcsync_asset_t *a = &cat->assets[i];
		if (a->missing || !a->dest_rel)
			continue;
		if (arcsync_path_join(dest, sizeof(dest), st->stage_root, a->dest_rel) != 0)
			return 4;
		snprintf(parent, sizeof(parent), "%s", dest);
		slash = strrchr(parent, '/');
#ifdef _WIN32
		if (!slash) slash = strrchr(parent, '\\');
#endif
		if (slash) {
			*slash = '\0';
			if (arcsync_mkdir_p(parent) != 0)
				return 4;
		}
		if (opts->verbose)
			fprintf(stderr, "arcsync: copy %s → %s\n", a->src_path, a->dest_rel);
#ifdef _WIN32
		{
			DWORD flags = opts->force ? 0 : COPY_FILE_FAIL_IF_EXISTS;
			if (!CopyFileA(a->src_path, dest, opts->force ? FALSE : TRUE)) {
				fprintf(stderr, "arcsync: copy failed %s (err %lu)\n",
				    a->src_path, (unsigned long)GetLastError());
				return 4;
			}
			(void)flags;
		}
#else
		if (copyfile(a->src_path, dest, NULL, COPYFILE_DATA | COPYFILE_EXCL) != 0) {
			if (errno == EEXIST && opts->force) {
				unlink(dest);
				if (copyfile(a->src_path, dest, NULL, COPYFILE_DATA) != 0) {
					fprintf(stderr, "arcsync: copy failed %s: %s\n",
					    a->src_path, strerror(errno));
					return 4;
				}
			} else {
				fprintf(stderr, "arcsync: copy failed %s: %s\n",
				    a->src_path, strerror(errno));
				return 4;
			}
		}
		chmod(dest, 0644);
#endif
		if (!opts->quiet && !opts->verbose && (i + 1) % 50 == 0)
			fprintf(stderr, "arcsync: copying  %zu/%zu\n", i + 1, cat->n_assets);
	}
	return 0;
}
