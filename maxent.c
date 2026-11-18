#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<math.h>

#include	"dag.h"
#include	"chart.h"
#include	"hash.h"
#include	"freeze.h"
#include	"maxent.h"

struct maxent_feature
{
	float	score;
	// XXX features below this comment are not accessible at parse time -- only at grammar load time
	char	*head;
	char	*rhs[2];

	int		ngp;
	char	**gp;
};

void print_mef(struct maxent_feature *f);

static struct hash	*mem;
static struct hash	*mem_ro;	// table for subset that only uses rule names; fixed-length keys

// values: ruleid
// -1 as rhs means no such daughter
// -1 as gp means no such grandparent
// -2 as gp means root
struct __attribute__((packed)) maxent_rule_key
{
	signed short lhs, rhs1, rhs2;
	signed short	gp[MAX_GRANDPARENTING];
};

#define MAXENT_NOISE(fmt, ...)
//#define MAXENT_NOISE printf

//int	nmaxent_requests = 0, nmaxent_lookups = 0, nmaxent_visits = 0;

/*show_me()
{
	fprintf(stderr, "MAXENT: %d visits / %d lookups / %d requests = %.1f per request ; %.1f%% hit rate\n", nmaxent_visits, nmaxent_lookups, nmaxent_requests, (float)nmaxent_visits / nmaxent_requests, 100*(float)nmaxent_visits / nmaxent_lookups);
}*/


/* Compact frozen form of the model.
 *
 * The original frozen form keeps both tables as chained hashes: a 24-byte
 * bucket per feature, a separately allocated float, and (for the string
 * table) a separately stored key string.  The compact form keeps the same
 * features in open-addressed arrays with the score stored inline.
 *
 * String keys are stored as their ':'-separated pieces, each piece mapped to
 * a 16-bit symbol id.  Joining with ':' is reversible by splitting on ':', so
 * two key strings are equal exactly when their piece lists are equal; that
 * makes a piece-list lookup give exactly the same answer as the original
 * strcmp() lookup.  Lookups whose key string would be truncated by the
 * original 512-byte buffer take the original string-building path, so they
 * see the same truncated keys as before.
 *
 * Duplicate features keep whichever score the original hash_find() returned.
 */

#define	MKEY_PIECES		8
#define	MKEY_NONE		0xFFFF	// unused piece slot
#define	MKEY_UNKNOWN	0xFFFE	// piece not in the model; can never match

struct maxent_compact_ro
{
	struct maxent_rule_key	key;
	float					score;
};

struct maxent_compact_str
{
	unsigned short	piece[MKEY_PIECES];
	float			score;
};

struct maxent_compact
{
	unsigned int				nro, ro_mask;	// *_mask fields hold table sizes
	struct maxent_compact_ro	*ro;
	unsigned int				nstr, str_mask;
	struct maxent_compact_str	*str;
	unsigned int				nsym, sym_mask;
	unsigned int				*sym_slot;	// symbol id + 1, or 0 if empty
	unsigned int				*sym_off;	// offset of each symbol in sym_pool
	char						*sym_pool;
};

static struct maxent_compact	*compact;

