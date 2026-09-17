/*
 * ArcSync — archive a photo library to a hybrid CD/DVD ISO.
 *
 * Win32 asm photo-CD tools, 1998; this is that program after the API grew up.
 */

#include "arcsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	arcsync_opts_t opts;
	int rc;

	rc = arcsync_parse_args(argc, argv, &opts);
	if (rc != 0)
		return 1;

	if (opts.help) {
		arcsync_print_help();
		return 0;
	}

	if (opts.version) {
		arcsync_banner();
		return 0;
	}

	if (opts.verbose && !opts.json)
		arcsync_banner();

	return arcsync_run_pipeline(&opts);
}
