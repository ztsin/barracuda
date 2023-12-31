#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bwtgap.h"
#include "bwtaln.h"

#define STDOUT_RESULT 0
#define USE_STACK_TABLE 0
#define MAX_NO_OF_GAP_ENTRIES 512

gap_stack_t *gap_init_stack(int max_mm, int max_gapo, int max_gape, const barracuda_gap_opt_t *opt)
{
	int i;
	gap_stack_t *stack;
	stack = (gap_stack_t*)calloc(1, sizeof(gap_stack_t));
	stack->n_stacks = aln_score(max_mm+1, max_gapo+1, max_gape+1, opt);
	stack->stacks = (gap_stack1_t*)calloc(stack->n_stacks, sizeof(gap_stack1_t));
	for (i = 0; i != stack->n_stacks; ++i) {
		gap_stack1_t *p = stack->stacks + i;
		p->m_entries = 4;
		p->stack = (gap_entry_t*)calloc(p->m_entries, sizeof(gap_entry_t));
	}
	return stack;
}


void gap_destroy_stack(gap_stack_t *stack)
{
	int i;
	for (i = 0; i != stack->n_stacks; ++i) free(stack->stacks[i].stack);
	free(stack->stacks);
	free(stack);
}

static void gap_reset_stack(gap_stack_t *stack)
{
	int i;
	for (i = 0; i != stack->n_stacks; ++i)
		stack->stacks[i].n_entries = 0;
	stack->best = stack->n_stacks;
	stack->n_entries = 0;
}


static inline void gap_push(gap_stack_t *stack, int a, int i, bwtint_t k, bwtint_t l, int n_mm, int n_gapo, int n_gape,
							int state, int is_diff, const barracuda_gap_opt_t *opt)
{

	int score;
	gap_entry_t *p;
	gap_stack1_t *q;
	score = aln_score(n_mm, n_gapo, n_gape, opt);
	q = &stack->stacks[score];
	if (q->n_entries == q->m_entries) {
		q->m_entries +=1;// <<= 1;
		q->stack = (gap_entry_t*)realloc(q->stack, sizeof(gap_entry_t) * q->m_entries);
	}
	p = q->stack + q->n_entries;
	p->length = i ; p->k = k; p->l = l;
	p->n_mm = n_mm; p->n_gapo = n_gapo; p->n_gape = n_gape; p->state = (a<<7)|state;
	if (is_diff) p->last_diff_pos = i;
	++(q->n_entries);
	++(stack->n_entries);
	if (stack->best > score) stack->best = score;

}


static inline void gap_pop(gap_stack_t *stack, gap_entry_t *e)
{
	gap_stack1_t *q;
	q = &stack->stacks[stack->best];
	*e = q->stack[q->n_entries - 1];
	--(q->n_entries);
	--(stack->n_entries);
	if (q->n_entries == 0 && stack->n_entries) { // reset best
		int i;
		for (i = stack->best + 1; i < stack->n_stacks; ++i)
			if (stack->stacks[i].n_entries != 0) break;
		stack->best = i;
	} else if (stack->n_entries == 0) stack->best = stack->n_stacks;
}

static inline void gap_shadow(int x, int len, bwtint_t max, int last_diff_pos, bwt_width_t *w)
{
	int i, j;
	for (i = j = 0; i < last_diff_pos; ++i) {
		if (w[i].w > x) w[i].w -= x;
		else if (w[i].w == x) {
			w[i].bid = 1;
			w[i].w = max - (++j);
		} // else should not happen
	}
}

static inline int int_log2(uint32_t v)
{
	int c = 0;
	if (v & 0xffff0000u) { v >>= 16; c |= 16; }
	if (v & 0xff00) { v >>= 8; c |= 8; }
	if (v & 0xf0) { v >>= 4; c |= 4; }
	if (v & 0xc) { v >>= 2; c |= 2; }
	if (v & 0x2) c |= 1;
	return c;
}

