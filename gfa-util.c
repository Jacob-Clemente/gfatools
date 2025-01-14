#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include "gfa-priv.h"
#include "kvec.h"
#include "ksort.h"
#include "kdq.h"
KDQ_INIT(uint64_t)

void gfa_sub(gfa_t *g, int n, char *const* seg, int step)
{
	int32_t i;
	int8_t *flag;
	kdq_t(uint64_t) *q;
	if (n == 0) return;
	q = kdq_init(uint64_t, 0);
	GFA_CALLOC(flag, g->n_seg * 2);
	for (i = 0; i < n; ++i) {
		int32_t s;
		s = gfa_name2id(g, seg[i]);
		if (s >= 0) {
			kdq_push(uint64_t, q, ((uint64_t)s<<1|0)<<32);
			kdq_push(uint64_t, q, ((uint64_t)s<<1|1)<<32);
		}
	}
	for (i = 0; i < g->n_seg; ++i) // mark all segments to be deleted
		g->seg[i].del = 1;
	while (kdq_size(q) > 0) {
		uint64_t x = *kdq_shift(uint64_t, q);
		uint32_t v = x>>32;
		int r = (int32_t)x;
		if (flag[v]) continue; // already visited
		flag[v] = 1;
		g->seg[v>>1].del = 0;
		if (r < step) {
			uint32_t nv = gfa_arc_n(g, v);
			gfa_arc_t *av = gfa_arc_a(g, v);
			for (i = 0; i < nv; ++i) {
				if (flag[av[i].w] == 0)
					kdq_push(uint64_t, q, (uint64_t)av[i].w<<32 | (r + 1));
				if (flag[av[i].w^1] == 0)
					kdq_push(uint64_t, q, (uint64_t)(av[i].w^1)<<32 | (r + 1));
			}
		}
	}
	kdq_destroy(uint64_t, q);
	free(flag);
	gfa_arc_rm(g);
}

static uint64_t find_join(const gfa_t *g, uint32_t v)
{
	gfa_seg_t *t, *s = &g->seg[v>>1];
	int32_t i, nv, n_low, n_r;
	uint32_t w;
	gfa_arc_t *av;
	if (s->rank == 0) return (uint64_t)-1;
	nv = gfa_arc_n(g, v);
	av = gfa_arc_a(g, v);
	for (i = 0, n_low = n_r = 0, w = 0; i < nv; ++i) {
		gfa_arc_t *q = &av[i];
		if (q->rank >= 0 && q->rank == s->rank) {
			++n_r, w = q->w;
		} else {
			t = &g->seg[q->w>>1];
			if (t->rank >= 0 && t->rank < s->rank)
				++n_low, w = q->w;
		}
	}
	if (n_r != 1 && gfa_verbose >= 2)
		fprintf(stderr, "[W] failed to find the associated arc for vertex %c%s[%d]: %d,%d\n", "><"[v&1], g->seg[v>>1].name, v, n_r, n_low);
	if (n_r != 1 && n_low != 1) return (uint64_t)-1;
	t = &g->seg[w>>1];
	return (uint64_t)t->snid<<32 | (uint32_t)(w&1? t->soff + t->len : t->soff) << 1 | (w&1);
}

