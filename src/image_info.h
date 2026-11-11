/*
 * The image's build manifest (/etc/odi-build, one name=value per line) as
 * labels on gpon_image_info. Pure text in, text out, through the provision.h
 * writer: no syscalls, so test/test_image_info.c runs it natively.
 */
#ifndef ODI_IMAGE_INFO_H
#define ODI_IMAGE_INFO_H

#include "provision.h"

/*
 * A label value must not contain a quote, a backslash or a newline, and this
 * one comes from argv[0] — chosen by whoever started the process, not by us.
 * Escaping it properly would be more code than refusing it: an unexpected path
 * is reported as "unknown" rather than being allowed to produce an exposition
 * Prometheus cannot parse.
 */
static int label_safe(const char *s)
{
	unsigned long i;

	for (i = 0; s[i]; i++)
		if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r')
			return 0;
	return i != 0;
}

/*
 * `man` holds n bytes and a terminating zero at man[n]; the parse stays
 * inside those n bytes. `cut` is set when the file filled the buffer, so the
 * last line may be cut off: a final line without a newline is then dropped,
 * where on a shorter file it is a complete line and kept.
 */
static void image_info_emit(const struct prov_out *o, char *man, unsigned long n, int cut)
{
	unsigned long i = 0;
	int first = 1;

	prov_header(o, "gpon_image_info",
		    "Always 1. Labels name the firmware image this stick was built "
		    "from and the component builds inside it, from /etc/odi-build.");
	prov_puts(o, "gpon_image_info{");
	while (i < n) {
		unsigned long ls = i, le = i, eq;

		while (le < n && man[le] != '\n')
			le++;
		if (le == n && cut)
			break;
		eq = ls;
		while (eq < le && man[eq] != '=')
			eq++;
		if (eq == le || eq == ls)
			goto next;

		man[eq] = 0;
		man[le] = 0;
		if (label_safe(man + ls) && label_safe(man + eq + 1)) {
			if (!first)
				prov_puts(o, ",");
			first = 0;
			prov_puts(o, man + ls);
			prov_puts(o, "=\"");
			prov_puts(o, man + eq + 1);
			prov_puts(o, "\"");
		}
next:
		i = le + 1;
	}
	prov_puts(o, "} 1\n");
}

#endif