barracuda_aln1_t *bwt_match_gap(bwt_t *const bwts[2], int len, const ubyte_t *seq[2], bwt_width_t *w[2], bwt_width_t *seed_w[2], const barracuda_gap_opt_t *opt, int *_n_aln, gap_stack_t *stack)
{
	int best_score = aln_score(opt->max_diff+1, opt->max_gapo+1, opt->max_gape+1, opt);
	int best_diff = opt->max_diff + 1, max_diff = opt->max_diff;
	int best_cnt = 0;
	//int max_entries = 0;
	int j, _j, n_aln, m_aln;
	barracuda_aln1_t *aln;



	m_aln = 4; n_aln = 0;
	aln = (barracuda_aln1_t*)calloc(m_aln, sizeof(barracuda_aln1_t));

	//printf("Forward Sequence:");
	// check whether there are too many N
	for (j = _j = 0; j < len; ++j)
	{
		if (seq[0][j] > 3) ++_j;
	//	printf("%d", seq[0][j]);
	}
//	printf("\nReverse Sequence:");

	//for (j = 0; j < len; ++j)
//	{
	//	printf("%d", seq[1][j]);

//	}
	//printf("\n");
	if (_j > max_diff) {
		*_n_aln = n_aln;
		return aln;
	}

	//printf("max diff: %d\n", max_diff);
	//for (j = 0; j != len; ++j) printf("#0 %d: [%d,%u]\t[%d,%u]\n", j, w[0][j].bid, w[0][j].w, w[1][j].bid, w[1][j].w);
	gap_reset_stack(stack); // reset stack
	gap_push(stack, 0, len, 0, bwts[0]->seq_len, 0, 0, 0, 0, 0, opt);
	gap_push(stack, 1, len, 0, bwts[0]->seq_len, 0, 0, 0, 0, 0, opt);

	int loop_count = 0; //debug only
	while (stack->n_entries)
	{
		gap_entry_t e;
		int a, i, m, m_seed = 0, hit_found, allow_diff, allow_M, tmp;
		bwtint_t k, l, cnt_k[4], cnt_l[4], occ;
		const bwt_t *bwt;
		const ubyte_t *str;
		const bwt_width_t *seed_width = 0;
		bwt_width_t *width;
		loop_count ++;
		int worst_tolerated_score = best_score + opt->s_mm;
//		printf("best score %d, worst tolerated score %d\n", best_score, worst_tolerated_score);
//		printf("Entering loop %d, no of entries %d\n", loop_count, stack->n_entries); //debug only

		if (stack->n_entries > opt->max_entries) break;

//		if (stack->n_entries > max_entries) max_entries = stack->n_entries;

		gap_pop(stack, &e); // get the besqt entry

		k = e.k; l = e.l; // SA interval
		a = e.state>>7; i = e.length; // strand, length


		int score = aln_score(e.n_mm, e.n_gapo, e.n_gape, opt);

		if (!(opt->mode & BWA_MODE_NONSTOP) && score > worst_tolerated_score) break; // no need to proceed


//		printf("\nParent_1,");
//		printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, i, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);

		m = max_diff - (e.n_mm + e.n_gapo);
		if (opt->mode & BWA_MODE_GAPE) m -= e.n_gape;
		if (m < 0) continue;
		bwt = bwts[1-a]; str = seq[a]; width = w[a];

		if (seed_w) { // apply seeding
			seed_width = seed_w[a];
			m_seed = opt->max_seed_diff - (e.n_mm + e.n_gapo);
			if (opt->mode & BWA_MODE_GAPE) m_seed -= e.n_gape;
		}
		//printf("#1\t[%d,%d,%d,%c]\t[%d,%d,%d]\t[%u,%u]\t[%u,%u]\t%d\n", stack->n_entries, a, i, "MID"[e.state], e.n_mm, e.n_gapo, e.n_gape, width[i-1].bid, width[i-1].w, k, l, e.last_diff_pos);
		if (i > 0 && m < width[i-1].bid)
		{
			continue;
		}

		// check whether a hit is found
		hit_found = 0;
		if (i == 0) hit_found = 1;
		else if (m == 0 && ((e.state&0x7F) == STATE_M || (opt->mode&BWA_MODE_GAPE) || e.n_gape == opt->max_gape)) { // no diff allowed
			if (bwt_match_exact_alt(bwt, i, str, &k, &l)) hit_found = 1;
			else
			{
				continue; // no hit, skip
			}
		}

		if (hit_found) { // action for found hits

			int do_add = 1;
			//fprintf(stderr,"#2 hits found: %d:(%u,%u)\n", e.n_mm+e.n_gapo, k, l);
			//printf("#2 hits found: %d:(%u,%u)\n", e.n_mm+e.n_gapo, k, l);
			//printf("max_diff before %d,", max_diff);
			if (n_aln == 0) {
				best_score = score;
				best_diff = e.n_mm + e.n_gapo;
				if (opt->mode & BWA_MODE_GAPE) best_diff += e.n_gape;
				if (!(opt->mode & BWA_MODE_NONSTOP))
					max_diff = (best_diff + 1 > opt->max_diff)? opt->max_diff : best_diff + 1; // top2 behaviour
			}
			if (score == best_score) best_cnt += l - k + 1;
			else if (best_cnt > opt->max_top2)
				{
					break; // top2b behaviour
				}
			if (e.n_gapo) { // check whether the hit has been found. this may happen when a gap occurs in a tandem repeat
				// if this alignment was already found, do not add to alignment record array
				for (j = 0; j != n_aln; ++j)
					if (aln[j].k == k && aln[j].l == l)
						{
							break;
						}
				if (j < n_aln) do_add = 0;
			}

			if (do_add) { // append result the alignment record array

				barracuda_aln1_t *p;
				gap_shadow(l - k + 1, len, bwt->seq_len, e.last_diff_pos, width);
				if (n_aln == m_aln) {
					m_aln <<= 1;
					aln = (barracuda_aln1_t*)realloc(aln, m_aln * sizeof(barracuda_aln1_t));
					memset(aln + m_aln/2, 0, m_aln/2*sizeof(barracuda_aln1_t));
				}
				p = aln + n_aln;
				// record down number of mismatch, gap open, gap extension and a??
				p->n_mm = e.n_mm; p->n_gapo = e.n_gapo; p->n_gape = e.n_gape; p->a = a;
				// the suffix array interval
				p->k = k; p->l = l;
				// the score as a alignment record
				p->score = score;
				++n_aln;
			}
			continue;
		}

		--i;
		bwt_2occ4(bwt, k - 1, l, cnt_k, cnt_l); // retrieve Occurrence values
		occ = l - k + 1;

		// test whether difference is allowed
		allow_diff = allow_M = 1;

		if (i > 0) {
			int ii = i - (len - opt->seed_len);
			if (width[i-1].bid > m-1) allow_diff = 0;
			else if (width[i-1].bid == m-1 && width[i].bid == m-1 && width[i-1].w == width[i].w) allow_M = 0;

			if (seed_w && ii > 0) {
				if (seed_width[ii-1].bid > m_seed-1) allow_diff = 0;
				else if (seed_width[ii-1].bid == m_seed-1 && seed_width[ii].bid == m_seed-1
						 && seed_width[ii-1].w == seed_width[ii].w) allow_M = 0;
			}
		}

		// insertion and deletions
		tmp = (opt->mode & BWA_MODE_LOGGAP)? int_log2(e.n_gape + e.n_gapo)/2+1 : e.n_gapo + e.n_gape;
		if (allow_diff && i >= opt->indel_end_skip + tmp && len - i >= opt->indel_end_skip + tmp) {
			if ((e.state&0x7F) == STATE_M) { // gap open
				if (e.n_gapo < opt->max_gapo) { // gap open is allowed
				// insertion
//					printf("\nParent,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//					printf("daughter,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",a, i, k, l, e.n_mm, e.n_gapo + 1, e.n_gape, STATE_I);
					//if((score + opt->s_gapo+1 <= worst_tolerated_score) ||(opt->mode & BWA_MODE_NONSTOP))
					gap_push(stack, a, i, k, l, e.n_mm, e.n_gapo + 1, e.n_gape, STATE_I, 1, opt);

					// deletion
					for (j = 0; j != 4; ++j) {
						k = bwt->L2[j] + cnt_k[j] + 1;
						l = bwt->L2[j] + cnt_l[j];
						if (k <= l)
						{
//							printf("\nParent,");
//							printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//							printf("daughter,");
//							printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",1, i, k, l, e.n_mm, e.n_gapo+1, e.n_gape, STATE_D);
						//	if((score + opt->s_gapo <= worst_tolerated_score) ||(opt->mode & BWA_MODE_NONSTOP))
							gap_push(stack, a, i + 1, k, l, e.n_mm, e.n_gapo + 1, e.n_gape, STATE_D, 1, opt);
						}
					}
				}
			} else if ((e.state&0x7F) == STATE_I) { // Extension of an insertion
				if (e.n_gape < opt->max_gape) // gap extension is allowed
				{
//					printf("\nParent,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//					printf("daughter,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",a, i, k, l, e.n_mm, e.n_gapo, e.n_gape+1, STATE_I);
					//if((score + opt->s_gape <= worst_tolerated_score) ||(opt->mode & BWA_MODE_NONSTOP))
					gap_push(stack, a, i, k, l, e.n_mm, e.n_gapo, e.n_gape + 1, STATE_I, 1, opt);
				}

			} else if ((e.state&0x7F) == STATE_D) { // Extension of a deletion
				if (e.n_gape < opt->max_gape) { // gap extension is allowed
					if (e.n_gape + e.n_gapo < max_diff || occ < opt->max_del_occ) {
						for (j = 0; j != 4; ++j) {
							k = bwt->L2[j] + cnt_k[j] + 1;
							l = bwt->L2[j] + cnt_l[j];
							if (k <= l)
							{
//								printf("\nParent,");
//								printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//								printf("daughter,");
//								printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",a, i, k, l, e.n_mm, e.n_gapo, e.n_gape+1, STATE_D);
								//if((score + opt->s_gape <= worst_tolerated_score) ||(opt->mode & BWA_MODE_NONSTOP))
								gap_push(stack, a, i + 1, k, l, e.n_mm, e.n_gapo, e.n_gape + 1, STATE_D, 1, opt);
							}
						}
					}
				}
			}
		}

		// mismatches
		if (allow_diff && allow_M)
		{ // mismatch is allowed
			for (j = 1; j <= 4; ++j) {
				int c = (str[i] + j) & 3;
				int is_mm = (j != 4 || str[i] > 3);
				k = bwt->L2[c] + cnt_k[c] + 1;
				l = bwt->L2[c] + cnt_l[c];
				if (k <= l)
				{
//					printf("\nParent,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//					printf("daughter,");
//					printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",a, i, k, l, e.n_mm + is_mm, e.n_gapo, e.n_gape, STATE_M);
					if(((score + is_mm * opt->s_mm) <= worst_tolerated_score) ||(opt->mode & BWA_MODE_NONSTOP))
					gap_push(stack, a, i, k, l, e.n_mm + is_mm, e.n_gapo, e.n_gape, STATE_M, is_mm, opt); //these pushes four times?
				}

			}
		} else if (str[i] < 4) { // try exact match only

			int c = str[i] & 3;
			k = bwt->L2[c] + cnt_k[c] + 1;
			l = bwt->L2[c] + cnt_l[c];
			if (k <= l)
			{
//				printf("\nParent,");
//				printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d,",a, e.length, e.k, e.l, e.n_mm, e.n_gapo, e.n_gape, e.state&0x7F);
//				printf("daughter,");
//				printf("a: %d i: %d k: %u l: %u n_mm %d %d %d %d",a, i, k, l, e.n_mm, e.n_gapo, e.n_gape,  STATE_M);
				gap_push(stack, a, i, k, l, e.n_mm, e.n_gapo, e.n_gape, STATE_M, 0, opt);
			}

		}
	}
	*_n_aln = n_aln;
//	printf("max_entries %d:\n", max_entries);
//	printf("loop count: %d\n", loop_count);
	return aln;
}


