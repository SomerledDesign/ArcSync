#include "arcsync.h"

#ifndef _WIN32
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#endif
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int
write_placeholder_jpeg(const char *path)
{
	/* Minimal 1x1 gray JPEG */
	static const unsigned char jpeg[] = {
		0xFF,0xD8,0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,
		0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
		0x07,0x07,0x07,0x09,0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,
		0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,0x24,0x2E,0x27,0x20,
		0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,
		0x39,0x3D,0x38,0x32,0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,
		0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xFF,0xC4,0x00,0x14,
		0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00,0x37,0xFF,0xD9
	};
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	if (fwrite(jpeg, 1, sizeof(jpeg), fp) != sizeof(jpeg)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

static int
make_thumb(const char *src, const char *dst, int long_edge)
{
#ifdef _WIN32
	(void)src; (void)long_edge;
	return write_placeholder_jpeg(dst);
#else
	CFStringRef path;
	CFURLRef url;
	CGImageSourceRef src_ref;
	CGImageRef image = NULL;
	CGImageRef scaled = NULL;
	CGContextRef ctx = NULL;
	CGColorSpaceRef cs = NULL;
	CFURLRef out_url = NULL;
	CGImageDestinationRef dest = NULL;
	CFMutableDictionaryRef opts = NULL;
	size_t w, h;
	double scale;
	size_t nw, nh;
	int rc = -1;

	path = CFStringCreateWithCString(NULL, src, kCFStringEncodingUTF8);
	if (!path)
		return -1;
	url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
	CFRelease(path);
	if (!url)
		return -1;
	src_ref = CGImageSourceCreateWithURL(url, NULL);
	CFRelease(url);
	if (!src_ref)
		return -1;
	image = CGImageSourceCreateImageAtIndex(src_ref, 0, NULL);
	CFRelease(src_ref);
	if (!image)
		return -1;

	w = CGImageGetWidth(image);
	h = CGImageGetHeight(image);
	if (w == 0 || h == 0) {
		CGImageRelease(image);
		return -1;
	}
	scale = (double)long_edge / (double)(w > h ? w : h);
	if (scale > 1.0)
		scale = 1.0;
	nw = (size_t)(w * scale + 0.5);
	nh = (size_t)(h * scale + 0.5);
	if (nw < 1) nw = 1;
	if (nh < 1) nh = 1;

	cs = CGColorSpaceCreateDeviceRGB();
	ctx = CGBitmapContextCreate(NULL, nw, nh, 8, nw * 4, cs,
	    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (!ctx)
		goto done;
	CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
	CGContextDrawImage(ctx, CGRectMake(0, 0, nw, nh), image);
	scaled = CGBitmapContextCreateImage(ctx);
	if (!scaled)
		goto done;

	{
		CFStringRef opath = CFStringCreateWithCString(NULL, dst, kCFStringEncodingUTF8);
		if (!opath)
			goto done;
		out_url = CFURLCreateWithFileSystemPath(NULL, opath, kCFURLPOSIXPathStyle, false);
		CFRelease(opath);
	}
	if (!out_url)
		goto done;
	dest = CGImageDestinationCreateWithURL(out_url, CFSTR("public.jpeg"), 1, NULL);
	if (!dest)
		goto done;
	opts = CFDictionaryCreateMutable(NULL, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	{
		double q = 0.72;
		CFNumberRef qn = CFNumberCreate(NULL, kCFNumberDoubleType, &q);
		CFDictionarySetValue(opts, kCGImageDestinationLossyCompressionQuality, qn);
		CFRelease(qn);
	}
	CGImageDestinationAddImage(dest, scaled, opts);
	if (!CGImageDestinationFinalize(dest))
		goto done;
	rc = 0;
done:
	if (opts) CFRelease(opts);
	if (dest) CFRelease(dest);
	if (out_url) CFRelease(out_url);
	if (scaled) CGImageRelease(scaled);
	if (ctx) CGContextRelease(ctx);
	if (cs) CGColorSpaceRelease(cs);
	if (image) CGImageRelease(image);
	return rc;
}
#endif /* !_WIN32 */


int
arcsync_make_thumbs(arcsync_catalog_t *cat, const arcsync_stage_t *st,
    const arcsync_opts_t *opts)
{
	char thumbdir[4096], dest[4096];
	size_t i;

	if (arcsync_path_join(thumbdir, sizeof(thumbdir), st->stage_root, "thumbs") != 0)
		return 4;
	if (arcsync_mkdir_p(thumbdir) != 0)
		return 4;

	for (i = 0; i < cat->n_assets; i++) {
		arcsync_asset_t *a = &cat->assets[i];
		if (!a->thumb_rel)
			continue;
		if (arcsync_path_join(dest, sizeof(dest), st->stage_root, a->thumb_rel) != 0)
			return 4;
		if (a->kind == ARCSYNC_KIND_VIDEO) {
			if (write_placeholder_jpeg(dest) != 0)
				return 4;
			continue;
		}
		if (make_thumb(a->src_path, dest, opts->thumb_size) != 0) {
			if (!opts->quiet)
				fprintf(stderr, "arcsync: thumb fallback for %s\n", a->orig_name);
			if (write_placeholder_jpeg(dest) != 0)
				return 4;
		}
	}
	if (!opts->quiet)
		fprintf(stderr, "arcsync: thumbs   %zu\n", cat->n_assets);
	return 0;
}