gfa_sfa_t *gfa_gfa2sfa(const gfa_t *g, int32_t *n_sfa_, int32_t write_seq)
{
	int32_t i, j, k, *scnt, *soff, n_sfa;
	gfa_sfa_t *sfa = 0;
	uint64_t *a;

	*n_sfa_ = 0;
	if (g->n_sseq == 0) return 0;

	// precount
	GFA_CALLOC(scnt, g->n_sseq);
	for (i = 0; i < g->n_seg; ++i)
		if (g->seg[i].snid >= 0)
			++scnt[g->seg[i].snid];
	GFA_MALLOC(soff, g->n_sseq + 1);
	for (soff[0] = 0, i = 1; i <= g->n_sseq; ++i)
		soff[i] = soff[i - 1] + scnt[i - 1];

	// fill a[]
	GFA_BZERO(scnt, g->n_sseq);
	GFA_MALLOC(a, g->n_seg);
	for (i = 0; i < g->n_seg; ++i) {
		const gfa_seg_t *s = &g->seg[i];
		if (s->snid < 0) continue;
		a[soff[s->snid] + scnt[s->snid]] = (uint64_t)s->soff<<32 | i;
		++scnt[s->snid];
	}
	for (i = 0; i < g->n_sseq; ++i)
		if (scnt[i] > 1)
			radix_sort_gfa64(&a[soff[i]], &a[soff[i+1]]);
	free(scnt);

	// check
	n_sfa = g->n_sseq;
	for (i = 0; i < g->n_sseq; ++i) {
		const gfa_seg_t *s;
		if (soff[i] == soff[i+1]) --n_sfa;
		if (soff[i] == soff[i+1]) continue;
		s = &g->seg[(int32_t)a[soff[i]]];
		if (s->rank == 0 && s->soff != 0) {
			if (gfa_verbose >= 2)
				fprintf(stderr, "[W] rank-0 stable sequence \"%s\" not started with 0\n", g->sseq[s->snid].name);
			goto end_check;
		}
		for (j = soff[i] + 1; j < soff[i+1]; ++j) {
			const gfa_seg_t *s = &g->seg[(int32_t)a[j-1]];
			const gfa_seg_t *t = &g->seg[(int32_t)a[j]];
			if (s->soff + s->len > t->soff) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] overlap on stable sequence \"%s\"\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->rank == 0 && s->soff + s->len != t->soff) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] rank-0 stable sequence \"%s\" is not contiguous\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->rank != t->rank) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] stable sequence \"%s\" associated with different ranks\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->soff + s->len == t->soff) {
				int32_t k, nv;
				const gfa_arc_t *av;
				nv = gfa_arc_n(g, (uint32_t)a[j-1]<<1);
				av = gfa_arc_a(g, (uint32_t)a[j-1]<<1);
				for (k = 0; k < nv; ++k)
					if (av[k].w == (uint32_t)a[j]<<1)
						break;
				if (s->rank == 0 && k == nv) {
					if (gfa_verbose >= 2)
						fprintf(stderr, "[W] nearby segments on rank-0 stable sequence \"%s\" are not connected\n", g->sseq[s->snid].name);
					goto end_check;
				}
				if (k == nv) ++n_sfa;
			} else ++n_sfa;
		}
	}

	// fill sfa[]
	*n_sfa_ = n_sfa;
	GFA_CALLOC(sfa, n_sfa);
	for (i = 0, k = 0; i < g->n_sseq; ++i) {
		int32_t jst;
		if (soff[i] == soff[i+1]) continue;
		for (j = soff[i] + 1, jst = j - 1; j <= soff[i+1]; ++j) {
			int32_t is_cont = 0;
			if (j < soff[i+1]) {
				const gfa_seg_t *s = &g->seg[(int32_t)a[j-1]];
				const gfa_seg_t *t = &g->seg[(int32_t)a[j]];
				if (s->soff + s->len == t->soff) {
					int32_t k, nv;
					const gfa_arc_t *av;
					nv = gfa_arc_n(g, (uint32_t)a[j-1]<<1);
					av = gfa_arc_a(g, (uint32_t)a[j-1]<<1);
					for (k = 0; k < nv; ++k)
						if (av[k].w == (uint32_t)a[j]<<1)
							break;
					if (k < nv) is_cont = 1;
				}
			}
			if (!is_cont) {
				int32_t l;
				const gfa_seg_t *s = &g->seg[(int32_t)a[jst]];
				gfa_sfa_t *p = &sfa[k++];
				assert(jst < j);
				p->snid = s->snid, p->soff = s->soff, p->rank = s->rank;
				p->end[0] = find_join(g, (uint32_t)a[jst]<<1|1);
				if (p->end[0] != (uint64_t)-1) p->end[0] ^= 1;
				p->end[1] = find_join(g, (uint32_t)a[j-1]<<1);
				for (l = jst, p->len = 0; l < j; ++l)
					p->len += g->seg[(int32_t)a[l]].len;
				if (write_seq) {
					GFA_MALLOC(p->seq, p->len + 1);
					for (l = jst, p->len = 0; l < j; ++l) {
						s = &g->seg[(int32_t)a[l]];
						memcpy(&p->seq[p->len], s->seq, s->len);
						p->len += s->len;
					}
					p->seq[p->len] = 0;
				}
				jst = j;
			}
		}
	}
	assert(k == n_sfa);

end_check:
	free(soff);
	free(a);
	return sfa;
}

