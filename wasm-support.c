/* Support for building the engine with Emscripten (WebAssembly).
 *
 * A grammar image is a raw dump of memory that must be loaded at the same
 * address it was written at (FREEZER_MMAP_BASE).  WebAssembly has no
 * mmap() at a chosen address, so the image region is reserved directly from
 * the heap break at a fixed low address, before the heap has grown past it,
 * and files are read into / written from it with plain read() and write().
 */
#ifdef __EMSCRIPTEN__
#include	<stdio.h>
#include	<stdlib.h>
#include	<stdint.h>
#include	<unistd.h>
#include	"freeze.h"

static size_t	wasm_reserved = 0;

void	*wasm_freezer_region(size_t size)
{
	if(wasm_reserved)
	{
		if(size > wasm_reserved)
		{
			fprintf(stderr, "wasm: grammar image region is %zu bytes but %zu are needed\n", wasm_reserved, size);
			exit(-1);
		}
		return (void*)FREEZER_MMAP_BASE;
	}
	uintptr_t	cur = (uintptr_t)sbrk(0);
	if(cur > FREEZER_MMAP_BASE)
	{
		fprintf(stderr, "wasm: heap already passes the grammar image address (%p > %p)\n", (void*)cur, (void*)FREEZER_MMAP_BASE);
		exit(-1);
	}
	if(cur < FREEZER_MMAP_BASE && sbrk(FREEZER_MMAP_BASE - cur) == (void*)-1) { perror("sbrk"); exit(-1); }
	if(sbrk(size) != (void*)FREEZER_MMAP_BASE) { fprintf(stderr, "wasm: couldn't reserve the grammar image region\n"); exit(-1); }
	wasm_reserved = size;
	return (void*)FREEZER_MMAP_BASE;
}

int	wasm_read_all(int fd, void *dst, size_t len)
{
	size_t	got = 0;
	lseek(fd, 0, SEEK_SET);
	while(got < len)
	{
		ssize_t	r = read(fd, (char*)dst + got, len - got);
		if(r <= 0) { perror("read grammar image"); return -1; }
		got += r;
	}
	return 0;
}

int	wasm_write_all(int fd, void *src, size_t len)
{
	size_t	done = 0;
	lseek(fd, 0, SEEK_SET);
	while(done < len)
	{
		ssize_t	w = write(fd, (char*)src + done, len - done);
		if(w <= 0) { perror("write grammar image"); return -1; }
		done += w;
	}
	return 0;
}

/* The quickcheck extractor is C generated from the grammar.  Natively it is
 * compiled to a shared library and loaded with dlopen(); here it is compiled
 * into the module instead.  This weak stub is replaced by that code. */
struct type; struct dg;
__attribute__((weak)) void	extractor(struct type **entries, struct dg *d)
{
	fprintf(stderr, "ERROR: this build has no quickcheck extractor linked in; generate it with ACE_QC_C_ONLY=1 and rebuild with QC_C=<file>\n");
	exit(-1);
}
#endif

/* ---- Browser entry points (used by wasm/web/worker.js) ----
 *
 *   base = ace_web_reserve(bytes)   reserve the image region; JS streams the image into it
 *   ace_web_init(bytes, "args")      parse engine arguments and set up from the in-memory image
 *   n = ace_web_parse("sentence")    parse one line; output goes to stdout (Module.print)
 */
#ifdef __EMSCRIPTEN__
#include	<emscripten.h>
#include	<string.h>
#include	<locale.h>

extern void	parse_args(int argc, char *argv[]);
extern void	initialize_engine();
extern int	parse_line(char *line);
extern void	clear_slab();
extern char	*load_grammar_path;
long	wasm_memory_image_bytes = 0;

EMSCRIPTEN_KEEPALIVE void	*ace_web_reserve(unsigned int bytes)
{
	return wasm_freezer_region(bytes);
}

EMSCRIPTEN_KEEPALIVE int	ace_web_init(unsigned int image_bytes, char *args)
{
	static char	*argv[64];
	int	argc = 0;
	setlocale(LC_ALL, "C.UTF-8");	// grammars are UTF-8; see the locale note in the build docs
	setlocale(LC_NUMERIC, "POSIX");
	argv[argc++] = "ace";
	char	*copy = strdup(args ? args : ""), *tok;
	for(tok = strtok(copy, " "); tok && argc < 60; tok = strtok(NULL, " "))argv[argc++] = tok;
	argv[argc++] = "-g";
	argv[argc++] = "@memory";
	argv[argc] = NULL;
	wasm_memory_image_bytes = image_bytes;
	parse_args(argc, argv);
	initialize_engine();
	return 0;
}

EMSCRIPTEN_KEEPALIVE int	ace_web_parse(char *line)
{
	char	*copy = strdup(line);
	clear_slab();
	int	n = parse_line(copy);
	free(copy);
	fflush(stdout);
	fflush(stderr);
	return n;
}
#endif
