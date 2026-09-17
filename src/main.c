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

	/*
	 * Scaffold only (PRD section 14 steps 1-2). Pipeline lands next:
	 * --dir walker -> thumbs -> HTML -> hdiutil -> optional --burn.
	 */
	if (!opts.quiet) {
		fprintf(stderr,
		    "arcsync: scaffold ready (args + banner). "
		    "No archive path yet — pass --dir when the pipeline lands.\n");
		if (opts.dir)
			fprintf(stderr, "arcsync: would use --dir %s\n", opts.dir);
		else if (opts.library)
			fprintf(stderr, "arcsync: would use --library %s\n", opts.library);
		else
			fprintf(stderr,
			    "arcsync: default library would be "
			    "~/Pictures/Photos Library.photoslibrary\n");
		fprintf(stderr, "arcsync: out=%s media=%d burn=%d dry_run=%d\n",
		    opts.out, (int)opts.media, opts.burn, opts.dry_run);
	}

	if (opts.json) {
		printf("{\"version\":\"%s\",\"scaffold\":true,\"out\":\"%s\",\"burn\":%s}\n",
		    ARCSYNC_VERSION, opts.out, opts.burn ? "true" : "false");
	}

	return 0;
}