const char *gfa_parse_reg(const char *s, int32_t *beg, int32_t *end)
{
	int32_t i, k, l, name_end;
	*beg = *end = -1;
	name_end = l = strlen(s);
	// determine the sequence name
	for (i = l - 1; i >= 0; --i) if (s[i] == ':') break; // look for colon from the end
	if (i >= 0) name_end = i;
	if (name_end < l) { // check if this is really the end
		int n_hyphen = 0;
		for (i = name_end + 1; i < l; ++i) {
			if (s[i] == '-') ++n_hyphen;
			else if (!isdigit(s[i]) && s[i] != ',') break;
		}
		if (i < l || n_hyphen > 1) name_end = l; // malformated region string; then take str as the name
	}
	// parse the interval
	if (name_end < l) {
		char *tmp, *tmp0;
		tmp0 = tmp = (char*)malloc(l - name_end + 1);
		for (i = name_end + 1, k = 0; i < l; ++i)
			if (s[i] != ',') tmp[k++] = s[i];
		tmp[k] = 0;
		if ((*beg = strtol(tmp, &tmp, 10) - 1) < 0) *beg = 0;
		*end = *tmp? strtol(tmp + 1, &tmp, 10) : 1<<29;
		if (*beg > *end) name_end = l;
		free(tmp0);
	}
	if (name_end == l) *beg = 0, *end = 1<<29;
	return s + name_end;
}

static char **gfa_append_list(char **a, uint32_t *n, uint32_t *m, const char *p)
{
	if (*n == *m) GFA_EXPAND(a, *m);
	a[(*n)++] = gfa_strdup(p);
	return a;
}

char **gfa_query_by_id(const gfa_t *g, int32_t n_bb, const gfa_bubble_t *bb, int32_t snid, int32_t start, int32_t end, int *n_seg_)
{ // TODO: This is an inefficient implementationg. Faster query requires to index the bubble intervals first.
	int32_t i, j, last = 0, bb_st = -1, bb_st_on = -1, bb_en = -1, bb_en_on = -1, bb_last = -1;
	uint32_t n_seg = 0, m_seg = 0;
	char **seg = 0;
	assert(start <= end && start >= 0);
	*n_seg_ = 0;
	for (i = 0; i < n_bb; ++i) {
		const gfa_bubble_t *b = &bb[i];
		if (i == 0 || bb[i].snid != bb[i-1].snid) last = 0;
		if (b->snid != snid) continue;
		assert(b->n_seg > 0);
		bb_last = i;
		if (last <= start && start < b->ss) {
			assert(bb_st < 0);
			bb_st = i, bb_st_on = 1;
		} else if (b->ss <= start && start < b->se) {
			bb_st = i, bb_st_on = 0;
		}
		if (last < end && end <= b->ss) {
			assert(bb_st >= 0);
			bb_en = i, bb_en_on = 1;
		} else if (b->ss < end && end <= b->se) {
			bb_en = i, bb_en_on = 0;
		}
		last = b->se;
	}
	if (bb_last < 0) return 0; // snid not found
	if (bb_st < 0) { // on the last stem
		const gfa_seg_t *s = &g->seg[bb[bb_last].v[bb[bb_last].n_seg - 1]>>1];
		assert(s->snid == snid && start >= s->soff);
		if (start < s->soff + s->len)
			seg = gfa_append_list(seg, &n_seg, &m_seg, s->name);
	} else if (bb_st_on && bb_st == bb_en && bb_en_on) { // on one stem
		seg = gfa_append_list(seg, &n_seg, &m_seg, g->seg[bb[bb_st].v[0]>>1].name);
	} else { // extract bubbles
		if (bb_en < 0) bb_en = bb_last;
		for (i = bb_st; i <= bb_en; ++i) {
			int32_t s = i == bb_st? 0 : 1;
			for (j = s; j < bb[i].n_seg; ++j)
				seg = gfa_append_list(seg, &n_seg, &m_seg, g->seg[bb[i].v[j]>>1].name);
		}
	}
	*n_seg_ = n_seg;
	return seg;
}

char **gfa_query_by_reg(const gfa_t *g, int32_t n_bb, const gfa_bubble_t *bb, const char *reg, int *n_seg)
{
	int32_t snid, start, end;
	const char *p;
	char *tmp;
	*n_seg = 0;
	p = gfa_parse_reg(reg, &start, &end);
	if (p == 0) return 0;
	tmp = gfa_strndup(reg, p - reg);
	snid = gfa_sseq_get(g, tmp);
	free(tmp);
	if (snid < 0) return 0;
	return gfa_query_by_id(g, n_bb, bb, snid, start, end, n_seg);
}
