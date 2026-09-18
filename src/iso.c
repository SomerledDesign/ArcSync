#include "arcsync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <io.h>
# include <process.h>
# define unlink _unlink
# define access _access
# define F_OK 0
#else
# include <spawn.h>
# include <sys/wait.h>
# include <unistd.h>
extern char **environ;
#endif

#ifndef _WIN32
static int
run_argv(char *const argv[], const arcsync_opts_t *opts)
{
	pid_t pid;
	int status;
	if (opts && opts->verbose) {
		size_t i;
		fprintf(stderr, "arcsync: exec");
		for (i = 0; argv[i]; i++)
			fprintf(stderr, " %s", argv[i]);
		fputc('\n', stderr);
	}
	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
		fprintf(stderr, "arcsync: cannot spawn %s: %s\n", argv[0], strerror(errno));
		return 6;
	}
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "arcsync: waitpid failed: %s\n", strerror(errno));
		return 6;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "arcsync: %s failed (status %d)\n", argv[0], status);
		return 6;
	}
	return 0;
}
#endif

int
arcsync_make_iso(const arcsync_stage_t *st, const arcsync_opts_t *opts,
    const char *out_path)
{
	int rc;

	if (!opts->force && access(out_path, F_OK) == 0) {
		fprintf(stderr, "arcsync: %s exists (use --force)\n", out_path);
		return 4;
	}
	if (opts->force)
		unlink(out_path);

#ifdef _WIN32
	rc = arcsync_write_iso9660(st->stage_root, out_path,
	    opts->volume_name ? opts->volume_name : "ARCSYNC");
#else
	{
		char *argv[16];
		int i = 0;
		argv[i++] = "/usr/bin/hdiutil";
		argv[i++] = "makehybrid";
		argv[i++] = "-iso";
		argv[i++] = "-joliet";
		argv[i++] = "-udf";
		argv[i++] = "-hfs";
		argv[i++] = "-default-volume-name";
		argv[i++] = (char *)(opts->volume_name ? opts->volume_name : "ARCSYNC");
		argv[i++] = "-hfs-openfolder";
		argv[i++] = (char *)st->stage_root;
		argv[i++] = "-o";
		argv[i++] = (char *)out_path;
		argv[i++] = (char *)st->stage_root;
		argv[i] = NULL;
		rc = run_argv(argv, opts);
	}
#endif
	if (rc == 0 && !opts->quiet)
		fprintf(stderr, "arcsync: iso      %s\n", out_path);
	return rc;
}

int
arcsync_burn_iso(const char *iso_path, const arcsync_opts_t *opts)
{
#ifdef _WIN32
	(void)iso_path;
	fprintf(stderr, "arcsync: --burn is not supported on Windows yet; ISO written.\n");
	(void)opts;
	return 0;
#else
	char *argv[8];
	int i = 0;
	argv[i++] = "/usr/bin/hdiutil";
	argv[i++] = "burn";
	argv[i++] = (char *)iso_path;
	argv[i] = NULL;
	if (!opts->quiet)
		fprintf(stderr, "arcsync: burn     %s (insert blank media if prompted)\n", iso_path);
	return run_argv(argv, opts);
#endif
}