static inline unsigned int	mc_hash_bytes(const void *p, int len)
{
	const unsigned char	*b = p;
	unsigned int	h = 2166136261u;
	int i;
	for(i=0;i<len;i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

static inline unsigned int	mc_slot(unsigned int h, unsigned int size)
{
	return (unsigned int)(((unsigned long long)h * size) >> 32);
}

static inline unsigned short	mc_symbol(const char *str, int len)
{
	unsigned int	i = mc_slot(mc_hash_bytes(str, len), compact->sym_mask);
	while(compact->sym_slot[i])
	{
		unsigned int	id = compact->sym_slot[i] - 1;
		char	*sym = compact->sym_pool + compact->sym_off[id];
		if(!strncmp(sym, str, len) && sym[len]==0)return id;
		i = (i+1 == compact->sym_mask) ? 0 : i+1;
	}
	return MKEY_UNKNOWN;
}

// split 'str' on ':' and append the pieces' symbol ids; returns the new count
// (which may exceed MKEY_PIECES, meaning no stored key can match)
static inline int	mc_pieces(const char *str, unsigned short *out, int n)
{
	for(;;)
	{
		const char	*e = strchr(str, ':');
		int	len = e ? (int)(e-str) : (int)strlen(str);
		if(n < MKEY_PIECES)out[n] = mc_symbol(str, len);
		n++;
		if(!e)return n;
		str = e+1;
	}
}

static inline float	*mc_find_str(unsigned short *pieces, int n)
{
	if(n > MKEY_PIECES)return NULL;
	unsigned short	key[MKEY_PIECES];
	int k;
	for(k=0;k<n;k++)
	{
		if(pieces[k]==MKEY_UNKNOWN)return NULL;
		key[k] = pieces[k];
	}
	for(;k<MKEY_PIECES;k++)key[k] = MKEY_NONE;
	unsigned int	i = mc_slot(mc_hash_bytes(key, sizeof(key)), compact->str_mask);
	while(compact->str[i].piece[0] != MKEY_NONE)
	{
		if(!memcmp(compact->str[i].piece, key, sizeof(key)))return &compact->str[i].score;
		i = (i+1 == compact->str_mask) ? 0 : i+1;
	}
	return NULL;
}

static inline float	*mc_find_str_string(const char *key)
{
	unsigned short	pieces[MKEY_PIECES];
	int	n = mc_pieces(key, pieces, 0);
	return mc_find_str(pieces, n);
}

static inline float	*mc_find_ro(struct maxent_rule_key *key)
{
	unsigned int	i = mc_slot(mc_hash_bytes(key, sizeof(*key)), compact->ro_mask);
	while(compact->ro[i].key.lhs != -32768)
	{
		if(!memcmp(&compact->ro[i].key, key, sizeof(*key)))return &compact->ro[i].score;
		i = (i+1 == compact->ro_mask) ? 0 : i+1;
	}
	return NULL;
}

float	maxent_score_ro(int lhs, int rhs1, int rhs2, struct scoring_ancestry	*anc)
{
	struct maxent_rule_key key;
	int i;
	float	score = 0;
	if(!mem_ro && !compact)return 0;
	//printf("score_ro: mem_ro entries = %d\n", mem_ro->entries);
	//printf(" lhs %d = %s, rhs1 %d = %s, rhs2 %d, anc.rooted %d, anc.nparent_types %d [%s = %d]\n",
		//lhs, rules[lhs]->name, rhs1, rules[rhs1]->name, rhs2, anc->rooted, anc->nparent_types, anc->parent_types[0], lookup_rule(anc->parent_types[0])->rnum);
	key.lhs = lhs;
	key.rhs1 = rhs1;
	key.rhs2 = rhs2;
	for(i=0;i<MAX_GRANDPARENTING;i++)key.gp[i] = -1;
	for(i=0;i<=anc->nparent_types;i++)
	{
		if(i)
		{
			//struct rule	*r = lookup_rule(anc->parent_types[i-1]);
			//assert(r != NULL);
			//key.gp[i-1] = r->rnum;
			key.gp[i-1] = anc->gprnum[i-1];
		}
		float *v = compact ? mc_find_ro(&key) : hash_find(mem_ro, (void*)&key);
		if(v)score += *v;
	}
	if(anc->rooted && anc->nparent_types < MAX_GRANDPARENTING)
	{
		key.gp[anc->nparent_types] = -2;
		float *v = compact ? mc_find_ro(&key) : hash_find(mem_ro, (void*)&key);
		if(v)score += *v;
	}
	return score;
}

// visit all maxent features containing this local subtree and compatible with the ancestry
// add up their scores
float	maxent_score(char *lhs, char *rhs1, char *rhs2, int rtl_unused, struct scoring_ancestry	*anc)
{
	float ret = 0;
	//char	*key = rtl?rhs2:rhs1;
	char	hkey[512];
	int	space = 511;

	//nmaxent_requests++;

	//return (double)rand()/RAND_MAX - 0.5;
	if(!mem && !compact)return 0;

	if(compact)
	{
		int	ngpf;
		// the full key string at the deepest level, as the original code would build it
		int	i, full = strlen(lhs) + 1 + (rhs1?strlen(rhs1):0) + 1 + (rhs2?strlen(rhs2):0);
		for(i=0;i<anc->nparent_types;i++)full += 1 + strlen(rules[anc->gprnum[i]]->name);
		if(anc->rooted)full += 2;
		if(full <= 510)
		{
			unsigned short	pieces[MKEY_PIECES];
			int	n = mc_pieces(lhs, pieces, 0);
			n = mc_pieces(rhs1?:"", pieces, n);
			n = mc_pieces(rhs2?:"", pieces, n);
			for(ngpf=0;ngpf<=anc->nparent_types;ngpf++)
			{
				if(ngpf) n = mc_pieces(rules[anc->gprnum[ngpf-1]]->name, pieces, n);
				float	*v = mc_find_str(pieces, n);
				if(v)ret += *v;
			}
			if(anc->rooted)
			{
				n = mc_pieces("^", pieces, n);
				float	*v = mc_find_str(pieces, n);
				if(v)ret += *v;
			}
			return ret;
		}
	}

	int	ngp;
	char	*hkp = hkey;
	hkp += safe_snprintf(hkey, 511 - (hkp-hkey), "%s:%s:%s", lhs, rhs1?:"", rhs2?:"");//key?:"");
	for(ngp=0;ngp<=anc->nparent_types;ngp++)
	{
		//if(ngp) hkp += safe_snprintf(hkp, 511 - (hkp-hkey), ":%s", anc->parent_types[ngp-1]);
		if(ngp) hkp += safe_snprintf(hkp, 511 - (hkp-hkey), ":%s", rules[anc->gprnum[ngp-1]]->name);
		MAXENT_NOISE("scoring key = %s\n", hkey);
		float	*v = compact ? mc_find_str_string(hkey) : (float*)hash_find(mem, hkey);
		if(v)ret += *v;
		//nmaxent_lookups++;
	}
	if(anc->rooted)
	{
		hkp += safe_snprintf(hkp, 511 - (hkp-hkey), ":^");
		MAXENT_NOISE("scoring key = %s\n", hkey);
		float	*v = compact ? mc_find_str_string(hkey) : (float*)hash_find(mem, hkey);
		if(v)ret += *v;
		//nmaxent_lookups++;
	}
	MAXENT_NOISE(" ... %f\n", ret);
	return ret;
}

// loading

void print_mef(struct maxent_feature *f)
{
	printf("maxent head `%s' rhs [%s %s] score %f\n",
		f->head, f->rhs[0]?:"NO-RHS[0]", f->rhs[1]?:"", f->score);
}

void record_maxent_feature(char *lhs, char *rhs[2], float score, int	ngp, char	**gp)
{
	struct maxent_feature *feat = malloc(sizeof(struct maxent_feature));
	char	key[512], *kp = key;
	int	i;

	struct rule	*rhsrule[2];
	rhsrule[0] = lookup_rule(rhs[0]);
	rhsrule[1] = (*rhs[1])?lookup_rule(rhs[1]):NULL;
	if(rhsrule[0] && (rhsrule[1] || !*rhs[1]))
	{
		float	*v = malloc(sizeof(float));
		*v = score;
		struct maxent_rule_key	*bkey = malloc(sizeof(*bkey));
		struct rule	*lhsrule = lookup_rule(lhs);
		if(!lhsrule)
		{
			fprintf(stderr, "WARNING: no such rule as %s in maxent model\n", lhs);
			return;
		}
		assert(lhsrule != NULL);
		bkey->lhs = lhsrule->rnum;
		bkey->rhs1 = rhsrule[0]->rnum;
		if(*rhs[1])bkey->rhs2 = rhsrule[1]->rnum;
		else bkey->rhs2 = -1;
		for(i=0;i<ngp;i++)
		{
			if(!strcmp(gp[i],"^"))bkey->gp[i] = -2;
			else
			{
				struct rule	*rgp = lookup_rule(gp[i]);
				if(!rgp)
				{
					fprintf(stderr, "WARNING: no such rule %s in maxent model\n", gp[i]);
					return;
				}
				assert(rgp != NULL);
				bkey->gp[i] = rgp->rnum;
			}
		}
		for(;i<MAX_GRANDPARENTING;i++)bkey->gp[i] = -1;
		hash_add(mem_ro, (void*)bkey, v);
		//printf("mem_ro adding %d %d %d\n", bkey->lhs, bkey->rhs1, bkey->rhs2);
		//if(bkey->lhs == 333 && bkey->rhs1 == 255 && bkey->rhs2 == -1)printf("added 333,255,-1 to mem_ro\n");
		return;	// don't need it in ordinary 'mem' hash
	}

	kp += sprintf(key, "%s:%s:%s", lhs, rhs[0]?:"", rhs[1]?:"");
	for(i=0;i<ngp;i++)kp += sprintf(kp, ":%s", gp[i]);
	feat->score = score;

	feat->head = strdup(lhs);
	feat->rhs[0] = (rhs[0] && *rhs[0])?strdup(rhs[0]):NULL;
	feat->rhs[1] = (rhs[1] && *rhs[1])?strdup(rhs[1]):NULL;
	feat->ngp = ngp;
	feat->gp = malloc(sizeof(char*)*ngp);
	for(i=0;i<ngp;i++)feat->gp[i] = strdup(gp[i]);
	//print_mef(feat);
	//printf("recording feature at key '%s'\n", key);
	hash_add(mem, strdup(key), feat);
}

static int	maxent_lnum = 0;

static char	*parse_word(char	**P)
{
	char	*p = *P, *ret;

	if(*p==' ')p++;
	ret = p;
	int	quoted = 0;
	while(*p && (*p!=' ' || quoted))
	{
		if(*p=='\\' && p[1])p++;
		else if(*p=='"')quoted=!quoted;
		p++;
	}
	if(*p==' '){*p=0;p++;}
	else
	{
		if(quoted)
		{
			fprintf(stderr, "maxent: problem reading line %d of .mem file; quoted value not terminated properly?\n", maxent_lnum);
			exit(-1);
		}
		assert(!quoted);
	}
	*P = p;
	return *ret?ret:0;
}

int load_mem(char *fname)
{
	FILE	*f;
	char	line[1024];

#define DEBUG(fmt, ...)
//#define DEBUG	printf

	if(!fname)return 0;
	f = fopen(fname, "r");
	if(!f) { perror(fname); return -1; }

	/*if(mem)
		fprintf(stderr, "NOTE: overwriting pre-existing max-ent model with `%s'\n", fname);*/
	compact = NULL;	// a model loaded at run time replaces the frozen one
	mem = hash_new("maxent model");
	mem_ro = hash_new("maxent model (rule-only)");
	mem_ro->has_flk = sizeof(struct maxent_rule_key);

	DEBUG("loading maxent file from '%s'\n", fname);
	maxent_lnum = 0;

//struct timeval tv, tv2;
//gettimeofday(&tv, NULL);
//double tfgets = 0;
	while(fgets(line, 1024, f))
	{
		maxent_lnum++;
		// XXX this measurement was wrong...
		// need to reset `tv' before each fgets()
		//gettimeofday(&tv2, NULL);
		//tfgets += (double)(tv2.tv_sec - tv.tv_sec + 0.000001 * (tv2.tv_usec - tv.tv_usec));
		int llen = strlen(line), tint, i = 0;
		float sfloat;
		char *p, *type, *grandparents_count, *head, *rhs[2], *score;

		if(line[0]!='('){DEBUG("IGNORE maxent line `%s'\n", line);continue;}
		if(line[llen-1]=='\n')line[--llen] = 0;
		DEBUG("process maxent line '%s'\n", line);
		p = strrchr(line, ']');
		if(p>line+1)
		{
			// find *second* to last ']'
			p--;
			while(p>line && *p!=']')p--;
			if(p==line)p=NULL;
		}
		if(!p){DEBUG("IGNORE maxent line `%s' (no closing ])\n", line);continue;}
		*p = 0; score = p+1; p = strchr(line, '[');
		if(!p){DEBUG("IGNORE maxent line `%s' (no opening [)\n", line);continue;}
		p = p+1;
		while(*score==' ' || *score=='\t')score++;
		if((*score<'0' || *score>'9') && *score!='.' && *score!='+' && *score!='-'){DEBUG("IGNORE maxent score `%s'\n", score);continue;}
		sfloat = atof(score);

		type = parse_word(&p);
		grandparents_count = parse_word(&p);
		if(*grandparents_count != '('){DEBUG("IGNORE maxent line with bad grandparent count\n");continue;}
		int	ngrandparents = atoi(grandparents_count+1), j;
		char	*grandparents[ngrandparents];
		for(j=ngrandparents-1;j>=0;j--)grandparents[j] = parse_word(&p);
		head = parse_word(&p);
		if(!type || !head){DEBUG("IGNORE maxent line with type %s head %s\n", type?:"(null)", head?:"(null)");continue;}
		tint = atoi(type);
		if(tint <1 || tint >2){DEBUG("IGNORE maxentline with type %s\n", type);continue;}
		rhs[0] = parse_word(&p);
		while(*p==' ')p++;
		rhs[1] = p;
		record_maxent_feature(head, rhs, sfloat, ngrandparents, grandparents);
	}
	//printf("maxent loading fgets took %.2fs\n", tfgets);

	fclose(f);
	return 0;
}

struct stochastic_model
{
	struct hash	*mem, *mem_ro;
	struct maxent_compact	*compact;
};

static unsigned int	mc_table_size(unsigned int n)
{
	// at most 70% full
	return (unsigned int)(((unsigned long long)n * 10) / 7) + 16;
}

// build state (malloc'd while freezing, then copied into the image)
static char		*mcb_pool;
static int		mcb_pool_len, mcb_pool_cap;
static unsigned int	*mcb_sym_slot, *mcb_sym_off, mcb_nsym, mcb_sym_cap, mcb_sym_mask;

static int	mcb_symbol(const char *str, int len)
{
	unsigned int	i = mc_slot(mc_hash_bytes(str, len), mcb_sym_mask);
	while(mcb_sym_slot[i])
	{
		unsigned int	id = mcb_sym_slot[i] - 1;
		char	*sym = mcb_pool + mcb_sym_off[id];
		if(!strncmp(sym, str, len) && sym[len]==0)return id;
		i = (i+1 == mcb_sym_mask) ? 0 : i+1;
	}
	if(mcb_nsym >= MKEY_UNKNOWN || mcb_nsym >= mcb_sym_cap)return -1;
	if(mcb_pool_len + len + 1 > mcb_pool_cap)
	{
		mcb_pool_cap = (mcb_pool_cap + len + 1) * 2;
		mcb_pool = realloc(mcb_pool, mcb_pool_cap);
	}
	memcpy(mcb_pool + mcb_pool_len, str, len);
	mcb_pool[mcb_pool_len + len] = 0;
	mcb_sym_off[mcb_nsym] = mcb_pool_len;
	mcb_pool_len += len + 1;
	mcb_sym_slot[i] = ++mcb_nsym;
	return mcb_nsym - 1;
}

// returns NULL if the model doesn't fit the compact form; caller keeps the original form
static struct maxent_compact	*build_compact_model()
{
	int	i;
	struct hash_bucket	*walk;

	mcb_sym_cap = 65000;
	mcb_sym_mask = mc_table_size(mcb_sym_cap);
	mcb_sym_slot = calloc(mcb_sym_mask, sizeof(unsigned int));
	mcb_sym_off = calloc(mcb_sym_cap, sizeof(unsigned int));
	mcb_nsym = 0; mcb_pool = NULL; mcb_pool_len = mcb_pool_cap = 0;

	unsigned int	str_size = mc_table_size(mem->entries), str_mask = str_size, nstr = 0;
	struct maxent_compact_str	*str = malloc(sizeof(*str) * str_size);
	for(i=0;i<str_size;i++)str[i].piece[0] = MKEY_NONE;

	for(i=0;i<mem->size;i++)
		for(walk=mem->buckets[i];walk;walk=walk->next)
		{
			// the score a lookup of this key returns today (first match in its chain)
			struct maxent_feature	*winner = hash_find(mem, walk->key);
			unsigned short	key[MKEY_PIECES];
			int	n = 0, k;
			char	*p = walk->key;
			if(strlen(p) > 510) goto fail;	// lookups are truncated to 510 chars
			for(;;)
			{
				char	*e = strchr(p, ':');
				int	len = e ? (int)(e-p) : (int)strlen(p);
				if(n >= MKEY_PIECES) goto fail;
				int	id = mcb_symbol(p, len);
				if(id < 0) goto fail;
				key[n++] = id;
				if(!e)break;
				p = e+1;
			}
			for(k=n;k<MKEY_PIECES;k++)key[k] = MKEY_NONE;
			unsigned int	h = mc_slot(mc_hash_bytes(key, sizeof(key)), str_size);
			while(str[h].piece[0] != MKEY_NONE && memcmp(str[h].piece, key, sizeof(key)))h = (h+1 == str_size) ? 0 : h+1;
			if(str[h].piece[0] == MKEY_NONE)
			{
				memcpy(str[h].piece, key, sizeof(key));
				str[h].score = winner->score;
				nstr++;
			}
			else assert(str[h].score == winner->score || isnan(winner->score));
		}

	unsigned int	ro_size = mc_table_size(mem_ro->entries), ro_mask = ro_size, nro = 0;
	struct maxent_compact_ro	*ro = malloc(sizeof(*ro) * ro_size);
	for(i=0;i<ro_size;i++)ro[i].key.lhs = -32768;
	for(i=0;i<mem_ro->size;i++)
		for(walk=mem_ro->buckets[i];walk;walk=walk->next)
		{
			float	*winner = hash_find(mem_ro, walk->key);
			struct maxent_rule_key	*key = (struct maxent_rule_key*)walk->key;
			if(key->lhs == -32768) goto fail;
			unsigned int	h = mc_slot(mc_hash_bytes(key, sizeof(*key)), ro_size);
			while(ro[h].key.lhs != -32768 && memcmp(&ro[h].key, key, sizeof(*key)))h = (h+1 == ro_size) ? 0 : h+1;
			if(ro[h].key.lhs == -32768)
			{
				ro[h].key = *key;
				ro[h].score = *winner;
				nro++;
			}
		}

	// copy into the image
	struct maxent_compact	*mc = slab_alloc(sizeof(*mc));
	mc->nstr = nstr; mc->str_mask = str_mask;
	mc->str = slab_alloc(sizeof(*str) * str_size); memcpy(mc->str, str, sizeof(*str) * str_size);
	mc->nro = nro; mc->ro_mask = ro_mask;
	mc->ro = slab_alloc(sizeof(*ro) * ro_size); memcpy(mc->ro, ro, sizeof(*ro) * ro_size);
	mc->nsym = mcb_nsym; mc->sym_mask = mcb_sym_mask;
	mc->sym_slot = slab_alloc(sizeof(unsigned int) * mcb_sym_mask); memcpy(mc->sym_slot, mcb_sym_slot, sizeof(unsigned int) * mcb_sym_mask);
	mc->sym_off = slab_alloc(sizeof(unsigned int) * (mcb_nsym?mcb_nsym:1)); memcpy(mc->sym_off, mcb_sym_off, sizeof(unsigned int) * mcb_nsym);
	int	pool_len4 = (mcb_pool_len + 3) & ~3;
	mc->sym_pool = slab_alloc(pool_len4 ? pool_len4 : 4); memcpy(mc->sym_pool, mcb_pool, mcb_pool_len);
	fprintf(stderr, "NOTE: compact max-ent model: %u string features, %u rule features, %u symbols\n", nstr, nro, mcb_nsym);
	free(str); free(ro); free(mcb_sym_slot); free(mcb_sym_off); free(mcb_pool);
	return mc;

fail:
	fprintf(stderr, "NOTE: max-ent model doesn't fit the compact form; keeping the original form\n");
	free(str); free(mcb_sym_slot); free(mcb_sym_off); free(mcb_pool);
	return NULL;
}

void	*freeze_stochastic_model()
{
	int	i, j;
	struct hash_bucket *walk;
	if(!mem)
	{
		struct stochastic_model *mod = slab_alloc(sizeof(*mod));
		mod->mem = NULL;
		mod->mem_ro = NULL;
		mod->compact = NULL;
		return mod;
	}
	struct maxent_compact	*mc = getenv("ACE_ORIGINAL_MAXENT") ? NULL : build_compact_model();
	if(mc)
	{
		struct stochastic_model *mod = slab_alloc(sizeof(*mod));
		mod->mem = NULL;
		mod->mem_ro = NULL;
		mod->compact = mc;
		return mod;
	}
	for(i=0;i<mem->size;i++)
		for(walk=mem->buckets[i];walk;walk=walk->next)
		{
			struct maxent_feature *fi = walk->value;
			struct maxent_feature *fo = slab_alloc(sizeof(float));//slab_alloc(sizeof(struct maxent_feature));
			fo->score = fi->score;
			walk->value = fo;
		}
	for(i=0;i<mem_ro->size;i++)
		for(walk=mem_ro->buckets[i];walk;walk=walk->next)
		{
			float *v = walk->value;
			float *vo = slab_alloc(sizeof(float));//slab_alloc(sizeof(struct maxent_feature));
			*vo = *v;
			walk->value = vo;
		}
	struct stochastic_model *mod = slab_alloc(sizeof(*mod));
	mod->mem = freeze_hash(mem);
	mod->mem_ro = freeze_hash(mem_ro);
	mod->compact = NULL;
	return mod;
}

void	recover_stochastic_model(void *_frozen)
{
	extern int debug_level;
	struct stochastic_model *mod = _frozen;
	if(!mod) { mem = NULL; mem_ro = NULL; compact = NULL; }
	else
	{
		mem = mod->mem;
		mem_ro = mod->mem_ro;
		compact = mod->compact;
	}
	if(mem && debug_level)printf("NOTE: max-ent model hash contains %d entries in %d slots\n", mem->entries, mem->size);
}
