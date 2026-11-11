/* Host-native test for src/image_info.h: gpon_image_info rendered from the
 * text of /etc/odi-build. Build it with -fsanitize=address to catch a parse
 * that leaves the buffer. `make test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/image_info.h"

static char out[4096];
static unsigned long outn;
static int fails;

static void put(void *ctx, const char *s, unsigned long n)
{
	(void)ctx;
	if (outn + n >= sizeof out)
		return;
	memcpy(out + outn, s, n);
	outn += n;
	out[outn] = 0;
}

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

/* Render `text` the way metric_image_info does: copy at most cap - 1 bytes
 * into a cap-byte buffer, as read_file() does, and terminate it. The buffer
 * is exactly cap bytes so a read past it is a stack overflow for the
 * sanitizer. */
static const char *render(const char *text, unsigned long cap)
{
	struct prov_out o = { put, 0 };
	char man[512];
	unsigned long n = strlen(text);

	if (n > cap - 1)
		n = cap - 1;
	memcpy(man, text, n);
	man[n] = 0;
	outn = 0;
	out[0] = 0;
	image_info_emit(&o, man, n, n == cap - 1);
	return out;
}

int main(void)
{
	/* 600 bytes in 20-byte lines, k00=... to k29=...; the buffer holds 511,
	 * so line k25 is cut after 11 bytes. */
	char big[601];
	int i;

	for (i = 0; i < 30; i++)
		snprintf(big + i * 20, 21, "k%02d=value%010d\n", i, i);

	ck(strstr(render("image=ODI-1\nbase=b.tar\n", 512),
		  "{image=\"ODI-1\",base=\"b.tar\"} 1\n") != NULL,
	   "two complete lines");
	ck(strstr(render("image=ODI-1\nbuilt=x", 512), "built=\"x\"") != NULL,
	   "last line without a newline, file shorter than the buffer: kept");
	ck(strstr(render("a=b\nbad\n=c\nq=\"x\n", 512), "{a=\"b\"} 1\n") != NULL,
	   "lines without name=value or with a quote are dropped");

	render(big, 512);
	ck(strstr(out, "k24=\"value0000000024\"") != NULL,
	   "600 bytes: the last complete line is kept");
	ck(strstr(out, "k25") == NULL,
	   "600 bytes: the line cut by the buffer is dropped");
	ck(strcmp(out + strlen(out) - 5, "\"} 1\n") == 0,
	   "600 bytes: output ends with the closing brace line");

	return fails ? 1 : 0;
}
