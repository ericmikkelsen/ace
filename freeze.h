int			load_frozen_grammar(char	*path);
int			setup_save_frozen_grammar(char	*path);
int			load_grammar();
int			freeze_grammar();
void		dump_grammar();
void		reload_frozen_grammar();
char		*freeze_string(char	*str);
wchar_t		*freeze_wcs(wchar_t	*str);
void		*freeze_block(void	*src, int	len);
struct dg	*freeze_dg(struct dg	*dg);

extern long	loaded_freeze_size;

//#define	FREEZER_MMAP_BASE	0x60000000
//#define	FREEZER_MMAP_BASE	0x6000000000L
#ifdef	__EMSCRIPTEN__
// 32 MB: above static data and the stack, below anything the heap reaches early
#define	FREEZER_MMAP_BASE	0x02000000L
void	*wasm_freezer_region(size_t size);
int		wasm_read_all(int fd, void *dst, size_t len);
int		wasm_write_all(int fd, void *src, size_t len);
#else
#define	FREEZER_MMAP_BASE	0x7000000000L
#endif

#define	FROZEN(x)		((long)x>=FREEZER_MMAP_BASE && (long)x<(FREEZER_MMAP_BASE+loaded_freeze_size))
