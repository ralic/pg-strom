/*
 * gpujoin.c
 *
 * GPU accelerated relations join, based on nested-loop or hash-join
 * algorithm.
 * ----
 * Copyright 2011-2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include <math.h>
#include "pg_strom.h"
#include "cuda_numeric.h"
#include "cuda_gpujoin.h"

/*
 * GpuJoinPath
 *
 *
 *
 */
typedef struct
{
	CustomPath		cpath;
	int				num_rels;
	Index			outer_relid;	/* valid, if outer scan pull-up */
	List		   *outer_quals;	/* qualifier of outer scan */
	cl_uint			outer_nrows_per_block;
	struct {
		JoinType	join_type;		/* one of JOIN_* */
		double		join_nrows;		/* intermediate nrows in this depth */
		Path	   *scan_path;		/* outer scan path */
		List	   *hash_quals;		/* valid quals, if hash-join */
		List	   *join_quals;		/* all the device quals, incl hash_quals */
		Size		ichunk_size;	/* expected inner chunk size */
		double		nloops_minor;	/* # of virtual segment of inner buffer */
		double		nloops_major;	/* # of physical split of inner buffer */
	} inners[FLEXIBLE_ARRAY_MEMBER];
} GpuJoinPath;

/*
 * GpuJoinInfo - private state object of CustomScan(GpuJoin)
 */
typedef struct
{
	int			num_rels;
	char	   *kern_source;
	int			extra_flags;
	List	   *used_params;
	List	   *outer_quals;
	double		outer_ratio;
	double		outer_nrows;		/* number of estimated outer nrows*/
	int			outer_width;		/* copy of @plan_width in outer path */
	Cost		outer_startup_cost;	/* copy of @startup_cost in outer path */
	Cost		outer_total_cost;	/* copy of @total_cost in outer path */
	cl_uint		outer_nrows_per_block;
	/* for each depth */
	List	   *plan_nrows_in;	/* list of floatVal for planned nrows_in */
	List	   *plan_nrows_out;	/* list of floatVal for planned nrows_out */
	List	   *ichunk_size;
	List	   *join_types;
	List	   *join_quals;
	List	   *other_quals;
	List	   *nloops_minor;
	List	   *nloops_major;
	List	   *hash_inner_keys;	/* if hash-join */
	List	   *hash_outer_keys;	/* if hash-join */
	/* supplemental information of ps_tlist */
	List	   *ps_src_depth;	/* source depth of the ps_tlist entry */
	List	   *ps_src_resno;	/* source resno of the ps_tlist entry */
	cl_uint		extra_maxlen;	/* max length of extra area per rows */
} GpuJoinInfo;

static inline void
form_gpujoin_info(CustomScan *cscan, GpuJoinInfo *gj_info)
{
	List	   *privs = NIL;
	List	   *exprs = NIL;

	privs = lappend(privs, makeInteger(gj_info->num_rels));
	privs = lappend(privs, makeString(pstrdup(gj_info->kern_source)));
	privs = lappend(privs, makeInteger(gj_info->extra_flags));
	exprs = lappend(exprs, gj_info->used_params);
	exprs = lappend(exprs, gj_info->outer_quals);
	privs = lappend(privs, pmakeFloat(gj_info->outer_ratio));
	privs = lappend(privs, pmakeFloat(gj_info->outer_nrows));
	privs = lappend(privs, makeInteger(gj_info->outer_width));
	privs = lappend(privs, pmakeFloat(gj_info->outer_startup_cost));
	privs = lappend(privs, pmakeFloat(gj_info->outer_total_cost));
	privs = lappend(privs, makeInteger(gj_info->outer_nrows_per_block));
	/* for each depth */
	privs = lappend(privs, gj_info->plan_nrows_in);
	privs = lappend(privs, gj_info->plan_nrows_out);
	privs = lappend(privs, gj_info->ichunk_size);
	privs = lappend(privs, gj_info->join_types);
	exprs = lappend(exprs, gj_info->join_quals);
	exprs = lappend(exprs, gj_info->other_quals);
	privs = lappend(privs, gj_info->nloops_minor);
	privs = lappend(privs, gj_info->nloops_major);
	exprs = lappend(exprs, gj_info->hash_inner_keys);
	exprs = lappend(exprs, gj_info->hash_outer_keys);

	privs = lappend(privs, gj_info->ps_src_depth);
	privs = lappend(privs, gj_info->ps_src_resno);
	privs = lappend(privs, makeInteger(gj_info->extra_maxlen));

	cscan->custom_private = privs;
	cscan->custom_exprs = exprs;
}

static inline GpuJoinInfo *
deform_gpujoin_info(CustomScan *cscan)
{
	GpuJoinInfo *gj_info = palloc0(sizeof(GpuJoinInfo));
	List	   *privs = cscan->custom_private;
	List	   *exprs = cscan->custom_exprs;
	int			pindex = 0;
	int			eindex = 0;

	gj_info->num_rels = intVal(list_nth(privs, pindex++));
	gj_info->kern_source = strVal(list_nth(privs, pindex++));
	gj_info->extra_flags = intVal(list_nth(privs, pindex++));
	gj_info->used_params = list_nth(exprs, eindex++);
	gj_info->outer_quals = list_nth(exprs, eindex++);
	gj_info->outer_ratio = floatVal(list_nth(privs, pindex++));
	gj_info->outer_nrows = floatVal(list_nth(privs, pindex++));
	gj_info->outer_width = intVal(list_nth(privs, pindex++));
	gj_info->outer_startup_cost = floatVal(list_nth(privs, pindex++));
	gj_info->outer_total_cost = floatVal(list_nth(privs, pindex++));
	gj_info->outer_nrows_per_block = intVal(list_nth(privs, pindex++));
	/* for each depth */
	gj_info->plan_nrows_in = list_nth(privs, pindex++);
	gj_info->plan_nrows_out = list_nth(privs, pindex++);
	gj_info->ichunk_size = list_nth(privs, pindex++);
	gj_info->join_types = list_nth(privs, pindex++);
    gj_info->join_quals = list_nth(exprs, eindex++);
	gj_info->other_quals = list_nth(exprs, eindex++);
	gj_info->nloops_minor = list_nth(privs, pindex++);
	gj_info->nloops_major = list_nth(privs, pindex++);
	gj_info->hash_inner_keys = list_nth(exprs, eindex++);
    gj_info->hash_outer_keys = list_nth(exprs, eindex++);

	gj_info->ps_src_depth = list_nth(privs, pindex++);
	gj_info->ps_src_resno = list_nth(privs, pindex++);
	gj_info->extra_maxlen = intVal(list_nth(privs, pindex++));
	Assert(pindex == list_length(privs));
	Assert(eindex == list_length(exprs));

	return gj_info;
}

/*
 * GpuJoinState - execution state object of GpuJoin
 */
struct pgstrom_multirels;

typedef struct
{
	/*
	 * Execution status
	 */
	PlanState		   *state;
	ExprContext		   *econtext;

	List			   *pds_list;
	cl_int				pds_index;
	Size				pds_limit;
	Size				consumed;
	Size				ntuples;
	/* temp store, if KDS-hash overflow */
	Tuplestorestate	   *tupstore;

	/*
	 * Join properties; both nest-loop and hash-join
	 */
	int					depth;
	JoinType			join_type;
	int					nbatches_plan;
	int					nbatches_exec;
	double				nrows_ratio;
	cl_uint				ichunk_size;
	List			   *join_quals;		/* single element list of ExprState */
	List			   *other_quals;	/* single element list of ExprState */

	/*
	 * Join properties; only hash-join
	 */
	cl_uint				hgram_shift;
	cl_uint				hgram_curr;
	cl_uint				hgram_width;
	Size			   *hgram_size;
	Size			   *hgram_nitems;
	List			   *hash_outer_keys;
	List			   *hash_inner_keys;
	List			   *hash_keylen;
	List			   *hash_keybyval;
	List			   *hash_keytype;

	/* CPU Fallback related */
	AttrNumber		   *inner_dst_resno;
	AttrNumber			inner_src_anum_min;
	AttrNumber			inner_src_anum_max;
	cl_long				fallback_inner_index;
	pg_crc32			fallback_inner_hash;
	cl_bool				fallback_inner_matched;
	cl_bool				fallback_right_outer;
} innerState;

/*
 * run-time statistics; to be allocated on the shared memory
 */
typedef struct
{
	int				num_rels;		/* number of inner relations */
	slock_t			lock;
	size_t			source_ntasks;	/* number of sampled tasks */
	size_t			source_nitems;	/* number of sampled source items */
	size_t			results_nitems;	/* number of joined result items */
	size_t			results_usage;	/* sum of kds_dst->usage */
	size_t		   *inner_nitems;	/* number of inner join results items */
	size_t		   *right_nitems;	/* number of right join results items */
	cl_double	   *row_dist_score;	/* degree of result row distribution */
	bool			row_dist_score_valid; /* true, if RDS is valid */
	size_t			inner_dma_nums;	/* number of inner DMA calls */
	size_t			inner_dma_size;	/* total length of inner DMA calls */
} runtimeStat;

typedef struct
{
	GpuTaskState_v2	gts;
	/* expressions to be used in fallback path */
	List		   *join_types;
	List		   *outer_quals;	/* list of ExprState */
	double			outer_ratio;
	double			outer_nrows;
	List		   *hash_outer_keys;
	List		   *join_quals;
	/* current window of inner relations */
	struct pgstrom_multirels *curr_pmrels;
	/* result width per tuple for buffer length calculation */
	int				result_width;
	/* expected extra length per result tuple  */
	cl_uint			extra_maxlen;

	/* buffer for row materialization  */
	HeapTupleData	curr_tuple;

	/*
	 * The first RIGHT OUTER JOIN depth, if any. It is a hint for optimization
	 * because it is obvious the shallower depth will produce no tuples when
	 * no input tuples are supplied.
	 */
	cl_int			first_right_outer_depth;

	/*
	 * flag to set if outer plan reached to end of the relation
	 *
	 * NOTE: Don't use gts->scan_done for this purpose, because it means
	 * end of the scan on this node itself. It indicates wrong state to
	 * the cuda_control.c
	 */
	bool			outer_scan_done;

	/*
	 * CPU Fallback
	 */
	TupleTableSlot *slot_fallback;
	ProjectionInfo *proj_fallback;		/* slot_fallback -> scan_slot */
	AttrNumber	   *outer_dst_resno;	/* destination attribute number to */
	AttrNumber		outer_src_anum_min;	/* be mapped on the slot_fallback */
	AttrNumber		outer_src_anum_max;
	cl_long			fallback_outer_index;

	/*
	 * Runtime statistics
	 */
	runtimeStat	   *rt_stat;

	/*
	 * Properties of underlying inner relations
	 */
	int				num_rels;
	bool			inner_preloaded;
	innerState		inners[FLEXIBLE_ARRAY_MEMBER];
} GpuJoinState;

/*
 * pgstrom_multirels - inner buffer of multiple PDS/KDSs
 */
typedef struct pgstrom_multirels
{
	GpuJoinState   *gjs;		/* GpuJoinState of this buffer */
	Size			head_length;	/* length of the header portion */
	Size			usage_length;	/* length actually in use */
	pgstrom_data_store **inner_chunks;	/* array of inner PDS */
	cl_bool			needs_outer_join;	/* true, if OJ is needed */
	/* fields below can be updated by both of backend / GPU server */
	slock_t			lock;
	cl_int			n_attached;	/* Number of attached tasks */
	cl_int			refcnt;		/* Refcount of device memory resource */
	CUdeviceptr		m_kmrels;	/* GPU memory for inner relations */
	CUevent			ev_loaded;	/* Sync object for load of pmrels */
	CUdeviceptr		m_ojmaps;	/* GPU memory for outer join map */
	cl_bool		   *h_ojmaps;	/* Host memory for outer join map */
	kern_multirels	kern;
} pgstrom_multirels;

/*
 * pgstrom_gpujoin - task object of GpuJoin
 */
typedef struct
{
	GpuTask_v2		task;
	CUfunction		kern_main;
	CUdeviceptr		m_kgjoin;
	CUdeviceptr		m_kmrels;
	CUdeviceptr		m_kds_src;
	CUdeviceptr		m_kds_dst;
	CUdeviceptr		m_ojmaps;
	cl_bool			is_inner_loader;
	cl_bool			with_nvme_strom;
	runtimeStat	   *rt_stat;
	/* DMA buffers */
	pgstrom_multirels  *pmrels;		/* inner multi relations (heap or hash) */
	pgstrom_data_store *pds_src;	/* data store of outer relation */
	pgstrom_data_store *pds_dst;	/* data store of result buffer */
	kern_gpujoin	kern;			/* kern_gpujoin of this request */
} pgstrom_gpujoin;

/* static variables */
static set_join_pathlist_hook_type set_join_pathlist_next;
static CustomPathMethods	gpujoin_path_methods;
static CustomScanMethods	gpujoin_plan_methods;
static CustomExecMethods	gpujoin_exec_methods;
static bool					enable_gpunestloop;
static bool					enable_gpuhashjoin;

/* static functions */
static GpuTask_v2 *gpujoin_next_task(GpuTaskState_v2 *gts);
static void gpujoin_ready_task(GpuTaskState_v2 *gts, GpuTask_v2 *gtask);
static void gpujoin_switch_task(GpuTaskState_v2 *gts, GpuTask_v2 *gtask);
static TupleTableSlot *gpujoin_next_tuple(GpuTaskState_v2 *gts);
static TupleTableSlot *gpujoin_next_tuple_fallback(GpuJoinState *gjs,
												   pgstrom_gpujoin *pgjoin);
static pg_crc32 get_tuple_hashvalue(innerState *istate,
									bool is_inner_hashkeys,
									TupleTableSlot *slot,
									bool *p_is_null_keys);

static char *gpujoin_codegen(PlannerInfo *root,
							 CustomScan *cscan,
							 GpuJoinInfo *gj_info,
							 List *tlist,
							 codegen_context *context);

static void gpujoin_inner_unload(GpuJoinState *gjs, bool needs_rescan);
static pgstrom_multirels *gpujoin_inner_getnext(GpuJoinState *gjs);
static pgstrom_multirels *multirels_attach_buffer(pgstrom_multirels *pmrels);
static bool multirels_get_buffer(pgstrom_gpujoin *pgjoin,
								 CUstream cuda_stream);
static void multirels_put_buffer(pgstrom_gpujoin *pgjoin);

static void colocate_outer_join_maps_to_host(pgstrom_multirels *pmrels);
static void colocate_outer_join_maps_to_device(pgstrom_gpujoin *pgjoin,
											   CUmodule cuda_module,
											   CUstream cuda_stream);
static void multirels_detach_buffer(GpuJoinState *gjs,
									pgstrom_multirels *pmrels,
									bool may_kick_outer_join);
/*
 * misc declarations
 */

/* copied from joinpath.c */
#define PATH_PARAM_BY_REL(path, rel)  \
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path), (rel)->relids))

/*
 * returns true, if pathnode is GpuJoin
 */
bool
pgstrom_path_is_gpujoin(Path *pathnode)
{
	CustomPath *cpath = (CustomPath *) pathnode;

	if (IsA(cpath, CustomPath) &&
		cpath->methods == &gpujoin_path_methods)
		return true;
	return false;
}

/*
 * returns true, if plannode is GpuJoin
 */
bool
pgstrom_plan_is_gpujoin(const Plan *plannode)
{
	CustomScan *cscan = (CustomScan *) plannode;

	if (IsA(cscan, CustomScan) &&
		cscan->methods == &gpujoin_plan_methods)
		return true;
	return false;
}

/*
 * dump_gpujoin_path
 *
 * Dumps candidate GpuJoinPath for debugging
 */
static void
__dump_gpujoin_path(StringInfo buf, PlannerInfo *root, Path *pathnode)
{
	RelOptInfo *rel = pathnode->parent;
	Relids		relids = rel->relids;
	List	   *range_tables = root->parse->rtable;
	int			rtindex = -1;
	bool		is_first = true;


	if (rel->reloptkind != RELOPT_BASEREL)
		appendStringInfo(buf, "(");

	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
	{
		RangeTblEntry  *rte = rt_fetch(rtindex, range_tables);
		Alias		   *eref = rte->eref;

		appendStringInfo(buf, "%s%s",
						 is_first ? "" : ", ",
						 eref->aliasname);
		is_first = false;
	}

	if (rel->reloptkind != RELOPT_BASEREL)
		appendStringInfo(buf, ")");
}

/*
 * estimate_buffersize_gpujoin 
 *
 * Top half of cost_gpujoin - we determine expected buffer consumption.
 * If inner relations buffer is too large, we must split pmrels on
 * preloading. If result is too large, we must split range of inner
 * chunks logically.
 */
static bool
estimate_buffersize_gpujoin(PlannerInfo *root,
							RelOptInfo *joinrel,
							Path *outer_path,
							GpuJoinPath *gpath,
							double num_chunks)
{
	PathTarget *join_reltarget = joinrel->reltarget;
	Size		inner_limit_sz;
	Size		inner_total_sz;
	double		prev_nloops_minor;
	double		curr_nloops_minor;
	Size		largest_chunk_size = 0;
	cl_int		largest_chunk_index = -1;
	Size		largest_growth_ntuples = 0.0;
	cl_int		largest_growth_index = -1;
	Size		buffer_size;
	double		inner_ntuples;
	double		join_ntuples;
	double		prev_ntuples;
	cl_int		ncols;
	cl_int		i, num_rels = gpath->num_rels;

	/* init number of loops */
	for (i=0; i < num_rels; i++)
	{
		gpath->inners[i].nloops_minor = 1.0;
		gpath->inners[i].nloops_major = 1.0;
	}

	/*
	 * Estimation: size of multi relational inner buffer
	 */
retry_major:
	prev_nloops_minor = 1;
	largest_chunk_size = 0;
	largest_chunk_index = -1;
	largest_growth_ntuples = 0.0;
	largest_growth_index = -1;

	inner_total_sz = STROMALIGN(offsetof(kern_multirels,
										 chunks[num_rels]));
	prev_ntuples = outer_path->rows / num_chunks;
	for (i=0; i < num_rels; i++)
	{
		Path	   *inner_path = gpath->inners[i].scan_path;
		RelOptInfo *inner_rel = inner_path->parent;
		PathTarget *inner_reltarget = inner_rel->reltarget;
		Size		chunk_size;
		Size		entry_size;
		Size		num_items;

	retry_minor:
		/* total number of inner nloops until this depth */
		curr_nloops_minor = (prev_nloops_minor *
							 gpath->inners[i].nloops_minor);

		/* force a plausible relation size if no information. */
		inner_ntuples = Max(inner_path->rows *
							pgstrom_chunk_size_margin /
							gpath->inners[i].nloops_major,
							100.0);

		/*
		 * NOTE: PathTarget->width is not reliable for base relations 
		 * because this fields shows the length of attributes which
		 * are actually referenced, however, we usually load physical
		 * tuples on the KDS/KHash buffer if base relation.
		 */
		ncols = list_length(inner_reltarget->exprs);

		if (gpath->inners[i].hash_quals != NIL)
			entry_size = offsetof(kern_hashitem, t.htup);
		else
			entry_size = offsetof(kern_tupitem, htup);

		entry_size += MAXALIGN(offsetof(HeapTupleHeaderData,
									   t_bits[BITMAPLEN(ncols)]));
		if (inner_rel->reloptkind != RELOPT_BASEREL)
			entry_size += MAXALIGN(inner_reltarget->width);
		else
		{
			entry_size += MAXALIGN(((double)(BLCKSZ -
											 SizeOfPageHeaderData)
									* inner_rel->pages
									/ Max(inner_rel->tuples, 1.0))
								   - sizeof(ItemIdData)
								   - SizeofHeapTupleHeader);
		}

		/*
		 * inner chunk size estimation
		 */
		chunk_size = KDS_CALCULATE_HASH_LENGTH(ncols,
											   (Size)inner_ntuples,
											   entry_size *
											   (Size)inner_ntuples);
		gpath->inners[i].ichunk_size = chunk_size;

		if (largest_chunk_index < 0 || largest_chunk_size < chunk_size)
		{
			largest_chunk_size = chunk_size;
			largest_chunk_index = i;
		}
		inner_total_sz += chunk_size;

		/*
		 * NOTE: The number of intermediation result of GpuJoin has to
		 * fit pgstrom_chunk_size(). If too large number of rows are
		 * expected, we try to run same chunk multiple times with
		 * smaller inner_size[].
		 */
		join_ntuples = (gpath->inners[i].join_nrows /
						(double)(num_chunks * curr_nloops_minor));
		num_items = (Size)((double)(i+2) * join_ntuples *
						   pgstrom_chunk_size_margin);
		buffer_size = offsetof(kern_gpujoin, jscale[num_rels + 1])
			+ BLCKSZ	/* alternative of kern_parambuf */
			+ STROMALIGN(offsetof(kern_resultbuf, results[num_items]))
			+ STROMALIGN(offsetof(kern_resultbuf, results[num_items]));
		if (buffer_size > pgstrom_chunk_size())
		{
			Size	nsplit_minor = (buffer_size / pgstrom_chunk_size()) + 1;

			if (nsplit_minor > INT_MAX)
			{
				elog(DEBUG1, "Too large kgjoin {nitems=%zu size=%zu}",
					 num_items, buffer_size);
				/*
				 * NOTE: Heuristically, it is not a reasonable plan to
				 * expect massive amount of intermediation result items.
				 * It will lead very large ammount of minor iteration
				 * for GpuJoin kernel invocations. So, we bail out this
				 * plan immediately.
				 */
				return false;
			}
			gpath->inners[i].nloops_minor *= nsplit_minor;
			goto retry_minor;
		}

		if (largest_growth_index < 0 ||
			join_ntuples - prev_ntuples > largest_growth_ntuples)
		{
			largest_growth_index = i;
			largest_growth_ntuples = join_ntuples - prev_ntuples;
		}
		prev_nloops_minor = curr_nloops_minor;
		prev_ntuples = join_ntuples;
	}

	/*
	 * NOTE:If expected consumption of destination buffer exceeds the
	 * limitation, we logically divide an inner chunk (with largest
	 * growth ratio) and run GpuJoin task multiple times towards same
	 * data set.
	 * At this moment, we cannot determine which result format shall
	 * be used (KDS_FORMAT_ROW or KDS_FORMAT_SLOT), so we adopt the
	 * larger one, for safety.
	 */
	Assert(gpath->inners[num_rels-1].join_nrows == gpath->cpath.path.rows);
	join_ntuples = gpath->cpath.path.rows / (double)(num_chunks *
													 prev_nloops_minor);
	ncols = list_length(join_reltarget->exprs);
	buffer_size = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
	buffer_size += (Max(LONGALIGN((sizeof(Datum) + sizeof(char)) * ncols),
						MAXALIGN(offsetof(kern_tupitem, htup) +
								 join_reltarget->width) + sizeof(cl_uint))
					* (Size) join_ntuples);
	if (buffer_size > pgstrom_chunk_size_limit())
	{
		double	nloops_minor_next;

		Assert(largest_growth_index >= 0 &&
			   largest_growth_index < num_rels);

		nloops_minor_next = gpath->inners[largest_growth_index].nloops_minor
			* (double)((buffer_size / pgstrom_chunk_size_limit()) + 1);
		if (nloops_minor_next > (double) INT_MAX)
		{
			elog(DEBUG1, "Too large KDS-Dest {nrooms=%zu size=%zu}",
				 (Size) join_ntuples, (Size) buffer_size);
			return false;
		}
		gpath->inners[largest_growth_index].nloops_minor *= nloops_minor_next;
		goto retry_major;
	}

	/*
	 * NOTE: If total size of inner multi-relations buffer is out of
	 * range, we have to split inner buffer multiple portions to fit
	 * GPU RAMs. It is a restriction come from H/W capability.
	 *
	 * Also note that the estimated inner_total_sz can be extremely
	 * large, so it often leads 32bit integer overflow. Please be
	 * careful.
	 */
	inner_limit_sz = Min(gpuMemMaxAllocSize() / 2,
						 dmaBufferMaxAllocSize()) - BLCKSZ * num_rels;
	if (inner_total_sz > inner_limit_sz)
	{
		double	nloops_major_next;

		Assert(largest_chunk_index >= 0 &&
			   largest_chunk_index < num_rels);

		nloops_major_next = gpath->inners[largest_chunk_index].nloops_major
			* (double)(inner_total_sz / inner_limit_sz + 1);
		if (nloops_major_next > (double) INT_MAX)
		{
			elog(DEBUG1, "Too large Inner multirel buffer {size=%zu}",
				 (Size) inner_total_sz);
			return false;
		}
		gpath->inners[largest_chunk_index].nloops_major = nloops_major_next;
		goto retry_major;
	}
	return true;	/* probably, reasonable plan for buffer usage */
}

/*
 * cost_gpujoin
 *
 * estimation of GpuJoin cost
 */
static bool
cost_gpujoin(PlannerInfo *root,
			 GpuJoinPath *gpath,
			 RelOptInfo *joinrel,
			 List *final_tlist,
			 Path *outer_path,
			 Relids required_outer,
			 int parallel_nworkers)
{
	PathTarget *join_reltarget = joinrel->reltarget;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		run_cost_per_chunk = 0.0;
	Cost		startup_delay;
	QualCost   *join_cost;
	Size		inner_total_sz = 0;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	double		parallel_divisor = 1.0;
	double		num_chunks;
	double		chunk_ntuples;
	double		total_nloops_minor = 1.0;	/* loops by kds_dst overflow */
	double		total_nloops_major = 1.0;	/* loops by pmrels overflow */
	int			i, num_rels = gpath->num_rels;

	/*
	 * Cost comes from the outer-path
	 */
	if (gpath->outer_relid > 0)
	{
		double		dummy;

		cost_gpuscan_common(root,
							outer_path->parent,
							gpath->outer_quals,
							parallel_nworkers,
							&parallel_divisor,
							&dummy,		/* equivalent to outer_path->rows */
							&num_chunks,
							&gpath->outer_nrows_per_block,
							&startup_cost,
							&run_cost);
	}
	else
	{
		startup_cost = pgstrom_gpu_setup_cost + outer_path->startup_cost;
		run_cost = outer_path->total_cost - outer_path->startup_cost;
		num_chunks = estimate_num_chunks(outer_path);
	}

	/*
	 * Estimation of inner / destination buffer consumption
	 */
	if (!estimate_buffersize_gpujoin(root, joinrel,
									 outer_path, gpath, num_chunks))
		return false;

	for (i=0; i < num_rels; i++)
	{
		total_nloops_major *= gpath->inners[i].nloops_major;
		total_nloops_minor *= gpath->inners[i].nloops_minor;
	}

	/*
	 * Cost of per-tuple evaluation
	 */
	join_cost = palloc0(sizeof(QualCost) * num_rels);
	for (i=0; i < num_rels; i++)
	{
		cost_qual_eval(&join_cost[i], gpath->inners[i].join_quals, root);
		join_cost[i].per_tuple *= gpu_ratio;
	}

	/*
	 * Cost for each depth
	 */
	chunk_ntuples = outer_path->rows / num_chunks;
	for (i=0; i < num_rels; i++)
	{
		Path	   *scan_path = gpath->inners[i].scan_path;

		/* cost to load all the tuples from inner-path */
		startup_cost += scan_path->total_cost;

		/* cost for join_qual startup */
		startup_cost += join_cost[i].startup;

		/*
		 * cost to evaluate join qualifiers according to
		 * the GpuJoin logic
		 */
		if (gpath->inners[i].hash_quals != NIL)
		{
			/*
			 * GpuHashJoin - It computes hash-value of inner tuples by CPU,
			 * but outer tuples by GPU, then it evaluates join-qualifiers
			 * for each items on inner hash table by GPU.
			 */
			List	   *hash_quals = gpath->inners[i].hash_quals;
			cl_uint		num_hashkeys = list_length(hash_quals);
			double		hash_nsteps = scan_path->rows /
				(double)__KDS_NSLOTS((Size)scan_path->rows);

			/* cost to compute inner hash value by CPU */
			startup_cost += (cpu_operator_cost * num_hashkeys *
							 scan_path->rows);
			/* cost to compute hash value by GPU */
			run_cost_per_chunk += (pgstrom_gpu_operator_cost *
								   num_hashkeys *
								   chunk_ntuples);
			/* cost to evaluate join qualifiers */
			run_cost_per_chunk += (join_cost[i].per_tuple *
								   chunk_ntuples *
								   Max(hash_nsteps, 1.0));
		}
		else
		{
			/*
			 * GpuNestLoop - It evaluates join-qual for each pair of outer
			 * and inner tuples. So, its run_cost is usually higher than
			 * GpuHashJoin.
			 */
			double		inner_ntuples = scan_path->rows
				/ (gpath->inners[i].nloops_major *
				   gpath->inners[i].nloops_minor);

			/* cost to load inner heap tuples by CPU */
			startup_cost += cpu_tuple_cost * scan_path->rows;

			/* cost to evaluate join qualifiers */
			run_cost_per_chunk += (join_cost[i].per_tuple *
								   chunk_ntuples *
								   clamp_row_est(inner_ntuples));
		}
		/* number of outer items on the next depth */
		chunk_ntuples = (gpath->inners[i].join_nrows /
						 ((double) num_chunks *
						  gpath->inners[i].nloops_minor));

		/* consider inner chunk size to be sent over DMA */
		inner_total_sz += gpath->inners[i].ichunk_size;
	}
	/* total GPU execution cost */
	run_cost += (run_cost_per_chunk *
				 (double) num_chunks *
				 (double) total_nloops_minor);
	/*
	 * cost to sent inner/outer chunks; we assume 20% of kernel task call
	 * also involve DMA of inner multi-relations buffer
	 */
	/* outer DMA cost */
	run_cost += (double)num_chunks * pgstrom_gpu_dma_cost;
	/* inner DMA cost */
	run_cost += ((double)inner_total_sz / (double)pgstrom_chunk_size() *
				 (double)num_chunks * pgstrom_gpu_dma_cost *
				 total_nloops_minor * 0.20);
	/*
	 * Major inner split makes iteration of entire process multiple times
	 */
	run_cost *= total_nloops_major;

	/*
	 * cost discount by GPU projection, if this join is the last level
	 */
	if (final_tlist != NIL)
	{
		Cost		discount_per_tuple = 0.0;
		Cost		discount_total;
		QualCost	qcost;
		cl_uint		num_vars = 0;
		ListCell   *lc;

		foreach (lc, final_tlist)
		{
			TargetEntry	   *tle = lfirst(lc);

			if (IsA(tle->expr, Var) ||
				IsA(tle->expr, Const) ||
				IsA(tle->expr, Param))
				num_vars++;
			else if (pgstrom_device_expression(tle->expr))
            {
                cost_qual_eval_node(&qcost, (Node *)tle->expr, root);
                discount_per_tuple += (qcost.per_tuple *
                                       Max(1.0 - gpu_ratio, 0.0) / 10.0);
                num_vars++;
            }
            else
            {
				List	   *vars_list
					= pull_vars_of_level((Node *)tle->expr, 0);
				num_vars += list_length(vars_list);
				list_free(vars_list);
			}
		}

		if (num_vars > list_length(join_reltarget->exprs))
			discount_per_tuple -= cpu_tuple_cost *
				(double)(num_vars - list_length(join_reltarget->exprs));
		discount_total = Max(discount_per_tuple, 0.0) * joinrel->rows;

		run_cost = Max(run_cost - discount_total, 0.0);
	}

	/*
	 * delay to fetch the first tuple
	 */
	startup_delay = run_cost * (1.0 / (double)(num_chunks));

	/*
	 * cost of final materialization, but GPU does projection
	 */
	run_cost += cpu_tuple_cost * gpath->cpath.path.rows;

	/*
	 * Put cost value on the gpath.
	 */
	gpath->cpath.path.startup_cost = startup_cost + startup_delay;
	gpath->cpath.path.total_cost = startup_cost + run_cost;

	/*
	 * NOTE: If very large number of rows are estimated, it may cause
	 * overflow of variables, then makes nearly negative infinite cost
	 * even though the plan is very bad.
	 * At this moment, we put assertion to detect it.
	 */
	Assert(gpath->cpath.path.startup_cost >= 0.0 &&
		   gpath->cpath.path.total_cost >= 0.0);

	if (add_path_precheck(gpath->cpath.path.parent,
						  gpath->cpath.path.startup_cost,
						  gpath->cpath.path.total_cost,
						  NULL, required_outer))
	{
		/* Dumps candidate GpuJoinPath for debugging */
		if (client_min_messages <= DEBUG1)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			__dump_gpujoin_path(&buf, root, outer_path);
			for (i=0; i < gpath->num_rels; i++)
			{
				JoinType	join_type = gpath->inners[i].join_type;
				Path	   *inner_path = gpath->inners[i].scan_path;
				bool		is_nestloop = (gpath->inners[i].hash_quals == NIL);

				appendStringInfo(&buf, " %s%s ",
								 join_type == JOIN_FULL ? "F" :
								 join_type == JOIN_LEFT ? "L" :
								 join_type == JOIN_RIGHT ? "R" : "I",
								 is_nestloop ? "NL" : "HJ");

				__dump_gpujoin_path(&buf, root, inner_path);
			}
			elog(DEBUG1, "GpuJoin: %s Cost=%.2f..%.2f",
				 buf.data,
				 gpath->cpath.path.startup_cost,
				 gpath->cpath.path.total_cost);
			pfree(buf.data);
		}
		return true;
	}
	return false;
}

typedef struct
{
	JoinType	join_type;
	Path	   *inner_path;
	List	   *join_quals;
	List	   *hash_quals;
	double		join_nrows;
} inner_path_item;

static GpuJoinPath *
create_gpujoin_path(PlannerInfo *root,
					RelOptInfo *joinrel,
					Path *outer_path,
					List *inner_path_items_list,
					List *final_tlist,
					ParamPathInfo *param_info,
					Relids required_outer,
					bool try_parallel_path)
{
	GpuJoinPath *gjpath;
	cl_int		num_rels = list_length(inner_path_items_list);
	ListCell   *lc;
	int			parallel_nworkers = 0;
	bool		inner_parallel_safe = true;
	bool		parallel_aware = false;
	int			i;

	/* parallel path must have parallel_safe sub-paths */
	if (try_parallel_path)
	{
		if (!outer_path->parallel_safe)
			return NULL;
		foreach (lc, inner_path_items_list)
		{
			inner_path_item *ip_item = lfirst(lc);

			if (!ip_item->inner_path->parallel_safe)
				return NULL;
		}
		parallel_nworkers = outer_path->parallel_workers;
	}

	gjpath = palloc0(offsetof(GpuJoinPath, inners[num_rels + 1]));
	NodeSetTag(gjpath, T_CustomPath);
	gjpath->cpath.path.pathtype = T_CustomScan;
	gjpath->cpath.path.parent = joinrel;
	gjpath->cpath.path.pathtarget = joinrel->reltarget;
	gjpath->cpath.path.param_info = param_info;	// XXXXXX
	gjpath->cpath.path.pathkeys = NIL;
	gjpath->cpath.path.rows = joinrel->rows;
	gjpath->cpath.flags = 0;
	gjpath->cpath.methods = &gpujoin_path_methods;
	gjpath->outer_relid = 0;
	gjpath->outer_quals = NIL;
	gjpath->num_rels = num_rels;

	i = 0;
	foreach (lc, inner_path_items_list)
	{
		inner_path_item *ip_item = lfirst(lc);
		List	   *hash_quals;

		if (enable_gpuhashjoin && ip_item->hash_quals != NIL)
			hash_quals = ip_item->hash_quals;
		else if (enable_gpunestloop &&
				 (ip_item->join_type == JOIN_INNER ||
				  ip_item->join_type == JOIN_LEFT))
			hash_quals = NIL;
		else
		{
			pfree(gjpath);
			return NULL;
		}
		if (!ip_item->inner_path->parallel_safe)
			inner_parallel_safe = false;
		gjpath->inners[i].join_type = ip_item->join_type;
		gjpath->inners[i].join_nrows = ip_item->join_nrows;
		gjpath->inners[i].scan_path = ip_item->inner_path;
		gjpath->inners[i].hash_quals = hash_quals;
		gjpath->inners[i].join_quals = ip_item->join_quals;
		gjpath->inners[i].ichunk_size = 0;		/* to be set later */
		gjpath->inners[i].nloops_minor = 1.0;	/* to be set later */
		gjpath->inners[i].nloops_major = 1.0;	/* to be set later */
		i++;
	}
	Assert(i == num_rels);

	/* Try to pull up outer scan if enough simple */
	pgstrom_pullup_outer_scan(outer_path,
							  &gjpath->outer_relid,
							  &gjpath->outer_quals,
							  &parallel_aware);

	/*
	 * cost calculation of GpuJoin, then, add this path to the joinrel,
	 * unless its cost is not obviously huge.
	 */
	if (cost_gpujoin(root,
					 gjpath,
					 joinrel,
					 final_tlist,
					 outer_path,
					 required_outer,
					 parallel_nworkers))
	{
		List   *custom_paths = list_make1(outer_path);

		/* informs planner a list of child pathnodes */
		for (i=0; i < num_rels; i++)
			custom_paths = lappend(custom_paths, gjpath->inners[i].scan_path);
		gjpath->cpath.custom_paths = custom_paths;
		gjpath->cpath.path.parallel_aware = parallel_aware;
		gjpath->cpath.path.parallel_safe = (joinrel->consider_parallel &&
											outer_path->parallel_safe &&
											inner_parallel_safe);
		if (!gjpath->cpath.path.parallel_safe)
			gjpath->cpath.path.parallel_workers = 0;
		else
			gjpath->cpath.path.parallel_workers = parallel_nworkers;
		return gjpath;
	}
	pfree(gjpath);
	return NULL;
}

/*
 * gpujoin_find_cheapest_path
 *
 * finds the cheapest path-node but not parameralized by other relations
 * involved in this GpuJoin.
 */
static Path *
gpujoin_find_cheapest_path(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *inputrel,
						   bool only_parallel_safe)
{
	Path	   *input_path = inputrel->cheapest_total_path;
	Relids		other_relids;
	ListCell   *lc;

	other_relids = bms_difference(joinrel->relids, inputrel->relids);
	if ((only_parallel_safe && !input_path->parallel_safe) ||
		bms_overlap(PATH_REQ_OUTER(input_path), other_relids))
	{
		/*
		 * We try to find out the second best path if cheapest path is not
		 * sufficient for the requiement of GpuJoin
		 */
		foreach (lc, inputrel->pathlist)
		{
			Path   *curr_path = lfirst(lc);

			if (only_parallel_safe && !curr_path->parallel_safe)
				continue;
			if (bms_overlap(PATH_REQ_OUTER(curr_path), other_relids))
				continue;
			if (input_path == NULL ||
				input_path->total_cost > curr_path->total_cost)
				input_path = curr_path;
		}
	}
	bms_free(other_relids);

	return input_path;
}

/*
 * extract_gpuhashjoin_quals - pick up qualifiers usable for GpuHashJoin
 */
static List *
extract_gpuhashjoin_quals(PlannerInfo *root,
						  RelOptInfo *outer_rel,
						  RelOptInfo *inner_rel,
						  JoinType jointype,
						  List *restrict_clauses)
{
	List	   *hash_quals = NIL;
	ListCell   *lc;

	foreach (lc, restrict_clauses)
	{
		RestrictInfo   *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * If processing an outer join, only use its own join clauses
		 * for hashing.  For inner joins we need not be so picky.
		 */
		if (IS_OUTER_JOIN(jointype) && rinfo->is_pushed_down)
			continue;

		/* Is it hash-joinable clause? */
		if (!rinfo->can_join || !OidIsValid(rinfo->hashjoinoperator))
			continue;

		/*
		 * Check if clause has the form "outer op inner" or
		 * "inner op outer". If suitable, we may be able to choose
		 * GpuHashJoin logic. See clause_sides_match_join also.
		 */
		if ((bms_is_subset(rinfo->left_relids,  outer_rel->relids) &&
			 bms_is_subset(rinfo->right_relids, inner_rel->relids)) ||
			(bms_is_subset(rinfo->left_relids,  inner_rel->relids) &&
			 bms_is_subset(rinfo->right_relids, outer_rel->relids)))
		{
			/* OK, it is hash-joinable qualifier */
			hash_quals = lappend(hash_quals, rinfo);
		}
	}
	return hash_quals;
}

/*
 * try_add_gpujoin_paths
 */
static void
try_add_gpujoin_paths(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  List *final_tlist,
					  Path *outer_path,
					  Path *inner_path,
					  JoinType join_type,
					  JoinPathExtraData *extra,
					  bool try_parallel_path)
{
	Relids			required_outer;
	ParamPathInfo  *param_info;
	inner_path_item *ip_item;
	List		   *ip_items_list;
	List		   *restrict_clauses = extra->restrictlist;
	ListCell	   *lc;

	/* Quick exit if unsupported join type */
	if (join_type != JOIN_INNER &&
		join_type != JOIN_FULL &&
		join_type != JOIN_RIGHT &&
		join_type != JOIN_LEFT)
		return;

	/*
	 * Check to see if proposed path is still parameterized, and reject
	 * if the parameterization wouldn't be sensible.
	 * Note that GpuNestLoop does not support parameterized nest-loop,
	 * only cross-join or non-symmetric join are supported, therefore,
	 * calc_non_nestloop_required_outer() is sufficient.
	 */
	required_outer = calc_non_nestloop_required_outer(outer_path,
													  inner_path);
	if (required_outer &&
		!bms_overlap(required_outer, extra->param_source_rels))
	{
		bms_free(required_outer);
		return;
	}

	/*
	 * Get param info
	 */
	param_info = get_joinrel_parampathinfo(root,
										   joinrel,
										   outer_path,
										   inner_path,
										   extra->sjinfo,
										   required_outer,
										   &restrict_clauses);
	/*
	 * It makes no sense to run cross join on GPU devices without
	 * GPU projection opportunity.
	 */
	if (!final_tlist && !restrict_clauses)
		return;

	/*
	 * All the join-clauses must be executable on GPU device.
	 * Even though older version supports HostQuals to be
	 * applied post device join, it leads undesirable (often
	 * unacceptable) growth of the result rows in device join.
	 * So, we simply reject any join that contains host-only
	 * qualifiers.
	 */
	foreach (lc, restrict_clauses)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		if (!pgstrom_device_expression(rinfo->clause))
			return;
	}

	/*
	 * setup inner_path_item
	 */
	ip_item = palloc0(sizeof(inner_path_item));
	ip_item->join_type = join_type;
	ip_item->inner_path = inner_path;
	ip_item->join_quals = restrict_clauses;
	ip_item->hash_quals = extract_gpuhashjoin_quals(root,
													outer_path->parent,
													inner_path->parent,
													join_type,
													restrict_clauses);
	ip_item->join_nrows = joinrel->rows;
	ip_items_list = list_make1(ip_item);

	for (;;)
	{
		GpuJoinPath	   *gjpath = create_gpujoin_path(root,
													 joinrel,
													 outer_path,
													 ip_items_list,
													 final_tlist,
													 param_info,
													 required_outer,
													 try_parallel_path);
		if (!gjpath)
			break;

		if (try_parallel_path)
			add_partial_path(joinrel, (Path *)gjpath);
		else
			add_path(joinrel, (Path *)gjpath);

		/*
		 * pull up outer and 
		 */
		if (pgstrom_path_is_gpujoin(outer_path))
		{
			GpuJoinPath	   *gjpath = (GpuJoinPath *) outer_path;
			int				i;

			for (i=gjpath->num_rels-1; i>=0; i--)
			{
				inner_path_item *ip_temp = palloc0(sizeof(inner_path_item));

				ip_temp->join_type  = gjpath->inners[i].join_type;
				ip_temp->inner_path = gjpath->inners[i].scan_path;
				ip_temp->join_quals = gjpath->inners[i].join_quals;
				ip_temp->hash_quals = gjpath->inners[i].hash_quals;
				ip_temp->join_nrows = gjpath->inners[i].join_nrows;

				ip_items_list = lcons(ip_temp, ip_items_list);
			}
			outer_path = linitial(gjpath->cpath.custom_paths);
		}
		else if (outer_path->pathtype == T_NestLoop ||
				 outer_path->pathtype == T_HashJoin ||
				 outer_path->pathtype == T_MergeJoin)
		{
			JoinPath   *join_path = (JoinPath *) outer_path;

			/*
			 * We cannot pull-up outer join path if its inner/outer paths
			 * are mutually parameterized.
			 */
			if (bms_overlap(PATH_REQ_OUTER(join_path->innerjoinpath),
							join_path->outerjoinpath->parent->relids) ||
				bms_overlap(PATH_REQ_OUTER(join_path->outerjoinpath),
							join_path->innerjoinpath->parent->relids))
				return;

			ip_item = palloc0(sizeof(inner_path_item));
			ip_item->join_type = join_path->jointype;
			ip_item->inner_path = join_path->innerjoinpath;
			ip_item->join_quals = join_path->joinrestrictinfo;
			ip_item->hash_quals = extract_gpuhashjoin_quals(
										root,
										join_path->outerjoinpath->parent,
										join_path->innerjoinpath->parent,
										join_path->jointype,
										join_path->joinrestrictinfo);
			ip_item->join_nrows = join_path->path.parent->rows;
			ip_items_list = lcons(ip_item, ip_items_list);
			outer_path = join_path->outerjoinpath;
		}
		else
			break;
	}
}

/*
 * gpujoin_add_join_path
 *
 * entrypoint of the GpuJoin logic
 */
static void
gpujoin_add_join_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  RelOptInfo *outerrel,
					  RelOptInfo *innerrel,
					  JoinType jointype,
					  JoinPathExtraData *extra)
{
	Path	   *outer_path;
	Path	   *inner_path;
	List	   *final_tlist = NIL;
	ListCell   *lc1, *lc2;

	/* calls secondary module if exists */
	if (set_join_pathlist_next)
		set_join_pathlist_next(root,
							   joinrel,
							   outerrel,
							   innerrel,
							   jointype,
							   extra);

	/* nothing to do, if PG-Strom is not enabled */
	if (!pgstrom_enabled)
		return;

	/*
	 * Pay attention for the device projection cost if this joinrel may become
	 * the root of plan tree, thus generates the final results.
	 * The cost for projection shall be added at apply_projection_to_path()
	 * later, so we decrement the estimated benefit by GpuProjection.
	 */
	if (bms_equal(root->all_baserels, joinrel->relids))
	{
		foreach (lc1, root->processed_tlist)
		{
			TargetEntry	   *tle = lfirst(lc1);

			if (!IsA(tle->expr, Var) &&
				!IsA(tle->expr, Const) &&
				!IsA(tle->expr, Param) &&
				pgstrom_device_expression(tle->expr))
				break;
		}
		if (!lc1)
			final_tlist = root->processed_tlist;
	}

	/*
	 * make a traditional sequential path
	 */
	inner_path = gpujoin_find_cheapest_path(root, joinrel, innerrel, false);
	if (!inner_path)
		return;
	outer_path = gpujoin_find_cheapest_path(root, joinrel, outerrel, false);
	if (!outer_path)
		return;
	try_add_gpujoin_paths(root, joinrel, final_tlist,
						  outer_path, inner_path,
						  jointype, extra, false);

	/*
	 * consider partial paths if any partial outers
	 */
	if (joinrel->consider_parallel)
	{
		Relids	other_relids = bms_difference(joinrel->relids,
											  outerrel->relids);
		foreach (lc1, innerrel->pathlist)
		{
			inner_path = lfirst(lc1);

			if (!inner_path->parallel_safe ||
				bms_overlap(PATH_REQ_OUTER(inner_path), other_relids))
				continue;

			foreach (lc2, outerrel->partial_pathlist)
			{
				outer_path = lfirst(lc2);

				if (!outer_path->parallel_safe ||
					outer_path->parallel_workers == 0 ||
					bms_overlap(PATH_REQ_OUTER(outer_path), other_relids))
					continue;
				try_add_gpujoin_paths(root, joinrel, final_tlist,
									  outer_path, inner_path,
									  jointype, extra, true);
			}
		}
	}
}

/*
 * build_flatten_qualifier
 *
 * It makes a flat AND expression that is equivalent to the given list.
 */
static Expr *
build_flatten_qualifier(List *clauses)
{
	List	   *args = NIL;
	ListCell   *lc;

	foreach (lc, clauses)
	{
		Node   *expr = lfirst(lc);

		if (!expr)
			continue;
		Assert(exprType(expr) == BOOLOID);
		if (IsA(expr, BoolExpr) &&
			((BoolExpr *) expr)->boolop == AND_EXPR)
			args = list_concat(args, ((BoolExpr *) expr)->args);
		else
			args = lappend(args, expr);
	}
	if (list_length(args) == 0)
		return NULL;
	if (list_length(args) == 1)
		return linitial(args);
	return make_andclause(args);
}

/*
 * build_device_targetlist
 *
 * It constructs a tentative custom_scan_tlist, according to
 * the expression to be evaluated, returned or shown in EXPLAIN.
 * Usually, all we need to pay attention is columns referenced by host-
 * qualifiers and target-list. However, we may need to execute entire
 * JOIN operations on CPU if GPU raised CpuReCheck error. So, we also
 * adds columns which are also referenced by device qualifiers.
 * (EXPLAIN command has to solve the name, so we have to have these
 * Var nodes in the custom_scan_tlist.)
 *
 * pgstrom_post_planner_gpujoin() may update the custom_scan_tlist
 * to push-down CPU projection. In this case, custom_scan_tlist will
 * have complicated expression not only simple Var-nodes, to simplify
 * targetlist of the CustomScan to reduce cost for CPU projection as
 * small as possible we can.
 */
typedef struct
{
	List		   *ps_tlist;
	List		   *ps_depth;
	List		   *ps_resno;
	GpuJoinPath	   *gpath;
	List		   *custom_plans;
	Index			outer_scanrelid;
	bool			resjunk;
} build_device_tlist_context;

static bool
build_device_tlist_walker(Node *node, build_device_tlist_context *context)
{
	GpuJoinPath	   *gpath = context->gpath;
	RelOptInfo	   *rel;
	ListCell	   *cell;
	int				i;

	if (!node)
		return false;
	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *) node;
		Var	   *ps_node;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (!IsA(tle->expr, Var))
				continue;

			ps_node = (Var *) tle->expr;
			if (ps_node->varno == varnode->varno &&
				ps_node->varattno == varnode->varattno &&
				ps_node->varlevelsup == varnode->varlevelsup)
			{
				/* sanity checks */
				Assert(ps_node->vartype == varnode->vartype &&
					   ps_node->vartypmod == varnode->vartypmod &&
					   ps_node->varcollid == varnode->varcollid);
				return false;
			}
		}

		/*
		 * Not in the pseudo-scan targetlist, so append this one
		 */
		for (i=0; i <= gpath->num_rels; i++)
		{
			if (i == 0)
			{
				Path   *outer_path = linitial(gpath->cpath.custom_paths);

				rel = outer_path->parent;
				/* special case if outer scan was pulled up */
				if (varnode->varno == context->outer_scanrelid)
				{
					TargetEntry	   *ps_tle =
						makeTargetEntry((Expr *) copyObject(varnode),
										list_length(context->ps_tlist) + 1,
										NULL,
										context->resjunk);
					context->ps_tlist = lappend(context->ps_tlist, ps_tle);
					context->ps_depth = lappend_int(context->ps_depth, i);
					context->ps_resno = lappend_int(context->ps_resno,
													varnode->varattno);
					Assert(bms_is_member(varnode->varno, rel->relids));
					Assert(varnode->varno == rel->relid);
					return false;
				}
			}
			else
				rel = gpath->inners[i-1].scan_path->parent;

			if (bms_is_member(varnode->varno, rel->relids))
			{
				Plan   *plan = list_nth(context->custom_plans, i);

				foreach (cell, plan->targetlist)
				{
					TargetEntry *tle = lfirst(cell);

					if (equal(varnode, tle->expr))
					{
						TargetEntry	   *ps_tle =
							makeTargetEntry((Expr *) copyObject(varnode),
											list_length(context->ps_tlist) + 1,
											NULL,
											context->resjunk);
						context->ps_tlist = lappend(context->ps_tlist, ps_tle);
						context->ps_depth = lappend_int(context->ps_depth, i);
						context->ps_resno = lappend_int(context->ps_resno,
														tle->resno);
						return false;
					}
				}
				break;
			}
		}
		elog(ERROR, "Bug? uncertain origin of Var-node: %s",
			 nodeToString(varnode));
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phvnode = (PlaceHolderVar *) node;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (equal(phvnode, tle->expr))
				return false;
		}

		/* Not in the pseudo-scan target-list, so append a new one */
		for (i=0; i <= gpath->num_rels; i++)
		{
			if (i == 0)
			{
				/*
				 * NOTE: We don't assume PlaceHolderVar that references the
				 * outer-path which was pulled-up, because only simple scan
				 * paths (SeqScan or GpuScan with no host-only qualifiers)
				 * can be pulled-up, thus, no chance for SubQuery paths.
				 */
				Index	outer_scanrelid = context->outer_scanrelid;
				Path   *outer_path = linitial(gpath->cpath.custom_paths);

				if (outer_scanrelid != 0 &&
					bms_is_member(outer_scanrelid, phvnode->phrels))
					elog(ERROR, "Bug? PlaceHolderVar referenced simple scan outer-path, not expected: %s", nodeToString(phvnode));

				rel = outer_path->parent;
			}
			else
				rel = gpath->inners[i-1].scan_path->parent;

			if (bms_is_subset(phvnode->phrels, rel->relids))
			{
				Plan   *plan = list_nth(context->custom_plans, i);

				foreach (cell, plan->targetlist)
				{
					TargetEntry	   *tle = lfirst(cell);
					TargetEntry	   *ps_tle;
					AttrNumber		ps_resno;

					if (!equal(phvnode, tle->expr))
						continue;

					ps_resno = list_length(context->ps_tlist) + 1;
					ps_tle = makeTargetEntry((Expr *) copyObject(phvnode),
											 ps_resno,
											 NULL,
											 context->resjunk);
					context->ps_tlist = lappend(context->ps_tlist, ps_tle);
					context->ps_depth = lappend_int(context->ps_depth, i);
					context->ps_resno = lappend_int(context->ps_resno,
													tle->resno);
					return false;
				}
			}
		}
		elog(ERROR, "Bug? uncertain origin of PlaceHolderVar-node: %s",
			 nodeToString(phvnode));
	}
	else if (!context->resjunk &&
			 pgstrom_device_expression((Expr *)node))
	{
		TargetEntry	   *ps_tle;

		foreach (cell, context->ps_tlist)
		{
			TargetEntry	   *tle = lfirst(cell);

			if (equal(node, tle->expr))
				return false;
		}

		ps_tle = makeTargetEntry((Expr *) copyObject(node),
								 list_length(context->ps_tlist) + 1,
								 NULL,
								 context->resjunk);
		context->ps_tlist = lappend(context->ps_tlist, ps_tle);
		context->ps_depth = lappend_int(context->ps_depth, -1);	/* dummy */
		context->ps_resno = lappend_int(context->ps_resno, -1);	/* dummy */

		return false;
	}
	return expression_tree_walker(node, build_device_tlist_walker,
								  (void *) context);
}

static void
build_device_targetlist(GpuJoinPath *gpath,
						CustomScan *cscan,
						GpuJoinInfo *gj_info,
						List *targetlist,
						List *custom_plans)
{
	build_device_tlist_context	context;

	Assert(outerPlan(cscan)
		   ? cscan->scan.scanrelid == 0
		   : cscan->scan.scanrelid != 0);

	memset(&context, 0, sizeof(build_device_tlist_context));
	context.gpath = gpath;
	context.custom_plans = custom_plans;
	context.outer_scanrelid = cscan->scan.scanrelid;
	context.resjunk = false;

	build_device_tlist_walker((Node *)targetlist, &context);

	/*
	 * Above are host referenced columns. On the other hands, the columns
	 * newly added below are device-only columns, so it will never
	 * referenced by the host-side. We mark it resjunk=true.
	 *
	 * Also note that any Var nodes in the device executable expression
	 * must be added with resjunk=true to solve the variable name.
	 */
	context.resjunk = true;
	build_device_tlist_walker((Node *)gj_info->outer_quals, &context);
	build_device_tlist_walker((Node *)gj_info->join_quals, &context);
	build_device_tlist_walker((Node *)gj_info->other_quals, &context);
	build_device_tlist_walker((Node *)gj_info->hash_inner_keys, &context);
	build_device_tlist_walker((Node *)gj_info->hash_outer_keys, &context);
	build_device_tlist_walker((Node *)targetlist, &context);

	Assert(list_length(context.ps_tlist) == list_length(context.ps_depth) &&
		   list_length(context.ps_tlist) == list_length(context.ps_resno));

	gj_info->ps_src_depth = context.ps_depth;
	gj_info->ps_src_resno = context.ps_resno;
	cscan->custom_scan_tlist = context.ps_tlist;
}

/*
 * PlanGpuJoinPath
 *
 * Entrypoint to create CustomScan(GpuJoin) node
 */
static Plan *
PlanGpuJoinPath(PlannerInfo *root,
				RelOptInfo *rel,
				CustomPath *best_path,
				List *tlist,
				List *clauses,
				List *custom_plans)
{
	GpuJoinPath	   *gjpath = (GpuJoinPath *) best_path;
	GpuJoinInfo		gj_info;
	CustomScan	   *cscan;
	codegen_context	context;
	char		   *kern_source;
	Plan		   *outer_plan;
	ListCell	   *lc;
	double			outer_nrows;
	int				i;

	Assert(gjpath->num_rels + 1 == list_length(custom_plans));
	outer_plan = linitial(custom_plans);

	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->flags = best_path->flags;
	cscan->methods = &gpujoin_plan_methods;
	cscan->custom_plans = list_copy_tail(custom_plans, 1);

	memset(&gj_info, 0, sizeof(GpuJoinInfo));
	gj_info.outer_ratio = 1.0;
	gj_info.outer_nrows = outer_plan->plan_rows;
	gj_info.outer_width = outer_plan->plan_width;
	gj_info.outer_startup_cost = outer_plan->startup_cost;
	gj_info.outer_total_cost = outer_plan->total_cost;
	gj_info.num_rels = gjpath->num_rels;

	outer_nrows = outer_plan->plan_rows;
	for (i=0; i < gjpath->num_rels; i++)
	{
		List	   *hash_inner_keys = NIL;
		List	   *hash_outer_keys = NIL;
		List	   *join_quals = NIL;
		List	   *other_quals = NIL;

		foreach (lc, gjpath->inners[i].hash_quals)
		{
			Path		   *scan_path = gjpath->inners[i].scan_path;
			RelOptInfo	   *scan_rel = scan_path->parent;
			RestrictInfo   *rinfo = lfirst(lc);
			OpExpr		   *op_clause = (OpExpr *) rinfo->clause;
			Relids			relids1;
			Relids			relids2;
			Node		   *arg1;
			Node		   *arg2;

			Assert(is_opclause(op_clause));
			arg1 = (Node *) linitial(op_clause->args);
			arg2 = (Node *) lsecond(op_clause->args);
			relids1 = pull_varnos(arg1);
			relids2 = pull_varnos(arg2);
			if (bms_is_subset(relids1, scan_rel->relids) &&
				!bms_is_subset(relids2, scan_rel->relids))
			{
				hash_inner_keys = lappend(hash_inner_keys, arg1);
				hash_outer_keys = lappend(hash_outer_keys, arg2);
			}
			else if (bms_is_subset(relids2, scan_rel->relids) &&
					 !bms_is_subset(relids1, scan_rel->relids))
			{
				hash_inner_keys = lappend(hash_inner_keys, arg2);
				hash_outer_keys = lappend(hash_outer_keys, arg1);
			}
			else
				elog(ERROR, "Bug? hash-clause reference bogus varnos");
		}

		/*
		 * Add properties of GpuJoinInfo
		 */
		gj_info.plan_nrows_in = lappend(gj_info.plan_nrows_in,
										pmakeFloat(outer_nrows));
		gj_info.plan_nrows_out = lappend(gj_info.plan_nrows_out,
									pmakeFloat(gjpath->inners[i].join_nrows));
		gj_info.ichunk_size = lappend_int(gj_info.ichunk_size,
										  gjpath->inners[i].ichunk_size);
		gj_info.join_types = lappend_int(gj_info.join_types,
										 gjpath->inners[i].join_type);

		if (IS_OUTER_JOIN(gjpath->inners[i].join_type))
		{
			extract_actual_join_clauses(gjpath->inners[i].join_quals,
										&join_quals, &other_quals);
		}
		else
		{
			join_quals = extract_actual_clauses(gjpath->inners[i].join_quals,
												false);
			other_quals = NIL;
		}
		gj_info.join_quals = lappend(gj_info.join_quals,
									 build_flatten_qualifier(join_quals));
		gj_info.other_quals = lappend(gj_info.other_quals,
									  build_flatten_qualifier(other_quals));

		gj_info.nloops_minor = lappend(gj_info.nloops_minor,
				makeInteger(double_as_long(gjpath->inners[i].nloops_minor)));
		gj_info.nloops_major = lappend(gj_info.nloops_major,
				makeInteger(double_as_long(gjpath->inners[i].nloops_major)));
		gj_info.hash_inner_keys = lappend(gj_info.hash_inner_keys,
										  hash_inner_keys);
		gj_info.hash_outer_keys = lappend(gj_info.hash_outer_keys,
										  hash_outer_keys);
		outer_nrows = gjpath->inners[i].join_nrows;
	}

	/*
	 * If outer-plan node is simple relation scan; SeqScan or GpuScan with
	 * device executable qualifiers, GpuJoin can handle the relation scan
	 * for better i/o performance. Elsewhere, call the child outer node.
	 */
	if (gjpath->outer_relid)
	{
		cscan->scan.scanrelid = gjpath->outer_relid;
		gj_info.outer_quals = gjpath->outer_quals;
	}
	else
	{
		outerPlan(cscan) = outer_plan;
	}
	gj_info.outer_nrows_per_block = gjpath->outer_nrows_per_block;

	/*
	 * Build a tentative pseudo-scan targetlist. At this point, we cannot
	 * know which expression shall be applied on the final results, thus,
	 * all we can construct is a pseudo-scan targetlist that is consists
	 * of Var-nodes only.
	 */
	build_device_targetlist(gjpath, cscan, &gj_info, tlist, custom_plans);

	/*
	 * construct kernel code
	 */
	pgstrom_init_codegen_context(&context);
	kern_source = gpujoin_codegen(root, cscan, &gj_info, tlist, &context);
	if (context.func_defs || context.expr_defs)
	{
		StringInfoData	buf;

		initStringInfo(&buf);
		pgstrom_codegen_func_declarations(&buf, &context);
		pgstrom_codegen_expr_declarations(&buf, &context);
		appendStringInfo(&buf, "%s", kern_source);

		kern_source = buf.data;
	}
	gj_info.kern_source = kern_source;
	gj_info.extra_flags = (DEVKERNEL_NEEDS_GPUSCAN |
						   DEVKERNEL_NEEDS_GPUJOIN |
						   DEVKERNEL_NEEDS_DYNPARA |
						   context.extra_flags);
	gj_info.used_params = context.used_params;

	form_gpujoin_info(cscan, &gj_info);

	return &cscan->scan.plan;
}

typedef struct
{
	int		depth;
	List   *ps_src_depth;
	List   *ps_src_resno;
} fixup_varnode_to_origin_context;

static Node *
fixup_varnode_to_origin_mutator(Node *node,
								fixup_varnode_to_origin_context *context)
{
	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *) node;
		int		varattno = varnode->varattno;
		int		src_depth;

		Assert(varnode->varno == INDEX_VAR);
		src_depth = list_nth_int(context->ps_src_depth,
								 varnode->varattno - 1);
		if (src_depth == context->depth)
		{
			Var	   *newnode = copyObject(varnode);

			newnode->varno = INNER_VAR;
			newnode->varattno = list_nth_int(context->ps_src_resno,
											 varattno - 1);
			return (Node *) newnode;
		}
		else if (src_depth > context->depth)
			elog(ERROR, "Expression reference deeper than current depth");
	}
	return expression_tree_mutator(node, fixup_varnode_to_origin_mutator,
								   (void *) context);
}

static List *
fixup_varnode_to_origin(int depth, List *ps_src_depth, List *ps_src_resno,
						List *expr_list)
{
	fixup_varnode_to_origin_context	context;

	Assert(IsA(expr_list, List));
	context.depth = depth;
	context.ps_src_depth = ps_src_depth;
	context.ps_src_resno = ps_src_resno;

	return (List *) fixup_varnode_to_origin_mutator((Node *) expr_list,
													&context);
}

/*
 * assign_gpujoin_session_info
 *
 * Gives some definitions to the static portion of GpuJoin implementation
 */
void
assign_gpujoin_session_info(StringInfo buf, GpuTaskState_v2 *gts)
{
	TupleTableSlot *slot = gts->css.ss.ss_ScanTupleSlot;
	TupleDesc		tupdesc = slot->tts_tupleDescriptor;

	Assert(gts->css.methods == &gpujoin_exec_methods);
	appendStringInfo(
		buf,
		"#define GPUJOIN_DEVICE_PROJECTION_NFIELDS %u\n"
		"#define GPUJOIN_DEVICE_PROJECTION_EXTRA_SIZE %u\n",
		tupdesc->natts,
		((GpuJoinState *) gts)->extra_maxlen);
}

static Node *
gpujoin_create_scan_state(CustomScan *node)
{
	GpuJoinState   *gjs;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(node);
	cl_int			num_rels = gj_info->num_rels;

	Assert(num_rels == list_length(node->custom_plans));
	gjs = palloc0(offsetof(GpuJoinState, inners[num_rels]));

	NodeSetTag(gjs, T_CustomScanState);
	gjs->gts.css.flags = node->flags;
	gjs->gts.css.methods = &gpujoin_exec_methods;

	return (Node *) gjs;
}

static void
ExecInitGpuJoin(CustomScanState *node, EState *estate, int eflags)
{
	GpuContext_v2  *gcontext = NULL;
	GpuJoinState   *gjs = (GpuJoinState *) node;
	ScanState	   *ss = &gjs->gts.css.ss;
	CustomScan	   *cscan = (CustomScan *) node->ss.ps.plan;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(cscan);
	TupleDesc		result_tupdesc = GTS_GET_RESULT_TUPDESC(gjs);
	TupleDesc		scan_tupdesc;
	TupleDesc		junk_tupdesc;
	List		   *tlist_fallback = NIL;
	bool			fallback_needs_projection = false;
	bool			fallback_meets_resjunk = false;
	ListCell	   *lc1;
	ListCell	   *lc2;
	cl_int			i, j, nattrs;
	cl_int			first_right_outer_depth = -1;
	char		   *kern_define;
	ProgramId		program_id;
	bool			with_connection = ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0);

	/* activate a GpuContext for CUDA kernel execution */
	gcontext = AllocGpuContext(with_connection);

	/*
	 * Re-initialization of scan tuple-descriptor and projection-info,
	 * because commit 1a8a4e5cde2b7755e11bde2ea7897bd650622d3e of
	 * PostgreSQL makes to assign result of ExecTypeFromTL() instead
	 * of ExecCleanTypeFromTL; that leads unnecessary projection.
	 * So, we try to remove junk attributes from the scan-descriptor.
	 *
	 * Also note that the supplied TupleDesc that contains junk attributes
	 * are still useful to run CPU fallback code. So, we keep this tuple-
	 * descriptor to initialize the related stuff.
	 */
	junk_tupdesc = gjs->gts.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	scan_tupdesc = ExecCleanTypeFromTL(cscan->custom_scan_tlist, false);
	ExecAssignScanType(&gjs->gts.css.ss, scan_tupdesc);
	ExecAssignScanProjectionInfoWithVarno(&gjs->gts.css.ss, INDEX_VAR);

	/* Setup common GpuTaskState fields */
	pgstromInitGpuTaskState(&gjs->gts,
							gcontext,
							GpuTaskKind_GpuJoin,
							gj_info->used_params,
							estate);
	gjs->gts.cb_next_task	= gpujoin_next_task;
	gjs->gts.cb_next_tuple	= gpujoin_next_tuple;
	gjs->gts.cb_ready_task	= gpujoin_ready_task;
	gjs->gts.cb_switch_task	= gpujoin_switch_task;
	if (pgstrom_bulkexec_enabled &&
		gjs->gts.css.ss.ps.qual == NIL &&
		gjs->gts.css.ss.ps.ps_ProjInfo == NULL)
		gjs->gts.cb_bulk_exec = pgstromBulkExecGpuTaskState;
	gjs->gts.outer_nrows_per_block = gj_info->outer_nrows_per_block;

	/*
	 * NOTE: outer_quals, hash_outer_keys and join_quals are intended
	 * to use fallback routine if GPU kernel required host-side to
	 * retry a series of hash-join/nest-loop operation. So, we need to
	 * pay attention which slot is actually referenced.
	 * Right now, ExecEvalScalarVar can reference only three slots
	 * simultaneously (scan, inner and outer). So, varno of varnodes
	 * has to be initialized according to depth of the expression.
	 *
	 * TODO: we have to initialize above expressions carefully for
	 * CPU fallback implementation.
	 */
	gjs->num_rels = gj_info->num_rels;
	gjs->join_types = gj_info->join_types;
	gjs->outer_quals = NIL;
	foreach (lc1, gj_info->outer_quals)
	{
		ExprState  *expr_state = ExecInitExpr(lfirst(lc1), &ss->ps);
		gjs->outer_quals = lappend(gjs->outer_quals, expr_state);
	}
	gjs->outer_ratio = gj_info->outer_ratio;
	gjs->outer_nrows = gj_info->outer_nrows;
	gjs->gts.css.ss.ps.qual = (List *)
		ExecInitExpr((Expr *)cscan->scan.plan.qual, &ss->ps);

	/*
	 * Init OUTER child node
	 */
	if (gjs->gts.css.ss.ss_currentRelation)
	{
		nattrs = RelationGetDescr(gjs->gts.css.ss.ss_currentRelation)->natts;
	}
	else
	{
		TupleTableSlot *outer_slot;

		outerPlanState(gjs) = ExecInitNode(outerPlan(cscan), estate, eflags);
		outer_slot = outerPlanState(gjs)->ps_ResultTupleSlot;
		nattrs = outer_slot->tts_tupleDescriptor->natts;
	}

	/*
	 * Init CPU fallback stuff
	 */
	foreach (lc1, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc1);
		Var			   *var;

		/*
		 * NOTE: Var node inside of general expression shall reference
		 * the custom_scan_tlist recursively. Thus, we don't need to
		 * care about varno/varattno fixup here.
		 */
		Assert(IsA(tle, TargetEntry));

		/*
		 * Because ss_ScanTupleSlot does not contain junk attribute,
		 * we have to remove junk attribute by projection, if any of
		 * target-entry in custom_scan_tlist (that is tuple format to
		 * be constructed by CPU fallback) are junk.
		 */
		if (tle->resjunk)
		{
			fallback_needs_projection = true;
			fallback_meets_resjunk = true;
		}
		else
		{
			/* no valid attribute after junk attribute */
			if (fallback_meets_resjunk)
				elog(ERROR, "Bug? a valid attribute appear after junk ones");

			Assert(!fallback_meets_resjunk);

			if (IsA(tle->expr, Var))
			{
				tle = copyObject(tle);
				var = (Var *) tle->expr;
				var->varnoold	= var->varno;
				var->varoattno	= var->varattno;
				var->varno		= INDEX_VAR;
				var->varattno	= tle->resno;
			}
			else
			{
				/* also, non-simple Var node needs projection */
				fallback_needs_projection = true;
			}
			tlist_fallback = lappend(tlist_fallback,
									 ExecInitExpr((Expr *) tle, &ss->ps));
		}
	}

	if (fallback_needs_projection)
	{
		gjs->slot_fallback = MakeSingleTupleTableSlot(junk_tupdesc);
		gjs->proj_fallback = ExecBuildProjectionInfo(tlist_fallback,
													 ss->ps.ps_ExprContext,
													 ss->ss_ScanTupleSlot,
													 junk_tupdesc);
	}
	else
	{
		gjs->slot_fallback = ss->ss_ScanTupleSlot;
		gjs->proj_fallback = NULL;
	}

	gjs->outer_src_anum_min = nattrs;
	gjs->outer_src_anum_max = FirstLowInvalidHeapAttributeNumber;
	nattrs -= FirstLowInvalidHeapAttributeNumber;
	gjs->outer_dst_resno = palloc0(sizeof(AttrNumber) * nattrs);
	j = 1;
	forboth (lc1, gj_info->ps_src_depth,
			 lc2, gj_info->ps_src_resno)
	{
		int		depth = lfirst_int(lc1);
		int		resno = lfirst_int(lc2);

		if (depth == 0)
		{
			if (gjs->outer_src_anum_min > resno)
				gjs->outer_src_anum_min = resno;
			if (gjs->outer_src_anum_max < resno)
				gjs->outer_src_anum_max = resno;
			resno -= FirstLowInvalidHeapAttributeNumber;
			Assert(resno > 0 && resno <= nattrs);
			gjs->outer_dst_resno[resno - 1] = j;
		}
		j++;
	}

	/*
	 * Init INNER child nodes for each depth
	 */
	for (i=0; i < gj_info->num_rels; i++)
	{
		Plan	   *inner_plan = list_nth(cscan->custom_plans, i);
		innerState *istate = &gjs->inners[i];
		Expr	   *join_quals;
		Expr	   *other_quals;
		List	   *hash_inner_keys;
		List	   *hash_outer_keys;
		TupleTableSlot *inner_slot;
		double		plan_nrows_in;
		double		plan_nrows_out;
		bool		be_row_format = false;

		/* row-format is preferable if plan is self-managed one */
		if (pgstrom_plan_is_gpuscan(inner_plan) ||
			pgstrom_plan_is_gpujoin(inner_plan))
			be_row_format = true;
		istate->state = ExecInitNode(inner_plan, estate, eflags);
		if (be_row_format)
			((GpuTaskState_v2 *)istate->state)->row_format = true;
		istate->econtext = CreateExprContext(estate);
		istate->depth = i + 1;
		istate->nbatches_plan =
			long_as_double(intVal(list_nth(gj_info->nloops_major, i)));
		istate->nbatches_exec =
			((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0 ? -1 : 0);
		plan_nrows_in = floatVal(list_nth(gj_info->plan_nrows_in, i));
		plan_nrows_out = floatVal(list_nth(gj_info->plan_nrows_out, i));
		istate->nrows_ratio = plan_nrows_out / Max(plan_nrows_in, 1.0);
		istate->ichunk_size = list_nth_int(gj_info->ichunk_size, i);
		istate->join_type = (JoinType)list_nth_int(gj_info->join_types, i);

		if (first_right_outer_depth < 0 &&
			(istate->join_type == JOIN_RIGHT ||
			 istate->join_type == JOIN_FULL))
			first_right_outer_depth = istate->depth;

		/*
		 * NOTE: We need to deal with Var-node references carefully,
		 * because varno/varattno pair depends on the context when
		 * ExecQual() is called.
		 * - join_quals and hash_outer_keys are only called for
		 * fallback process when CpuReCheck error was returned.
		 * So, we can expect values are stored in ecxt_scantuple
		 * according to the pseudo-scan-tlist.
		 *- hash_inner_keys are only called to construct hash-table
		 * prior to GPU execution, so, we can expect input values
		 * are deployed according to the result of child plans.
		 */
		join_quals = list_nth(gj_info->join_quals, i);
		if (!join_quals)
			istate->join_quals = NIL;
		else
		{
			ExprState  *expr_state = ExecInitExpr(join_quals, &ss->ps);
			istate->join_quals = list_make1(expr_state);
		}

		other_quals = list_nth(gj_info->other_quals, i);
		if (!other_quals)
			istate->other_quals = NIL;
		else
		{
			ExprState  *expr_state = ExecInitExpr(other_quals, &ss->ps);
			istate->other_quals = list_make1(expr_state);
		}

		hash_inner_keys = list_nth(gj_info->hash_inner_keys, i);
		if (hash_inner_keys != NIL)
		{
			cl_uint		shift;

			hash_inner_keys = fixup_varnode_to_origin(i+1,
													  gj_info->ps_src_depth,
													  gj_info->ps_src_resno,
													  hash_inner_keys);
			foreach (lc1, hash_inner_keys)
			{
				Expr	   *expr = lfirst(lc1);
				ExprState  *expr_state = ExecInitExpr(expr, &ss->ps);
				Oid			type_oid = exprType((Node *)expr);
				int16		typlen;
				bool		typbyval;

				istate->hash_inner_keys =
					lappend(istate->hash_inner_keys, expr_state);

				get_typlenbyval(type_oid, &typlen, &typbyval);
				istate->hash_keytype =
					lappend_oid(istate->hash_keytype, type_oid);
				istate->hash_keylen =
					lappend_int(istate->hash_keylen, typlen);
				istate->hash_keybyval =
					lappend_int(istate->hash_keybyval, typbyval);
			}
			/* outer keys also */
			hash_outer_keys = list_nth(gj_info->hash_outer_keys, i);
			Assert(hash_outer_keys != NIL);
			istate->hash_outer_keys = (List *)
				ExecInitExpr((Expr *)hash_outer_keys, &ss->ps);

			Assert(IsA(istate->hash_outer_keys, List) &&
				   list_length(istate->hash_inner_keys) ==
				   list_length(istate->hash_outer_keys));

			/* usage histgram */
			shift = get_next_log2(gjs->inners[i].nbatches_plan) + 8;
			Assert(shift < sizeof(cl_uint) * BITS_PER_BYTE);
			istate->hgram_width = (1U << shift);
			istate->hgram_size = palloc0(sizeof(Size) * istate->hgram_width);
			istate->hgram_nitems = palloc0(sizeof(Size) * istate->hgram_width);
			istate->hgram_shift = sizeof(cl_uint) * BITS_PER_BYTE - shift;
			istate->hgram_curr = 0;
		}

		/*
		 * CPU fallback setup for INNER reference
		 */
		inner_slot = istate->state->ps_ResultTupleSlot;
		nattrs = inner_slot->tts_tupleDescriptor->natts;
		istate->inner_src_anum_min = nattrs;
		istate->inner_src_anum_max = FirstLowInvalidHeapAttributeNumber;
		nattrs -= FirstLowInvalidHeapAttributeNumber;
		istate->inner_dst_resno = palloc0(sizeof(AttrNumber) * nattrs);

		j = 1;
		forboth (lc1, gj_info->ps_src_depth,
				 lc2, gj_info->ps_src_resno)
		{
			int		depth = lfirst_int(lc1);
			int		resno = lfirst_int(lc2);

			if (depth == istate->depth)
			{
				if (istate->inner_src_anum_min > resno)
					istate->inner_src_anum_min = resno;
				if (istate->inner_src_anum_max < resno)
					istate->inner_src_anum_max = resno;
				resno -= FirstLowInvalidHeapAttributeNumber;
				Assert(resno > 0 && resno <= nattrs);
				istate->inner_dst_resno[resno - 1] = j;
			}
			j++;
		}
		/* add inner state as children of this custom-scan */
		gjs->gts.css.custom_ps = lappend(gjs->gts.css.custom_ps,
										 istate->state);
	}

	/*
	 * Track the first RIGHT/FULL OUTER JOIN depth, if any
	 */
	gjs->first_right_outer_depth = Min(first_right_outer_depth,
									   gjs->num_rels + 1);

	/*
	 * Construct CUDA program, and kick asynchronous compile process.
	 * Note that assign_gpujoin_session_info() is called back from
	 * the pgstrom_assign_cuda_program(), thus, gjs->extra_maxlen has
	 * to be set prior to the program assignment.
	 */
	gjs->extra_maxlen = gj_info->extra_maxlen;

	kern_define = pgstrom_build_session_info(gj_info->extra_flags, &gjs->gts);
	program_id = pgstrom_create_cuda_program(gcontext,
											 gj_info->extra_flags,
											 gj_info->kern_source,
											 kern_define,
											 false);
	gjs->gts.program_id = program_id;

	/* expected kresults buffer expand rate */
	gjs->result_width =
		MAXALIGN(offsetof(HeapTupleHeaderData,
						  t_bits[BITMAPLEN(result_tupdesc->natts)]) +
				 (result_tupdesc->tdhasoid ? sizeof(Oid) : 0)) +
		MAXALIGN(cscan->scan.plan.plan_width);	/* average width */

	/*
	 * run-time statistics shall be initialized on the first call of
	 * executor or DSM initialization if parallel query
	 */
	gjs->rt_stat = NULL;

	/* init perfmon */
	//pgstrom_init_perfmon(&gjs->gts);
}

/*
 * ExecReCheckGpuJoin
 *
 * Routine of EPQ recheck on GpuJoin. Join condition shall be checked on
 * the EPQ tuples.
 */
static bool
ExecReCheckGpuJoin(CustomScanState *node, TupleTableSlot *slot)
{
	/*
	 * TODO: Extract EPQ tuples on CPU fallback slot, then check
	 * join condition by CPU
	 */
	return true;
}

/*
 * InitRuntimeStatistics
 */
static inline void
setup_runtime_statistics(GpuJoinState *gjs)
{
	if (!gjs->rt_stat)
	{
		runtimeStat	   *rt_stat;
		Size			required;

		required = (STROMALIGN(sizeof(runtimeStat)) +
					STROMALIGN(sizeof(size_t) * (gjs->num_rels + 1)) +
					STROMALIGN(sizeof(size_t) * (gjs->num_rels + 1)) +
					STROMALIGN(sizeof(cl_double) * (gjs->num_rels + 1)));

		rt_stat = dmaBufferAlloc(gjs->gts.gcontext, required);
		if (!rt_stat)
			elog(ERROR, "out of DMA buffer");
		memset(rt_stat, 0, required);
		rt_stat->num_rels = gjs->num_rels;
		SpinLockInit(&rt_stat->lock);
		rt_stat->inner_nitems = (size_t *)
			((char *)rt_stat + STROMALIGN(sizeof(runtimeStat)));
		rt_stat->right_nitems = (size_t *)
			((char *)rt_stat->inner_nitems + STROMALIGN(sizeof(size_t) *
														(gjs->num_rels + 1)));
		rt_stat->row_dist_score = (cl_double *)
			((char *)rt_stat->right_nitems + STROMALIGN(sizeof(size_t) *
														(gjs->num_rels + 1)));
		gjs->rt_stat = rt_stat;
	}
}

/*
 * ExecGpuJoin
 */
static TupleTableSlot *
ExecGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;

	if (!gjs->rt_stat)
		setup_runtime_statistics(gjs);
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) pgstromExecGpuTaskState,
					(ExecScanRecheckMtd) ExecReCheckGpuJoin);
}

static void
ExecEndGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	int				i;

	/*
	 * clean up GpuJoin specific resources
	 */
	if (gjs->curr_pmrels)
	{
		multirels_detach_buffer(gjs, gjs->curr_pmrels, false);
		gjs->curr_pmrels = NULL;
	}
	gpujoin_inner_unload(gjs, false);

	/*
	 * Clean up subtree (if any)
	 */
	ExecEndNode(outerPlanState(node));
	for (i=0; i < gjs->num_rels; i++)
		ExecEndNode(gjs->inners[i].state);
	/* then other generic resources */
	pgstromReleaseGpuTaskState(&gjs->gts);
}

static void
ExecReScanGpuJoin(CustomScanState *node)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	bool			keep_inners = true;
	cl_int			i;

	/* common rescan handling */
	pgstromRescanGpuTaskState(&gjs->gts);

	/*
	 * NOTE: ExecReScan() does not pay attention on the PlanState within
	 * custom_ps, so we need to assign its chgParam by ourself.
	 */
	if (gjs->gts.css.ss.ps.chgParam != NULL)
	{
		for (i=0; i < gjs->num_rels; i++)
		{
			UpdateChangedParamSet(gjs->inners[i].state,
								  gjs->gts.css.ss.ps.chgParam);
			if (gjs->inners[i].state->chgParam != NULL)
				keep_inners = false;
		}
	}

	/*
	 * Rewind the outer relation
	 */
	if (gjs->gts.css.ss.ss_currentRelation)
		gpuscanRewindScanChunk(&gjs->gts);
	else
		ExecReScan(outerPlanState(gjs));
	gjs->gts.scan_overflow = NULL;
	gjs->outer_scan_done = false;

	/*
	 * Detach previous inner relations buffer
	 */
	if (gjs->curr_pmrels)
	{
		multirels_detach_buffer(gjs, gjs->curr_pmrels, false);
		gjs->curr_pmrels = NULL;
	}

	if (!keep_inners)
		gpujoin_inner_unload(gjs, true);
	else
	{
		/*
		 * Just rewind the inner pointer.
		 *
		 * NOTE: It is a tricky hack. gpujoin_inner_getnext() increments
		 * the pds_index prior to construction of pmrels, so all pds_index
		 * shall be reverted to 1, as expected beginning point.
		 */
        for (i=0; i < gjs->num_rels; i++)
            gjs->inners[i].pds_index = 0;
	}
}

static void
ExplainGpuJoin(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	CustomScan	   *cscan = (CustomScan *) node->ss.ps.plan;
	GpuJoinInfo	   *gj_info = deform_gpujoin_info(cscan);
	runtimeStat	   *rt_stat = gjs->rt_stat;
	List		   *dcontext;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	ListCell	   *lc4;
	char		   *temp;
	char			qlabel[128];
	int				depth;
	StringInfoData	str;

	initStringInfo(&str);
	/* deparse context */
	dcontext =  set_deparse_context_planstate(es->deparse_cxt,
											  (Node *) node,
											  ancestors);
	/* Device projection */
	resetStringInfo(&str);
	foreach (lc1, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc1);

#if 1
		/* disable this code block, if junk TLE is noisy */
		if (tle->resjunk)
			continue;
#endif
		if (lc1 != list_head(cscan->custom_scan_tlist))
			appendStringInfo(&str, ", ");
		if (tle->resjunk)
			appendStringInfoChar(&str, '[');
		temp = deparse_expression((Node *)tle->expr, dcontext, true, false);
		appendStringInfo(&str, "%s", temp);
		if (es->verbose)
		{
			temp = format_type_with_typemod(exprType((Node *)tle->expr),
											exprTypmod((Node *)tle->expr));
			appendStringInfo(&str, "::%s", temp);
		}
		if (tle->resjunk)
			appendStringInfoChar(&str, ']');
	}
	ExplainPropertyText("GPU Projection", str.data, es);

	/* statistics for outer scan, if it was pulled-up */
	if (es->analyze)
	{
		gjs->gts.outer_instrument.tuplecount = (rt_stat->inner_nitems[0] +
												rt_stat->right_nitems[0]);
		gjs->gts.outer_instrument.nfiltered1 = (rt_stat->source_nitems -
												rt_stat->inner_nitems[0] -
												rt_stat->right_nitems[0]);
	}
	pgstromExplainOuterScan(&gjs->gts, dcontext, ancestors, es,
							gj_info->outer_quals,
                            gj_info->outer_startup_cost,
                            gj_info->outer_total_cost,
                            gj_info->outer_nrows,
                            gj_info->outer_width);
	/* join-qualifiers */
	depth = 1;
	forfour (lc1, gj_info->join_types,
			 lc2, gj_info->join_quals,
			 lc3, gj_info->other_quals,
			 lc4, gj_info->hash_outer_keys)
	{
		JoinType	join_type = (JoinType) lfirst_int(lc1);
		Expr	   *join_quals = lfirst(lc2);
		Expr	   *other_quals = lfirst(lc3);
		Expr	   *hash_outer_key = lfirst(lc4);
		innerState *istate = &gjs->inners[depth-1];
		int			indent_width;
		double		plan_nrows_in;
		double		plan_nrows_out;
		double		exec_nrows_in = 0.0;
		double		exec_nrows_out1 = 0.0;	/* by INNER JOIN */
		double		exec_nrows_out2 = 0.0;	/* by OUTER JOIN */

		/* fetch number of rows */
		plan_nrows_in = floatVal(list_nth(gj_info->plan_nrows_in, depth-1));
		plan_nrows_out = floatVal(list_nth(gj_info->plan_nrows_out, depth-1));
		if (es->analyze)
		{
			exec_nrows_in = (rt_stat->inner_nitems[depth - 1] +
							 rt_stat->right_nitems[depth - 1]);
			exec_nrows_out1 = rt_stat->inner_nitems[depth];
			exec_nrows_out2 = rt_stat->right_nitems[depth];
		}

		resetStringInfo(&str);
		if (hash_outer_key != NULL)
		{
			appendStringInfo(&str, "GpuHash%sJoin",
							 join_type == JOIN_FULL ? "Full" :
							 join_type == JOIN_LEFT ? "Left" :
							 join_type == JOIN_RIGHT ? "Right" : "");
		}
		else
		{
			appendStringInfo(&str, "GpuNestLoop%s",
							 join_type == JOIN_FULL ? "Full" :
							 join_type == JOIN_LEFT ? "Left" :
							 join_type == JOIN_RIGHT ? "Right" : "");
		}
		snprintf(qlabel, sizeof(qlabel), "Depth% 2d", depth);
		indent_width = es->indent * 2 + strlen(qlabel) + 2;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (!es->analyze)
				appendStringInfo(&str, "  (nrows %.0f...%.0f)",
								 plan_nrows_in,
								 plan_nrows_out);
			else if (exec_nrows_out2 > 0.0)
				appendStringInfo(&str, "  (plan nrows: %.0f...%.0f,"
								 " actual nrows: %.0f...%.0f+%.0f)",
								 plan_nrows_in,
								 plan_nrows_out,
								 exec_nrows_in,
								 exec_nrows_out1,
								 exec_nrows_out2);
			else
				appendStringInfo(&str, "  (plan nrows: %.0f...%.0f,"
								 " actual nrows: %.0f...%.0f)",
								 plan_nrows_in,
								 plan_nrows_out,
								 exec_nrows_in,
								 exec_nrows_out1);
			ExplainPropertyText(qlabel, str.data, es);
		}
		else
		{
			ExplainPropertyText(qlabel, str.data, es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth% 2d Plan Rows-in", depth);
			ExplainPropertyFloat(qlabel, plan_nrows_in, 0, es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth% 2d Plan Rows-out", depth);
			ExplainPropertyFloat(qlabel, plan_nrows_out, 0, es);

			if (es->analyze)
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d Actual Rows-in", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_in, 0, es);

				snprintf(qlabel, sizeof(qlabel),
                         "Depth% 2d Actual Rows-out by inner join", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_out1, 0, es);

				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d Actual Rows-out by outer join", depth);
				ExplainPropertyFloat(qlabel, exec_nrows_out2, 0, es);
			}
		}

		/*
		 * HashJoinKeys, if any
		 */
		if (hash_outer_key)
		{
			temp = deparse_expression((Node *)hash_outer_key,
                                      dcontext, true, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "HashKeys: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d HashKeys", depth);
				ExplainPropertyText(qlabel, temp, es);
			}
		}

		/*
		 * JoinQuals, if any
		 */
		if (join_quals)
		{
			temp = deparse_expression((Node *)join_quals, dcontext,
									  true, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "JoinQuals: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth% 2d JoinQuals", depth);
				ExplainPropertyText(qlabel, temp, es);
			}
		}

		/*
		 * OtherQuals, if any
		 */
		if (other_quals)
		{
			temp = deparse_expression((Node *)other_quals, dcontext,
									  es->verbose, false);
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				appendStringInfoSpaces(es->str, indent_width);
				appendStringInfo(es->str, "JoinFilter: %s\n", temp);
			}
			else
			{
				snprintf(qlabel, sizeof(qlabel), "Depth %02d-Filter", depth);
				ExplainPropertyText(qlabel, str.data, es);
			}
		}

		/*
		 * Inner KDS statistics
		 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, indent_width);
			appendStringInfo(es->str, "KDS-%s (size: %s, nbatched: %u)",
							 hash_outer_key ? "Hash" : "Heap",
							 format_bytesz(istate->pds_limit),
							 istate->nbatches_plan);
			if (es->analyze)
				appendStringInfo(es->str, " (actual size: %s, nbatched: %u)\n",
								 format_bytesz(istate->ichunk_size),
								 istate->nbatches_exec);
			else
				appendStringInfoChar(es->str, '\n');
		}
		else
		{
			snprintf(qlabel, sizeof(qlabel), "Depth %02d KDS Type", depth);
			ExplainPropertyText(qlabel, hash_outer_key ? "Hash" : "Heap", es);

			snprintf(qlabel, sizeof(qlabel),
					 "Depth % 2d KDS Plan Size", depth);
			ExplainPropertyText(qlabel, format_bytesz(istate->pds_limit), es);

			snprintf(qlabel, sizeof(qlabel),
                     "Depth % 2d KDS Plan nBatches", depth);
			ExplainPropertyInteger(qlabel, istate->nbatches_plan, es);

			if (es->analyze)
			{
				snprintf(qlabel, sizeof(qlabel),
						 "Depth % 2d KDS Actual Size", depth);
				ExplainPropertyText(qlabel,
									format_bytesz(istate->ichunk_size), es);

				snprintf(qlabel, sizeof(qlabel),
						 "Depth % 2d KDS Actual nBatches", depth);
				ExplainPropertyInteger(qlabel, istate->nbatches_exec, es);
			}
		}
		depth++;
	}
	/* inner multirels buffer statistics */
	if (es->analyze)
	{
		resetStringInfo(&str);
		for (depth=1; depth <= gjs->num_rels; depth++)
		{
			innerState *istate = &gjs->inners[depth-1];

			appendStringInfo(&str, "%s(", depth > 1 ? "x" : "");
			foreach (lc1, istate->pds_list)
			{
				pgstrom_data_store *pds = lfirst(lc1);

				if (lc1 != list_head(istate->pds_list))
					appendStringInfo(&str, ", ");
				appendStringInfo(&str, "%s",
								 format_bytesz(pds->kds.length));
			}
			appendStringInfo(&str, ")");
		}
	}
	/* other common field */
	pgstromExplainGpuTaskState(&gjs->gts, es);
}

/*
 * gpujoin_merge_worker_statistics
 */
void
gpujoin_merge_worker_statistics(GpuTaskState_v2 *gts)
{
	if (gts->css.methods == &gpujoin_exec_methods)
	{
		pgstromWorkerStatistics *worker_stat = gts->worker_stat;
		GpuJoinState   *gjs = (GpuJoinState *) gts;
		runtimeStat	   *rt_stat = gjs->rt_stat;
		int				i;

		for (i=0; i < gjs->num_rels; i++)
		{
			rt_stat->inner_nitems[i] += worker_stat->gpujoin[i].inner_nitems;
			rt_stat->right_nitems[i] += worker_stat->gpujoin[i].right_nitems;
		}
	}
}

/*
 * gpujpin_accum_worker_statistics
 */
void
gpujoin_accum_worker_statistics(GpuTaskState_v2 *gts)
{
	if (gts->css.methods == &gpujoin_exec_methods)
	{
		pgstromWorkerStatistics *worker_stat = gts->worker_stat;
		GpuJoinState   *gjs = (GpuJoinState *) gts;
		runtimeStat	   *rt_stat = gjs->rt_stat;
		int				i;

		for (i=0; i < gjs->num_rels; i++)
		{
			worker_stat->gpujoin[i].inner_nitems += rt_stat->inner_nitems[i];
			worker_stat->gpujoin[i].right_nitems += rt_stat->right_nitems[i];
		}
	}
}

/*
 * ExecGpuJoinEstimateDSM
 */
static Size
ExecGpuJoinEstimateDSM(CustomScanState *node,
					   ParallelContext *pcxt)
{
	if (node->ss.ss_currentRelation)
		return ExecGpuScanEstimateDSM(node, pcxt);
	return 0;
}

/*
 * ExecGpuJoinInitDSM
 */
static void
ExecGpuJoinInitDSM(CustomScanState *node,
				   ParallelContext *pcxt,
				   void *coordinate)
{
	GpuJoinState   *gjs = (GpuJoinState *) node;
	Size			len = offsetof(pgstromWorkerStatistics,
								   gpujoin[gjs->num_rels + 1]);

	gjs->gts.worker_stat = dmaBufferAlloc(gjs->gts.gcontext, len);
	memset(gjs->gts.worker_stat, 0, len);

	ExecGpuScanInitDSM(node, pcxt, coordinate);
}

/*
 * ExecGpuJoinInitWorker
 */
static void
ExecGpuJoinInitWorker(CustomScanState *node,
					  shm_toc *toc,
					  void *coordinate)
{
	if (node->ss.ss_currentRelation)
		ExecGpuScanInitWorker(node, toc, coordinate);
}

/*
 * gpujoin_codegen_var_decl
 *
 * declaration of the variables in 'used_var' list
 */
static void
gpujoin_codegen_var_param_decl(StringInfo source,
							   GpuJoinInfo *gj_info,
							   int cur_depth,
							   codegen_context *context)
{
	List	   *kern_vars = NIL;
	ListCell   *cell;
	int			depth;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);

	/*
	 * Pick up variables in-use and append its properties in the order
	 * corresponding to depth/resno.
	 */
	foreach (cell, context->used_vars)
	{
		Var		   *varnode = lfirst(cell);
		Var		   *kernode = NULL;
		ListCell   *lc1;
		ListCell   *lc2;
		ListCell   *lc3;

		Assert(IsA(varnode, Var));
		forthree (lc1, context->pseudo_tlist,
				  lc2, gj_info->ps_src_depth,
				  lc3, gj_info->ps_src_resno)
		{
			TargetEntry	*tle = lfirst(lc1);
			int		src_depth = lfirst_int(lc2);
			int		src_resno = lfirst_int(lc3);

			if (equal(tle->expr, varnode))
			{
				kernode = copyObject(varnode);
				kernode->varno = src_depth;			/* save the source depth */
				kernode->varattno = src_resno;		/* save the source resno */
				kernode->varoattno = tle->resno;	/* resno on the ps_tlist */
				if (src_depth < 0 || src_depth > cur_depth)
					elog(ERROR, "Bug? device varnode out of range");
				break;
			}
		}
		if (!kernode)
			elog(ERROR, "Bug? device varnode was not is ps_tlist: %s",
				 nodeToString(varnode));

		/*
		 * attach 'kernode' in the order corresponding to depth/resno.
		 */
		if (kern_vars == NIL)
			kern_vars = list_make1(kernode);
		else
		{
			lc2 = NULL;
			foreach (lc1, kern_vars)
			{
				Var	   *varnode = lfirst(lc1);

				if (varnode->varno > kernode->varno ||
					(varnode->varno == kernode->varno &&
					 varnode->varattno > kernode->varattno))
				{
					if (lc2 != NULL)
						lappend_cell(kern_vars, lc2, kernode);
					else
						kern_vars = lcons(kernode, kern_vars);
					break;
				}
				lc2 = lc1;
			}
			if (lc1 == NULL)
				kern_vars = lappend(kern_vars, kernode);
		}
	}

	/*
	 * parameter declaration
	 */
	pgstrom_codegen_param_declarations(source, context);

	/*
	 * variable declarations
	 */
	appendStringInfoString(
		source,
		"  HeapTupleHeaderData *htup  __attribute__((unused));\n"
		"  kern_data_store *kds_in    __attribute__((unused));\n"
		"  kern_colmeta *colmeta      __attribute__((unused));\n"
		"  void *datum                __attribute__((unused));\n");

	foreach (cell, kern_vars)
	{
		Var			   *kernode = lfirst(cell);
		devtype_info   *dtype;

		dtype = pgstrom_devtype_lookup(kernode->vartype);
		if (!dtype)
			elog(ERROR, "device type \"%s\" not found",
				 format_type_be(kernode->vartype));

		appendStringInfo(
			source,
			"  pg_%s_t KVAR_%u;\n",
			dtype->type_name,
			kernode->varoattno);
	}

	/*
	 * variable initialization
	 */
	depth = -1;
	foreach (cell, kern_vars)
	{
		Var			   *keynode = lfirst(cell);
		devtype_info   *dtype;

		dtype = pgstrom_devtype_lookup(keynode->vartype);
		if (!dtype)
			elog(ERROR, "device type \"%s\" not found",
				 format_type_be(keynode->vartype));

		if (depth != keynode->varno)
		{
			if (keynode->varno == 0)
			{
				/* htup from KDS */
				appendStringInfoString(
					source,
					"  /* variable load in depth-0 (outer KDS) */\n"
					"  colmeta = kds->colmeta;\n"
					"  if (!o_buffer)\n"
					"    htup = NULL;\n"
					"  else if (kds->format != KDS_FORMAT_BLOCK)\n"
					"    htup = KDS_ROW_REF_HTUP(kds,o_buffer[0],\n"
					"                            NULL,NULL);\n"
					"  else\n"
					"    htup = KDS_BLOCK_REF_HTUP(kds,o_buffer[0],\n"
					"                              NULL,NULL);\n");
			}
			else
			{
				/* in case of inner data store */
				appendStringInfo(
					source,
					"  /* variable load in depth-%u (data store) */\n"
					"  kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, %u);\n"
					"  assert(kds_in->format == %s);\n"
					"  colmeta = kds_in->colmeta;\n",
					keynode->varno,
					keynode->varno,
					list_nth(gj_info->hash_outer_keys,
							 keynode->varno - 1) == NIL
					? "KDS_FORMAT_ROW"
					: "KDS_FORMAT_HASH");

				if (keynode->varno < cur_depth)
					appendStringInfo(
						source,
						"  if (!o_buffer)\n"
						"    htup = NULL;\n"
						"  else\n"
						"    htup = KDS_ROW_REF_HTUP(kds_in,o_buffer[%d],\n"
						"                            NULL, NULL);\n",
						keynode->varno);
				else if (keynode->varno == cur_depth)
					appendStringInfo(
						source,
						"  htup = i_htup;\n"
						);
				else
					elog(ERROR, "Bug? too deeper varnode reference");
			}
			depth = keynode->varno;
		}
		appendStringInfo(
			source,
			"  datum = GPUJOIN_REF_DATUM(colmeta,htup,%u);\n"
			"  KVAR_%u = pg_%s_datum_ref(kcxt,datum,false);\n",
			keynode->varattno - 1,
			keynode->varoattno,
			dtype->type_name);
	}
	appendStringInfo(source, "\n");
}

/*
 * codegen for:
 * STATIC_FUNCTION(cl_bool)
 * gpujoin_join_quals_depth%u(kern_context *kcxt,
 *                            kern_data_store *kds,
 *                            kern_multirels *kmrels,
 *                            cl_int *o_buffer,
 *                            HeapTupleHeaderData *i_htup,
 *                            cl_bool *joinquals_matched)
 */
static void
gpujoin_codegen_join_quals(StringInfo source,
						   GpuJoinInfo *gj_info,
						   int cur_depth,
						   codegen_context *context)
{
	List	   *join_quals;
	List	   *other_quals;
	char	   *join_quals_code = NULL;
	char	   *other_quals_code = NULL;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);
	join_quals = list_nth(gj_info->join_quals, cur_depth - 1);
	other_quals = list_nth(gj_info->other_quals, cur_depth - 1);

	/*
	 * make a text representation of join_qual
	 */
	context->used_vars = NIL;
	context->param_refs = NULL;
	if (join_quals != NIL)
		join_quals_code = pgstrom_codegen_expression((Node *)join_quals,
													 context);
	if (other_quals != NIL)
		other_quals_code = pgstrom_codegen_expression((Node *)other_quals,
													  context);
	/*
	 * function declaration
	 */
	appendStringInfo(
		source,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpujoin_join_quals_depth%d(kern_context *kcxt,\n"
		"                           kern_data_store *kds,\n"
        "                           kern_multirels *kmrels,\n"
		"                           cl_uint *o_buffer,\n"
		"                           HeapTupleHeaderData *i_htup,\n"
		"                           cl_bool *joinquals_matched)\n"
		"{\n",
		cur_depth);

	/*
	 * variable/params declaration & initialization
	 */
	gpujoin_codegen_var_param_decl(source, gj_info, cur_depth, context);

	/*
	 * evaluation of other-quals and join-quals
	 */
	if (join_quals_code != NULL)
	{
		appendStringInfo(
			source,
			"  if (i_htup && o_buffer && !EVAL(%s))\n"
			"  {\n"
			"    if (joinquals_matched)\n"
			"      *joinquals_matched = false;\n"
			"    return false;\n"
			"  }\n",
			join_quals_code);
	}
	appendStringInfo(
		source,
		"  if (joinquals_matched)\n"
		"    *joinquals_matched = true;\n");
	if (other_quals_code != NULL)
	{
		appendStringInfo(
			source,
			"  if (!EVAL(%s))\n"
			"    return false;\n",
			other_quals_code);
	}
	appendStringInfo(
		source,
		"  return true;\n"
		"}\n");
}

/*
 * codegen for:
 * STATIC_FUNCTION(cl_uint)
 * gpujoin_hash_value_depth%u(kern_context *kcxt,
 *                            cl_uint *pg_crc32_table,
 *                            kern_data_store *kds,
 *                            kern_multirels *kmrels,
 *                            cl_int *outer_index,
 *                            cl_bool *is_null_keys)
 */
static void
gpujoin_codegen_hash_value(StringInfo source,
						   GpuJoinInfo *gj_info,
						   int cur_depth,
						   codegen_context *context)
{
	StringInfoData	body;
	List		   *hash_outer_keys;
	ListCell	   *lc;

	Assert(cur_depth > 0 && cur_depth <= gj_info->num_rels);
	hash_outer_keys = list_nth(gj_info->hash_outer_keys, cur_depth - 1);
	Assert(hash_outer_keys != NIL);

	appendStringInfo(
		source,
		"STATIC_FUNCTION(cl_uint)\n"
		"gpujoin_hash_value_depth%u(kern_context *kcxt,\n"
		"                           cl_uint *pg_crc32_table,\n"
		"                           kern_data_store *kds,\n"
		"                           kern_multirels *kmrels,\n"
		"                           cl_uint *o_buffer,\n"
		"                           cl_bool *p_is_null_keys)\n"
		"{\n"
		"  pg_anytype_t temp    __attribute__((unused));\n"
		"  cl_uint hash;\n"
		"  cl_bool is_null_keys = true;\n"
		"\n",
		cur_depth);

	context->used_vars = NIL;
	context->param_refs = NULL;

	initStringInfo(&body);
	appendStringInfo(
		&body,
		"  /* Hash-value calculation */\n"
		"  INIT_LEGACY_CRC32(hash);\n");
	foreach (lc, hash_outer_keys)
	{
		Node	   *key_expr = lfirst(lc);
		Oid			key_type = exprType(key_expr);
		devtype_info *dtype;

		dtype = pgstrom_devtype_lookup(key_type);
		if (!dtype)
			elog(ERROR, "Bug? device type \"%s\" not found",
                 format_type_be(key_type));
		appendStringInfo(
			&body,
			"  temp.%s_v = %s;\n"
			"  if (!temp.%s_v.isnull)\n"
			"    is_null_keys = false;\n"
			"  hash = pg_%s_comp_crc32(pg_crc32_table, hash, temp.%s_v);\n",
			dtype->type_name,
			pgstrom_codegen_expression(key_expr, context),
			dtype->type_name,
			dtype->type_name,
			dtype->type_name);
	}
	appendStringInfo(&body, "  FIN_LEGACY_CRC32(hash);\n");

	/*
	 * variable/params declaration & initialization
	 */
	gpujoin_codegen_var_param_decl(source, gj_info, cur_depth, context);

	appendStringInfo(
		source,
		"%s"
		"\n"
		"  *p_is_null_keys = is_null_keys;\n"
		"  return hash;\n"
		"}\n"
		"\n",
		body.data);
	pfree(body.data);
}

/*
 * gpujoin_codegen_projection
 *
 * It makes a device function for device projection.
 */
static void
gpujoin_codegen_projection(StringInfo source,
						   CustomScan *cscan,
						   GpuJoinInfo *gj_info,
						   codegen_context *context,
						   cl_uint *p_extra_maxlen)
{
	List		   *tlist_dev = cscan->custom_scan_tlist;
	List		   *ps_src_depth = gj_info->ps_src_depth;
	List		   *ps_src_resno = gj_info->ps_src_resno;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	AttrNumber	   *varattmaps;
	Bitmapset	   *refs_by_vars = NULL;
	Bitmapset	   *refs_by_expr = NULL;
	StringInfoData	body;
	StringInfoData	temp;
	cl_int			depth;
	cl_uint			extra_maxlen;
	cl_bool			is_first;

	varattmaps = palloc(sizeof(AttrNumber) * list_length(tlist_dev));
	initStringInfo(&body);
	initStringInfo(&temp);

	/*
	 * Pick up all the var-node referenced directly or indirectly by
	 * device expressions; which are resjunk==false.
	 */
	forthree (lc1, tlist_dev,
			  lc2, ps_src_depth,
			  lc3, ps_src_resno)
	{
		TargetEntry	*tle = lfirst(lc1);
		cl_int		src_depth = lfirst_int(lc2);

		if (tle->resjunk)
			continue;
		if (src_depth >= 0)
		{
			refs_by_vars = bms_add_member(refs_by_vars, tle->resno -
										  FirstLowInvalidHeapAttributeNumber);
		}
		else
		{
			List	   *expr_vars = pull_vars_of_level((Node *)tle->expr, 0);
			ListCell   *cell;

			foreach (cell, expr_vars)
			{
				TargetEntry	   *__tle = tlist_member(lfirst(cell), tlist_dev);

				if (!__tle)
					elog(ERROR, "Bug? no indirectly referenced Var-node exists in custom_scan_tlist");
				refs_by_expr = bms_add_member(refs_by_expr, __tle->resno -
										FirstLowInvalidHeapAttributeNumber);
			}
			list_free(expr_vars);
		}
	}

	appendStringInfoString(
		source,
		"STATIC_FUNCTION(void)\n"
		"gpujoin_projection(kern_context *kcxt,\n"
		"                   kern_data_store *kds_src,\n"
		"                   kern_multirels *kmrels,\n"
		"                   cl_uint *r_buffer,\n"
		"                   kern_data_store *kds_dst,\n"
		"                   Datum *tup_values,\n"
		"                   cl_bool *tup_isnull,\n"
		"                   cl_short *tup_depth,\n"
		"                   cl_char *extra_buf,\n"
		"                   cl_uint *extra_len)\n"
		"{\n"
		"  HeapTupleHeaderData *htup    __attribute__((unused));\n"
		"  kern_data_store *kds_in      __attribute__((unused));\n"
		"  ItemPointerData  t_self      __attribute__((unused));\n"
		"  char *addr                   __attribute__((unused));\n"
		"  char *extra_pos = extra_buf;\n"
		"  pg_anytype_t temp            __attribute__((unused));\n");

	for (depth=0; depth <= gj_info->num_rels; depth++)
	{
		List	   *kvars_srcnum = NIL;
		List	   *kvars_dstnum = NIL;
		const char *kds_label;
		cl_int		i, nattrs = -1;

		/* collect information in this depth */
		memset(varattmaps, 0, sizeof(AttrNumber) * list_length(tlist_dev));

		forthree (lc1, tlist_dev,
				  lc2, ps_src_depth,
				  lc3, ps_src_resno)
		{
			TargetEntry *tle = lfirst(lc1);
			cl_int		src_depth = lfirst_int(lc2);
			cl_int		src_resno = lfirst_int(lc3);
			cl_int		k = tle->resno - FirstLowInvalidHeapAttributeNumber;

			if (depth != src_depth)
				continue;
			if (bms_is_member(k, refs_by_vars))
				varattmaps[tle->resno - 1] = src_resno;
			if (bms_is_member(k, refs_by_expr))
			{
				kvars_srcnum = lappend_int(kvars_srcnum, src_resno);
				kvars_dstnum = lappend_int(kvars_dstnum, tle->resno);
			}
			if (bms_is_member(k, refs_by_vars) ||
				bms_is_member(k, refs_by_expr))
				nattrs = Max(nattrs, src_resno);
		}

		/* no need to extract inner/outer tuple in this depth */
		if (nattrs < 1)
			continue;

		appendStringInfo(
			&body,
			"  /* ---- extract %s relation (depth=%d) */\n",
			depth > 0 ? "inner" : "outer", depth);

		if (depth == 0)
			kds_label = "kds_src";
		else
		{
			appendStringInfo(
				&body,
				"  kds_in = KERN_MULTIRELS_INNER_KDS(kmrels, %d);\n",
				depth);
			kds_label = "kds_in";
		}
		appendStringInfo(
			&body,
			"  if (r_buffer[%d] == 0)\n"
			"    htup = NULL;\n",
			depth);
		if (depth == 0)
			appendStringInfo(
				&body,
				"  else if (%s->format == KDS_FORMAT_BLOCK)\n"
				"    htup = KDS_BLOCK_REF_HTUP(%s,r_buffer[%d],&t_self,NULL);\n",
				kds_label,
				kds_label, depth);
		appendStringInfo(
			&body,
			"  else\n"
			"    htup = KDS_ROW_REF_HTUP(%s,r_buffer[%d],&t_self,NULL);\n",
			kds_label, depth);

		/* System column reference if any */
		foreach (lc1, tlist_dev)
		{
			TargetEntry		   *tle = lfirst(lc1);
			Form_pg_attribute	attr;

			if (varattmaps[tle->resno-1] >= 0)
				continue;
			attr = SystemAttributeDefinition(varattmaps[tle->resno-1], true);
			appendStringInfo(
				&body,
				"  /* %s system column */\n"
				"  if (!htup)\n"
				"    tup_isnull[%d] = true;\n"
				"  else {\n"
				"    tup_isnull[%d] = false;\n"
				"    tup_values[%d] = kern_getsysatt_%s(%s, htup, &t_self);\n"
				"  }\n",
				NameStr(attr->attname),
				tle->resno-1,
				tle->resno-1,
				tle->resno-1,
				NameStr(attr->attname),
				kds_label);
		}

		/* begin to walk on the tuple */
		appendStringInfo(
			&body,
			"  EXTRACT_HEAP_TUPLE_BEGIN(addr, %s, htup);\n", kds_label);

		resetStringInfo(&temp);
		for (i=1; i <= nattrs; i++)
		{
			TargetEntry	   *tle;
			int16			typelen;
			bool			typebyval;
			cl_bool			referenced = false;

			foreach (lc1, tlist_dev)
			{
				tle = lfirst(lc1);

				if (varattmaps[tle->resno - 1] != i)
					continue;
				/* attribute shall be directly copied */
				get_typlenbyval(exprType((Node *)tle->expr),
								&typelen, &typebyval);
				if (!typebyval)
				{
					appendStringInfo(
						&temp,
						"  tup_isnull[%d] = (addr != NULL ? false : true);\n"
						"  tup_values[%d] = PointerGetDatum(addr);\n"
						"  tup_depth[%d] = %d;\n",
						tle->resno - 1,
						tle->resno - 1,
						tle->resno - 1, depth);
				}
				else
				{
					appendStringInfo(
						&temp,
						"  tup_isnull[%d] = (addr != NULL ? false : true);\n"
						"  if (addr)\n"
						"    tup_values[%d] = *((%s *) addr);\n"
						"  tup_depth[%d] = %d;\n",
						tle->resno - 1,
                        tle->resno - 1,
						(typelen == sizeof(cl_long)  ? "cl_long" :
						 typelen == sizeof(cl_int)   ? "cl_int" :
						 typelen == sizeof(cl_short) ? "cl_short"
													 : "cl_char"),
						tle->resno - 1, depth);
				}
				referenced = true;
			}

			forboth (lc1, kvars_srcnum,
					 lc2, kvars_dstnum)
			{
				devtype_info   *dtype;
				cl_int			src_num = lfirst_int(lc1);
				cl_int			dst_num = lfirst_int(lc2);
				Oid				type_oid;

				if (src_num != i)
					continue;
				/* add KVAR_%u declarations */
				tle = list_nth(tlist_dev, dst_num - 1);
				type_oid = exprType((Node *)tle->expr);
				dtype = pgstrom_devtype_lookup(type_oid);
				if (!dtype)
					elog(ERROR, "cache lookup failed for device type: %s",
						 format_type_be(type_oid));

				appendStringInfo(
					source,
					"  pg_%s_t KVAR_%u;\n",
					dtype->type_name,
					dst_num);
				appendStringInfo(
					&temp,
					"  KVAR_%u = pg_%s_datum_ref(kcxt, addr, false);\n",
					dst_num,
					dtype->type_name);

				referenced = true;
			}

			/* flush to the main buffer */
			if (referenced)
			{
				appendStringInfoString(&body, temp.data);
				resetStringInfo(&temp);
			}
			appendStringInfoString(
				&temp,
				"  EXTRACT_HEAP_TUPLE_NEXT(addr);\n");
		}
		appendStringInfoString(
			&body,
			"  EXTRACT_HEAP_TUPLE_END();\n");
	}

	/*
	 * Execution of the expression
	 */
	is_first = true;
	extra_maxlen = 0;
	forboth (lc1, tlist_dev,
			 lc2, ps_src_depth)
	{
		TargetEntry	   *tle = lfirst(lc1);
		cl_int			src_depth = lfirst_int(lc2);
		devtype_info   *dtype;

		if (tle->resjunk || src_depth >= 0)
			continue;

		if (is_first)
		{
			appendStringInfoString(
				&body,
				"\n"
				"  /* calculation of expressions */\n");
			is_first = false;
		}

		dtype = pgstrom_devtype_lookup(exprType((Node *) tle->expr));
		if (!dtype)
			elog(ERROR, "cache lookup failed for device type: %s",
				 format_type_be(exprType((Node *) tle->expr)));

		if (dtype->type_oid == NUMERICOID)
		{
			extra_maxlen += 32;
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"  {\n"
				"    cl_uint numeric_len =\n"
				"        pg_numeric_to_varlena(kcxt, extra_pos,\n"
				"                              temp.%s_v.value,\n"
				"                              temp.%s_v.isnull);\n"
				"    tup_values[%d] = PointerGetDatum(extra_pos);\n"
				"    extra_pos += MAXALIGN(numeric_len);\n"
				"  }\n"
				"  tup_depth[%d] = -1;\n",	/* use of local extra_buf */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				tle->resno - 1);
		}
		else if (dtype->type_byval)
		{
			/* fixed length built-in data type */
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"    tup_values[%d] = pg_%s_to_datum(temp.%s_v.value);\n"
				"  tup_depth[%d] = -255;\n",	/* just a poison */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1);
		}
		else if (dtype->type_length > 0)
		{
			/* fixed length pointer data type */
			extra_maxlen += MAXALIGN(dtype->type_length);
			appendStringInfo(
				&body,
				"  temp.%s_v = %s;\n"
				"  tup_isnull[%d] = temp.%s_v.isnull;\n"
				"  if (!temp.%s_v.isnull)\n"
				"  {\n"
				"    memcpy(extra_pos, &temp.%s_v.value,\n"
				"           sizeof(temp.%s_v.value));\n"
				"    tup_values[%d] = PointerGetDatum(extra_pos);\n"
				"    extra_pos += MAXALIGN(sizeof(temp.%s_v.value));\n"
				"  }\n"
				"  tup_depth[%d] = -1;\n",	/* use of local extra_buf */
				dtype->type_name,
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				dtype->type_name,
				tle->resno - 1,
				dtype->type_name,
				tle->resno - 1);
		}
		else
		{
			/*
			 * variable length pointer data type
			 *
			 * Pay attention for the case when expression may return varlena
			 * data type, even though we have no device function that can
			 * return a varlena function. Like:
			 *   CASE WHEN x IS NOT NULL THEN x ELSE 'no value' END
			 * In this case, a varlena data returned by the expression is
			 * located on either any of KDS buffer or KPARAMS buffer.
			 *
			 * Unless it is not obvious by the node type, we have to walk on
			 * the possible buffer range to find out right one. :-(
			 */
			appendStringInfo(
				&body,
				"  temp.varlena_v = %s;\n"
				"  tup_isnull[%d] = temp.varlena_v.isnull;\n"
				"  tup_values[%d] = PointerGetDatum(temp.varlena_v.value);\n",
				pgstrom_codegen_expression((Node *)tle->expr, context),
				tle->resno - 1,
				tle->resno - 1);

			if (IsA(tle->expr, Const) || IsA(tle->expr, Param))
			{
				/* always references to the kparams buffer */
				appendStringInfo(
					&body,
					"  tup_depth[%d] = -2;\n",
					tle->resno - 1);
			}
			else
			{
				cl_int		i;

				appendStringInfo(
					&body,
					"  if (temp.varlena_v.isnull)\n"
					"    tup_depth[%d] = -9999; /* never referenced */\n"
					"  else if (pointer_on_kparams(temp.varlena_v.value,\n"
					"                              kcxt->kparams))\n"
					"    tup_depth[%d] = -2;\n"
					"  else if (pointer_on_kds(temp.varlena_v.value,\n"
					"                          kds_dst))\n"
					"    tup_depth[%d] = -1;\n"
					"  else if (pointer_on_kds(temp.varlena_v.value,\n"
					"                          kds_src))\n"
					"    tup_depth[%d] = 0;\n",
					tle->resno - 1,
					tle->resno - 1,
					tle->resno - 1,
					tle->resno - 1);
				for (i=1; i <= gj_info->num_rels; i++)
				{
					appendStringInfo(
						&body,
						"  else if (pointer_on_kds(temp.varlena_v.value,\n"
						"           KERN_MULTIRELS_INNER_KDS(kmrels,%d)))\n"
						"    tup_depth[%d] = %d;\n",
						i, tle->resno - 1, i);
				}
				appendStringInfo(
					&body,
					"  else\n"
					"    tup_depth[%d] = -9999; /* should never happen */\n",
					tle->resno - 1);
			}
		}
	}
	/* how much extra field required? */
	appendStringInfoString(
		&body,
		"\n"
		"  *extra_len = (cl_uint)(extra_pos - extra_buf);\n");
	/* add parameter declarations */
	pgstrom_codegen_param_declarations(source, context);
	/* merge with declaration part */
	appendStringInfo(source, "\n%s}\n", body.data);

	*p_extra_maxlen = extra_maxlen;

	pfree(body.data);
	pfree(temp.data);
}

static char *
gpujoin_codegen(PlannerInfo *root,
				CustomScan *cscan,
				GpuJoinInfo *gj_info,
				List *tlist,
				codegen_context *context)
{
	StringInfoData source;
	int			depth;
	ListCell   *cell;

	initStringInfo(&source);

	/*
	 * gpuscan_quals_eval
	 */
	codegen_gpuscan_quals(&source,
						  context,
						  cscan->scan.scanrelid,
						  gj_info->outer_quals);
	/*
	 * gpujoin_join_quals
	 */
	context->pseudo_tlist = cscan->custom_scan_tlist;
	for (depth=1; depth <= gj_info->num_rels; depth++)
		gpujoin_codegen_join_quals(&source, gj_info, depth, context);
	appendStringInfo(
		&source,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpujoin_join_quals(kern_context *kcxt,\n"
		"                   kern_data_store *kds,\n"
		"                   kern_multirels *kmrels,\n"
		"                   int depth,\n"
		"                   cl_uint *o_buffer,\n"
		"                   HeapTupleHeaderData *i_htup,\n"
		"                   cl_bool *needs_outer_row)\n"
		"{\n"
		"  switch (depth)\n"
		"  {\n");

	for (depth=1; depth <= gj_info->num_rels; depth++)
	{
		appendStringInfo(
			&source,
			"  case %d:\n"
			"    return gpujoin_join_quals_depth%d(kcxt, kds, kmrels, o_buffer, i_htup, needs_outer_row);\n",
			depth, depth);
	}
	appendStringInfo(
		&source,
		"  default:\n"
		"    STROM_SET_ERROR(&kcxt->e, StromError_SanityCheckViolation);\n"
		"    break;\n"
		"  }\n"
		"  return false;\n"
		"}\n\n");


	depth = 1;
	foreach (cell, gj_info->hash_outer_keys)
	{
		if (lfirst(cell) != NULL)
			gpujoin_codegen_hash_value(&source, gj_info, depth, context);
		depth++;
	}

	/*
	 * gpujoin_hash_value
	 */
	appendStringInfo(
		&source,
		"STATIC_FUNCTION(cl_uint)\n"
		"gpujoin_hash_value(kern_context *kcxt,\n"
		"                   cl_uint *pg_crc32_table,\n"
		"                   kern_data_store *kds,\n"
		"                   kern_multirels *kmrels,\n"
		"                   cl_int depth,\n"
		"                   cl_uint *o_buffer,\n"
		"                   cl_bool *p_is_null_keys)\n"
		"{\n"
		"  switch (depth)\n"
		"  {\n");
	depth = 1;
	foreach (cell, gj_info->hash_outer_keys)
	{
		if (lfirst(cell) != NULL)
		{
			appendStringInfo(
				&source,
				"  case %u:\n"
				"    return gpujoin_hash_value_depth%u(kcxt,pg_crc32_table,\n"
				"                                      kds,kmrels,o_buffer,\n"
				"                                      p_is_null_keys);\n",
				depth, depth);
		}
		depth++;
	}
	appendStringInfo(
		&source,
		"  default:\n"
		"    STROM_SET_ERROR(&kcxt->e, StromError_SanityCheckViolation);\n"
		"    break;\n"
		"  }\n"
		"  return (cl_uint)(-1);\n"
		"}\n"
		"\n");

	/*
	 * gpujoin_projection
	 */
	gpujoin_codegen_projection(&source, cscan, gj_info, context,
							   &gj_info->extra_maxlen);

	return source.data;
}

/*
 * gpujoin_exec_estimate_nitems
 *
 * 
 *
 */
static double
gpujoin_exec_estimate_nitems(GpuJoinState *gjs,
							 pgstrom_gpujoin *pgjoin,
							 kern_join_scale *jscale_old,
							 double ntuples_in,
							 int depth)
{
	pgstrom_multirels  *pmrels = pgjoin->pmrels;
	innerState		   *istate = (depth > 0 ? gjs->inners + depth - 1 : NULL);
	runtimeStat		   *rt_stat = gjs->rt_stat;
	kern_join_scale	   *jscale = pgjoin->kern.jscale;
	size_t				source_ntasks;
	size_t				source_nitems;
	size_t				inner_nitems;
	size_t				right_nitems;
	double				ntuples_next;
	double				merge_ratio;
	double				plan_ratio;
	double				exec_ratio;

	/*
	 * Nrows estimation based on plan estimation and exec statistics.
	 * It shall be merged according to the task progress.
	 */
	SpinLockAcquire(&rt_stat->lock);
	source_ntasks = rt_stat->source_ntasks;
	source_nitems = rt_stat->source_nitems;
	SpinLockRelease(&rt_stat->lock);
	merge_ratio = Max((double) source_ntasks / 20.0,
					  gjs->outer_nrows > 0.0
					  ? ((double)(source_nitems) /
						 (double)(0.30 * gjs->outer_nrows))
					  : 0.0);
	merge_ratio = Min(1.0, merge_ratio);	/* up to 100% */

	/* special case handling for outer_quals evaluation */
	if (depth == 0)
	{
		pgstrom_data_store *pds_src = pgjoin->pds_src;

		/* RIGHT OUTER JOIN has no input rows to be processed */
		if (!pds_src)
			return 0.0;

		/*
		 * In case of the GpuJoin task re-enqueue with partition window,
		 * last execution result is the most reliable hint, because next
		 * task will have same evaluation to the same data, so we can
		 * expect same results.
		 */
		if (jscale_old != NULL)
		{
			ntuples_next = ((double)jscale[0].window_size *
							(double)(jscale_old[1].inner_nitems) /
							(double)(jscale_old[0].window_base +
									 jscale_old[0].window_size -
									 jscale_old[0].window_orig));
			if (pds_src->kds.format == KDS_FORMAT_BLOCK)
				ntuples_next *= 1.1 * (double)pds_src->kds.nrows_per_block;
			return ntuples_next;
		}

		if (!gjs->outer_quals)
		{
			/* nobody will filter out input rows if no outer quals */
			ntuples_next = (double) jscale[0].window_size;
		}
		else
		{
			/*
			 * We try to estimate amount of outer rows which are not elimiated
			 * by the qualifier, based on plan/exec time statistics
			 */
			SpinLockAcquire(&rt_stat->lock);
			inner_nitems = rt_stat->inner_nitems[0];
			right_nitems = rt_stat->right_nitems[0];
			source_nitems = rt_stat->source_nitems;
			SpinLockRelease(&rt_stat->lock);

			/*
			 * If there are no run-time statistics, we have no options except
			 * for relying on the plan estimation
			 */
			if (source_nitems == 0)
				ntuples_next = jscale[0].window_size * gjs->outer_ratio;
			else
			{
				/*
				 * Elsewhere, we mix the plan estimation and run-time
				 * statistics according to the outer scan progress.
				 * Once merge_ratio gets 100%, plan estimation shall be
				 * entirely ignored.
				 */
				plan_ratio = gjs->outer_ratio;
				exec_ratio = ((double)(inner_nitems + right_nitems) /
							  (double)(source_nitems));
				ntuples_next =  ((exec_ratio * merge_ratio +
								  plan_ratio * (1.0 - merge_ratio)) *
								 (double) jscale[0].window_size);
			}
		}

		/*
		 * In case of KDS_FORMAT_BLOCK, kds->nitems means number of blocks,
		 * not tuples. So, we need to adjust ntuples_next for size estimation
		 * purpose
		 */
		if (pds_src->kds.format == KDS_FORMAT_BLOCK)
			ntuples_next *= 1.1 * (double)gjs->gts.outer_nrows_per_block;

		return ntuples_next;
	}

	/*
	 * Obviously, no input rows will produce an empty results without
	 * RIGHT OUTER JOIN.
	 */
	if (ntuples_in <= 0.0)
		ntuples_next = 0.0;
	else
	{
		/*
		 * In case of task re-enqueue with virtual partition window
		 * shift, last execution result is the most reliable hint.
		 */
		if (jscale_old &&
			(jscale_old[depth - 1].inner_nitems +
			 jscale_old[depth - 1].right_nitems) > 0)
		{
			ntuples_next = ntuples_in *
				((double)(jscale_old[depth].inner_nitems) /
				 (double)(jscale_old[depth - 1].inner_nitems +
						  jscale_old[depth - 1].right_nitems)) *
				((double)(jscale[depth].window_size) /
				 (double)(jscale_old[depth].window_base +
						  jscale_old[depth].window_size -
						  jscale_old[depth].window_orig));
		}
		else
		{
			pgstrom_data_store *pds_in = pmrels->inner_chunks[depth - 1];
			cl_uint			nitems_in = pds_in->kds.nitems;
			size_t			next_nitems;

			SpinLockAcquire(&rt_stat->lock);
			inner_nitems = rt_stat->inner_nitems[depth - 1];
			right_nitems = rt_stat->right_nitems[depth - 1];
			next_nitems = rt_stat->inner_nitems[depth];
			SpinLockRelease(&rt_stat->lock);

			plan_ratio = istate->nrows_ratio;
			if (inner_nitems + right_nitems > 0)
				exec_ratio = ((double)(next_nitems) /
							  (double)(inner_nitems + right_nitems));
			else
				exec_ratio = 0.0;

			if (nitems_in == 0)
				ntuples_next = 0.0;
			else
				ntuples_next = ntuples_in *
					(exec_ratio * merge_ratio +
					 plan_ratio * (1.0 - merge_ratio)) *
					((double)jscale[depth].window_size / (double)nitems_in);
		}
	}

	/*
	 * RIGHT/FULL OUTER JOIN will suddenly produce rows in this depth
	 */
	if (!pgjoin->pds_src && (istate->join_type == JOIN_RIGHT ||
							 istate->join_type == JOIN_FULL))
	{
		pgstrom_data_store *pds_in = pmrels->inner_chunks[depth - 1];

		if (jscale[depth].window_size > 0)
		{
			/*
			 * In case of task re-enqueue with inner window shift,
			 * last execution result is the most reliable hint.
			 */
			if (jscale_old)
			{
				ntuples_next += (double) jscale_old[depth].right_nitems *
					((double)(jscale[depth].window_size) /
					 (double)(jscale_old[depth].window_base +
							  jscale_old[depth].window_size -
							  jscale_old[depth].window_orig));
			}
			else
			{
				/*
				 * Right now, we assume unmatched row ratio using
				 *  1.0 - SQRT(# of result rows) / (# of inner rows)
				 *
				 * XXX - We may need more exact statistics on outer_join_map
				 */
				cl_uint	nitems_in = pds_in->kds.nitems;
				double	match_ratio;

				if (nitems_in == 0)
					match_ratio = 1.0;	/* an obvious case */
				else
				{
					SpinLockAcquire(&rt_stat->lock);
					inner_nitems = rt_stat->inner_nitems[depth];
					right_nitems = rt_stat->right_nitems[depth];
					SpinLockRelease(&rt_stat->lock);
					match_ratio = sqrt((double)(inner_nitems + right_nitems) /
									   (double)(nitems_in));
					match_ratio = 1.0 - Min(1.0, match_ratio);
					match_ratio = Max(0.05, match_ratio);	/* at least 5% */
				}
				ntuples_next += match_ratio * jscale[depth].window_size;
			}
		}
	}
	return ntuples_next;
}


/*
 * gpujoin_exec_estimate_dest_buffer
 *
 * Run-time estimation of the destination buffer
 */
static pgstrom_data_store *
gpujoin_attach_result_buffer(GpuJoinState *gjs,
							 pgstrom_gpujoin *pgjoin,
							 double ntuples, cl_int target_depth)
{
	GpuContext_v2  *gcontext = gjs->gts.gcontext;
	TupleTableSlot *tupslot = gjs->gts.css.ss.ss_ScanTupleSlot;
	TupleDesc		tupdesc = tupslot->tts_tupleDescriptor;
	cl_int			ncols = tupdesc->natts;
	Size			nrooms = (Size)(ntuples * pgstrom_chunk_size_margin);
	runtimeStat	   *rt_stat = pgjoin->rt_stat;
	pgstrom_data_store *pds_dst;

	/*
	 * Calculation of the pds_dst length - If we have no run-time information,
	 * all we can do is statistic based estimation. Elsewhere, kds->nitems
	 * will tell us maximum number of row-slot consumption last time.
	 * If StromError_DataStoreNoSpace happen due to lack of kern_resultbuf,
	 * previous kds->nitems may shorter than estimation. So, for safety,
	 * we adopts the larger one.
	 */


	if (!gjs->gts.row_format)
	{
		/* KDS_FORMAT_SLOT */
		Size	length = (STROMALIGN(offsetof(kern_data_store,
											  colmeta[ncols])) +
						  LONGALIGN((sizeof(Datum) +
									 sizeof(char)) * ncols +
									gjs->extra_maxlen) * nrooms);

		/* Adjustment if too short or too large */
		if (ncols == 0)
		{
			/* MEMO: Typical usage of ncols == 0 is GpuJoin underlying
			 * COUNT(*) because it does not need to put any contents in
			 * the slot. So, we can allow to increment nitems as long as
			 * 32bit width. :-)
			 */
			Assert(gjs->extra_maxlen == 0);
			nrooms = INT_MAX;
		}
		else if (length < pgstrom_chunk_size() / 2)
		{
			/*
			 * MEMO: If destination buffer size is too small, we doubt
			 * incorrect estimation by planner, so we try to prepare at
			 * least half of the pgstrom_chunk_size().
			 */
			nrooms = (pgstrom_chunk_size() / 2 -
					  STROMALIGN(offsetof(kern_data_store,
										  colmeta[ncols])))
				/ (LONGALIGN((sizeof(Datum) +
							  sizeof(char)) * ncols) + gjs->extra_maxlen);
		}
		else if (length > pgstrom_chunk_size_limit())
		{
			/*
			 * MEMO: If expected result buffer length was too much,
			 * we retry size estimation with smaller inner window.
			 */
			cl_int	nsplit = length / pgstrom_chunk_size_limit() + 1;

			Assert(target_depth > 0 && target_depth <= gjs->num_rels);
			pgjoin->kern.jscale[target_depth].window_size
				= (pgjoin->kern.jscale[target_depth].window_size / nsplit) + 1;
			if (pgjoin->kern.jscale[target_depth].window_size <= 1)
				elog(ERROR, "Too much growth of result rows");
			return NULL;
		}
		pds_dst = PDS_create_slot(gjs->gts.gcontext,
								  tupdesc,
								  nrooms,
								  gjs->extra_maxlen * nrooms,
								  false);
	}
	else
	{
		/* KDS_FORMAT_ROW */
		double		merge_ratio;
		double		tup_width;
		size_t		source_ntasks;
		size_t		source_nitems;
		size_t		results_nitems;
		size_t		results_usage;
		Size		length;

		/*
		 * Tuple width estimation also follow the logic when we estimate
		 * number of rows.
		 */
		SpinLockAcquire(&rt_stat->lock);
		source_ntasks = rt_stat->source_ntasks;
		source_nitems = rt_stat->source_nitems;
		results_nitems = rt_stat->results_nitems;
		results_usage = rt_stat->results_usage;
		SpinLockRelease(&rt_stat->lock);

		merge_ratio = Max((double) source_ntasks / 20.0,
						  (double) source_nitems /
						  (double)(0.30 * gjs->outer_nrows));
		if (results_nitems == 0)
		{
			tup_width = gjs->result_width;
		}
		else if (merge_ratio < 1.0)
		{
			double	plan_width = gjs->result_width;
			double	exec_width = ((double) results_usage /
								  (double) results_nitems);
			tup_width = (plan_width * (1.0 - merge_ratio) +
						 exec_width * merge_ratio);
		}
		else
		{
			tup_width = ((double) results_usage /
						 (double) results_nitems);
		}

		/* Expected buffer length */
		length = (STROMALIGN(offsetof(kern_data_store,
									  colmeta[ncols])) +
				  STROMALIGN(sizeof(cl_uint) * nrooms) +
				  MAXALIGN(offsetof(kern_tupitem, htup) +
						   ceill(tup_width)) * nrooms);
		if (length < pgstrom_chunk_size() / 2)
			length = pgstrom_chunk_size() / 2;
		else if (length > pgstrom_chunk_size_limit())
		{
			Size		small_nrooms;
			cl_int		nsplit;

			/* maximum number of tuples we can store */
			small_nrooms = (pgstrom_chunk_size_limit() -
							STROMALIGN(offsetof(kern_data_store,
												colmeta[ncols])))
				/ (sizeof(cl_uint) +
				   MAXALIGN(offsetof(kern_tupitem, htup) +
							ceill(tup_width)));
			nsplit = nrooms / small_nrooms + 1;
			pgjoin->kern.jscale[target_depth].window_size
				= pgjoin->kern.jscale[target_depth].window_size / nsplit + 1;
			if (pgjoin->kern.jscale[target_depth].window_size <= 1)
				elog(ERROR, "Too much growth of result rows");
			return NULL;
		}
		pds_dst = PDS_create_row(gcontext, tupdesc, length);
	}
	return pds_dst;
}



/*
 * gpujoin_create_task
 *
 *
 *
 */
static GpuTask_v2 *
gpujoin_create_task(GpuJoinState *gjs,
					pgstrom_multirels *pmrels,
					pgstrom_data_store *pds_src,
					int file_desc,
					kern_join_scale *jscale_old)
{
	GpuContext_v2	   *gcontext = gjs->gts.gcontext;
	runtimeStat		   *rt_stat = gjs->rt_stat;
	pgstrom_gpujoin	   *pgjoin;
	double				ntuples;
	double				ntuples_next;
	double				ntuples_delta;
	Size				length;
	Size				required;
	Size				max_items;
	cl_int				i, j, depth;
	cl_int				target_depth;
	cl_double			target_row_dist_score;
	cl_bool				jscale_rewind = false;
	kern_parambuf	   *kparams __attribute__((unused));

	/* allocation of GpuJoinTask */
	length = offsetof(pgstrom_gpujoin, kern) +
		STROMALIGN(offsetof(kern_gpujoin, jscale[gjs->num_rels + 1]));
	required = length + STROMALIGN(gjs->gts.kern_params->length);
	pgjoin = dmaBufferAlloc(gcontext, required);
	memset(pgjoin, 0, length);

	pgstromInitGpuTask(&gjs->gts, &pgjoin->task);
	pgjoin->task.file_desc = file_desc;
	pgjoin->pmrels = multirels_attach_buffer(pmrels);
	pgjoin->pds_src = pds_src;
	pgjoin->pds_dst = NULL;		/* to be set later */
	pgjoin->rt_stat = gjs->rt_stat;

	/* Is NVMe-Strom available to run this GpuJoin? */
	if (pds_src && pds_src->kds.format == KDS_FORMAT_BLOCK)
	{
		Assert(gjs->gts.nvme_sstate != NULL);
		pgjoin->with_nvme_strom = (pds_src->nblocks_uncached > 0);
	}

	pgjoin->kern.kresults_1_offset = 0xe7e7e7e7;	/* to be set later */
	pgjoin->kern.kresults_2_offset = 0x7e7e7e7e;	/* to be set later */
	pgjoin->kern.num_rels = gjs->num_rels;
	pgjoin->kern.nitems_filtered = 0;

	/* setup of kern_parambuf */
	/* NOTE: KERN_GPUJOIN_PARAMBUF() depends on pgjoin->kern.num_rels */
	pgjoin->kern.kparams_offset
		= STROMALIGN(offsetof(kern_gpujoin, jscale[gjs->num_rels + 1]));
	kparams = KERN_GPUJOIN_PARAMBUF(&pgjoin->kern);
	memcpy(KERN_GPUJOIN_PARAMBUF(&pgjoin->kern),
		   gjs->gts.kern_params,
		   gjs->gts.kern_params->length);

	/*
	 * Assignment of the virtual partition window size to control the number
	 * of joined results, to avoid overflow of destination buffer.
	 * If a valid jscale_old is supplied, it means this task shall be
	 * re-enqueued because of smaller buffer than actual necessity.
	 */
	for (i = gjs->num_rels; i >= 0; i--)
	{
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		cl_uint				nitems;

		if (i == 0)
			nitems = (!pgjoin->pds_src ? 0 : pgjoin->pds_src->kds.nitems);
		else
			nitems = pmrels->inner_chunks[i-1]->kds.nitems;

		if (!jscale_old)
		{
			jscale[i].window_base = 0;
			jscale[i].window_size = nitems;
			jscale[i].window_orig = jscale[i].window_base;
		}
		else if (!jscale_rewind &&
				 jscale_old[i].window_base +
				 jscale_old[i].window_size < nitems)
		{
			jscale[i].window_base = (jscale_old[i].window_base +
									 jscale_old[i].window_size);
			jscale[i].window_size = (jscale_old[i].window_base +
									 jscale_old[i].window_size -
									 jscale_old[i].window_orig);
			jscale[i].window_orig = jscale[i].window_base;

			if (jscale[i].window_base +
				jscale[i].window_size > nitems)
				jscale[i].window_size = nitems - jscale[i].window_base;

			for (j = i + 1; j <= gjs->num_rels; j++)
			{
				jscale[j].window_base = 0;
				jscale[j].window_orig = jscale[j].window_base;
			}
			jscale_rewind = true;
		}
		else
		{
			/* keeps the previous partition size */
			jscale[i].window_base = jscale_old[i].window_base;
			jscale[i].window_size = jscale_old[i].window_size;
			jscale[i].window_orig = jscale[i].window_base;
		}
	}
	Assert(!jscale_old || jscale_rewind);

	/*
	 * Estimation of the number of join result items for each depth
	 */
major_retry:
	target_depth = 0;
	length = 0;
	ntuples = 0.0;
	ntuples_delta = 0.0;
	max_items = 0;

	/*
	 * Find out the largest distributed depth (if run-time statistics
	 * exists), or depth with largest delta elsewhere, for window-size
	 * reduction in the later stage.
	 * It might be a bit paranoia, however, all the score needs to be
	 * compared atomically.
	 */
	SpinLockAcquire(&rt_stat->lock);
	if (rt_stat->row_dist_score_valid)
	{
		target_row_dist_score = rt_stat->row_dist_score[0];
		for (depth=1; depth < gjs->num_rels; depth++)
		{
			if (target_row_dist_score < rt_stat->row_dist_score[depth])
			{
				target_row_dist_score = rt_stat->row_dist_score[depth];
				target_depth = depth;
			}
		}
	}
	else
	{
		/* cannot determine by the runtime-stat, so use delta of ntuples */
		target_row_dist_score = -1.0;
	}
	SpinLockRelease(&rt_stat->lock);

	for (depth = 0;
		 depth <= gjs->num_rels;
		 depth++, ntuples = ntuples_next)
	{
		Size		max_items_temp;

	minor_retry:
		ntuples_next = gpujoin_exec_estimate_nitems(gjs,
													pgjoin,
													jscale_old,
													ntuples,
													depth);

		/* check expected length of the kern_gpujoin head */
		max_items_temp = (Size)((double)(depth+1) *
								ntuples_next *
								pgstrom_chunk_size_margin);
		length = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern) +
			STROMALIGN(offsetof(kern_resultbuf, results[max_items_temp])) +
			STROMALIGN(offsetof(kern_resultbuf, results[max_items_temp]));

		/* split inner window if too large */
		if (length > 2 * pgstrom_chunk_size())
		{
			static int __count = 0;

			pgjoin->kern.jscale[target_depth].window_size
				/= (length / (2 * pgstrom_chunk_size())) + 1;
			if (pgjoin->kern.jscale[depth].window_size < 1)
				elog(ERROR, "Too much growth of result rows");
			if (depth == target_depth)
				goto minor_retry;
			if (__count++ > 10000)
				((char *)NULL)[3] = 'a';	// SEGV
			goto major_retry;
		}
		max_items = Max(max_items, max_items_temp);

		/*
		 * Determine the target depth by delta of ntuples if run-time
		 * statistics are not available.
		 */
		if (target_row_dist_score < 0 &&
			depth > 0 &&
			(depth == 1 || ntuples_next - ntuples > ntuples_delta))
		{
			ntuples_delta = Max(ntuples_next - ntuples, 0.0);
			target_depth = depth;
		}
		ntuples = ntuples_next;
	}

	/*
	 * Minimum guarantee of the kern_gpujoin buffer.
	 *
	 * NOTE: we usually have large volatility when GpuJoin tries to filter
	 * many rows, especially row selectivity is less than 1-5%, then it leads
	 * unpreferable retry of GpuJoin tasks,
	 * Unless it does not exceeds several megabytes, larger kern_resultbuf
	 * buffer is usually harmless.
	 */
	if (length < pgstrom_chunk_size() / 4)
	{
		Size	max_items_temp
			= (pgstrom_chunk_size() / 4
			   - KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern)
			   - STROMALIGN(offsetof(kern_resultbuf, results[0]))
			   - STROMALIGN(offsetof(kern_resultbuf, results[0])));
		Assert(max_items_temp >= max_items);
		length = pgstrom_chunk_size() / 4;
		max_items = max_items_temp;
	}

	/*
	 * Calculation of the destination buffer length.
	 * If expected ntuples was larger than limitation of chunk size, we
	 * have to reduce inner window size and estimate the join results.
	 * At that time, gpujoin_attach_result_buffer reduce inner_size based
	 * on the espected buffer length.
	 */
	pgjoin->pds_dst = gpujoin_attach_result_buffer(gjs, pgjoin, ntuples,
												   target_depth);
	if (!pgjoin->pds_dst)
		goto major_retry;

	/* offset of kern_resultbuf */
	pgjoin->kern.kresults_1_offset = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern);
	pgjoin->kern.kresults_2_offset = pgjoin->kern.kresults_1_offset +
		STROMALIGN(offsetof(kern_resultbuf, results[max_items]));
	pgjoin->kern.kresults_max_items = max_items;
	pgjoin->kern.num_rels = gjs->num_rels;

	return &pgjoin->task;
}


static GpuTask_v2 *
gpujoin_next_task(GpuTaskState_v2 *gts)
{
	GpuJoinState   *gjs = (GpuJoinState *) gts;
	pgstrom_data_store *pds = NULL;
	int				filedesc = -1;

	/*
	 * Logic to fetch inner multi-relations looks like nested-loop.
	 * If all the underlying inner scan already scaned its outer
	 * relation, current depth makes advance its scan pointer with
	 * reset of underlying scan pointer, or returns NULL if it is
	 * already reached end of scan.
	 */
	do {
		if (gjs->outer_scan_done || !gjs->curr_pmrels)
		{
			pgstrom_multirels *pmrels_new;

			/*
			 * NOTE: gpujoin_inner_getnext() has to be called prior to
			 * multirels_detach_buffer() because some inner chunk (PDS)
			 * may be reused on the next loop, thus, refcnt of the PDS
			 * should not be touched to zero.
			 */
			pmrels_new = gpujoin_inner_getnext(gjs);
			if (gjs->curr_pmrels)
			{
				Assert(gjs->outer_scan_done);
				multirels_detach_buffer(gjs, gjs->curr_pmrels, true);
				gjs->curr_pmrels = NULL;
			}

			/*
			 * NOTE: Neither inner nor outer relation has rows to be
			 * read any more, so we break the GpuJoin.
			 */
			if (!pmrels_new)
				return NULL;

			gjs->curr_pmrels = pmrels_new;

			/*
			 * Rewind the outer scan pointer,
			 * if it is not the first time
			 */
			if (gjs->outer_scan_done)
			{
				if (gjs->gts.css.ss.ss_currentRelation)
					gpuscanRewindScanChunk(&gjs->gts);
				else
					ExecReScan(outerPlanState(gjs));
				gjs->outer_scan_done = false;
			}
		}

		if (gjs->gts.css.ss.ss_currentRelation)
		{
			/* Scan and load the outer relation by itself */
			pds = gpuscanExecScanChunk(gts, &filedesc);
			if (!pds)
				gjs->outer_scan_done = true;
		}
		else
		{
			PlanState  *outer_node = outerPlanState(gjs);
			TupleDesc	tupdesc = ExecGetResultType(outer_node);

			while (true)
			{
				TupleTableSlot *slot;

				if (gjs->gts.scan_overflow)
				{
					slot = gjs->gts.scan_overflow;
					gjs->gts.scan_overflow = NULL;
				}
				else
				{
					slot = ExecProcNode(outer_node);
					if (TupIsNull(slot))
					{
						gjs->outer_scan_done = true;
						break;
					}
				}

				/* create a new data-store if not constructed yet */
				if (!pds)
				{
					pds = PDS_create_row(gjs->gts.gcontext,
										 tupdesc,
										 pgstrom_chunk_size());
				}

				/* insert the tuple on the data-store */
				if (!PDS_insert_tuple(pds, slot))
				{
					gjs->gts.scan_overflow = slot;
					break;
				}
			}
		}

		/*
		 * We also need to check existence of next inner hash-chunks,
		 * even if here is no more outer records, In case of multi-relations
		 * splited-out, we have to rewind the outer relation scan, then
		 * makes relations join with the next inner hash chunks.
		 */
	} while (!pds);

	return gpujoin_create_task(gjs, gjs->curr_pmrels, pds, filedesc, NULL);
}

/*
 * gpujoin_ready_task - callback when a pgstrom_gpujoin task gets processed
 * on the GPU server process then returned to the backend process again.
 */
static void
gpujoin_ready_task(GpuTaskState_v2 *gts, GpuTask_v2 *gtask)
{
	GpuJoinState	   *gjs = (GpuJoinState *)gts;
	pgstrom_gpujoin	   *pgjoin = (pgstrom_gpujoin *)gtask;
	pgstrom_gpujoin	   *pgjoin_new;
	pgstrom_multirels  *pmrels = pgjoin->pmrels;
	int					i, num_rels = gjs->num_rels;

	if (pgjoin->task.kerror.errcode != StromError_Success)
		elog(ERROR, "GpuJoin kernel internal error: %s",
			 errorTextKernel(&gtask->kerror));

	/*
	 * Enqueue another GpuJoin taks if completed one run on a part of
	 * inner window, and we still have another window to be executed.
	 * gpujoin_create_task() expects inner_base[] points the base offset
	 * of next task, and inner_size[] shall be adjusted according to the
	 * size of result buffer and chunk size limitation.
	 * (The new inner_size[] shall become baseline of the next inner scale)
	 */
	for (i = num_rels; i >= 0; i--)
	{
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		cl_uint				nitems;

		if (i == 0)
			nitems = (!pgjoin->pds_src ? 0 : pgjoin->pds_src->kds.nitems);
		else
		{
			pgstrom_data_store *pds = pmrels->inner_chunks[i-1];
			nitems = pds->kds.nitems;
		}

		if (jscale[i].window_base + jscale[i].window_size < nitems)
		{
			/*
			 * NOTE: consideration to a corner case - If CpuReCheck
			 * error was returned on JOIN_RIGHT/FULL processing, we
			 * cannot continue asynchronous task execution no longer,
			 * because outer-join-map may be updated during execution
			 * of the last task (with no valid outer PDS/KDS).
			 * For example, if depth=2 and depth=4 is RIGHT JOIN,
			 * depth=2 will produce half-NULL'ed tuples according to
			 * the outer-join-map. Thie tuple shall be processed in
			 * the depth=3 and later, according to INNER JOIN manner.
			 * It may add new match on the depth=4, then it updates
			 * the outer-join-map.
			 * If a particular portion of RIGHT JOIN are executed on
			 * both of CPU and GPU concurrently, we cannot guarantee
			 * the outer-join-map is consistent.
			 * Thus, once a pgstrom_gpujoin task got CpuReCheck error,
			 * we will process remaining RIGHT JOIN stuff on CPU
			 * entirely.
			 */
			if (!pgjoin->pds_src && pgjoin->task.cpu_fallback)
			{
				for (i=0; i < num_rels; i++)
				{
					pgstrom_data_store *pds = pmrels->inner_chunks[i];

					jscale[i+1].window_size = (pds->kds.nitems -
											   jscale[i+1].window_base);
				}
				break;
			}

			/*
			 * Instead of retain and release of PDS, we simply copy its
			 * pointer to reduce waste of spinlocks.
			 * Also note that the inner buffer (pmrels) shall be retained
			 * in the gpujoin_create_task(), so no need to do something
			 * special.
			 */
			pgjoin_new = (pgstrom_gpujoin *)
				gpujoin_create_task(gjs,
									pgjoin->pmrels,
									pgjoin->pds_src,
									-1,
									jscale);
			pgjoin->pds_src = NULL;
			gpuservSendGpuTask(gjs->gts.gcontext, &pgjoin_new->task);
			break;
		}
		Assert(jscale[i].window_base + jscale[i].window_size == nitems);
	}
}

/*
 * gpujoin_switch_task - callback when a pgstrom_gpujoin task gets completed
 * and assigned on the gts->curr_task.
 */
static void
gpujoin_switch_task(GpuTaskState_v2 *gts, GpuTask_v2 *gtask)
{
	GpuJoinState	   *gjs = (GpuJoinState *) gts;
	pgstrom_gpujoin	   *pgjoin = (pgstrom_gpujoin *) gtask;
	int					i;

	/* rewind the CPU fallback position */
	if (pgjoin->task.cpu_fallback)
	{
		gjs->fallback_outer_index = -1;
		for (i=0; i < gjs->num_rels; i++)
		{
			gjs->inners[i].fallback_inner_index = -1;
			gjs->inners[i].fallback_right_outer = false;
		}
		ExecStoreAllNullTuple(gjs->slot_fallback);
	}
	else
	{
		/*
		 * We don't need to have the inner pmrels buffer no longer, if GPU
		 * task gets successfully done.
		 */
		multirels_detach_buffer(gjs, pgjoin->pmrels, true);
		pgjoin->pmrels = NULL;
	}
}

static TupleTableSlot *
gpujoin_next_tuple(GpuTaskState_v2 *gts)
{
	GpuJoinState	   *gjs = (GpuJoinState *) gts;
	TupleTableSlot	   *slot = gjs->gts.css.ss.ss_ScanTupleSlot;
	pgstrom_gpujoin	   *pgjoin = (pgstrom_gpujoin *)gjs->gts.curr_task;
	pgstrom_data_store *pds_dst = pgjoin->pds_dst;

	if (pgjoin->task.cpu_fallback)
	{
		/*
		 * MEMO: We may reuse tts_values[]/tts_isnull[] of the previous
		 * tuple, to avoid same part of tuple extraction. For example,
		 * portion by depth < 2 will not be changed during iteration in
		 * depth == 3. You may need to pay attention on the core changes
		 * in the future version.
		 */
		slot = gpujoin_next_tuple_fallback(gjs, pgjoin);
	}
	else
	{
		/* fetch a result tuple */
		ExecClearTuple(slot);
		if (!PDS_fetch_tuple(slot, pds_dst, &gjs->gts))
			slot = NULL;
	}
#if 0
	/*
	 * MEMO: If GpuJoin generates a corrupted tuple, it may lead crash on
	 * the upper level of plan node. Even if we got a crash dump, it is not
	 * easy to analyze corrupted tuple later. ExecMaterializeSlot() can
	 * cause crash in proper level, and it will assist bug fixes.
	 */
	if (slot != NULL)
		(void) ExecMaterializeSlot(slot);
#endif
	return slot;
}

/* ----------------------------------------------------------------
 *
 * Routines for CPU fallback, if kernel code returned CpuReCheck
 * error code.
 *
 * ----------------------------------------------------------------
 */
static void
gpujoin_fallback_tuple_extract(TupleTableSlot *slot_fallback,
							   TupleDesc tupdesc, Oid table_oid,
							   kern_tupitem *tupitem,
							   AttrNumber *tuple_dst_resno,
							   AttrNumber src_anum_min,
							   AttrNumber src_anum_max)
{
	HeapTupleHeader	htup;
	bool		hasnulls;
	AttrNumber	fallback_nattrs __attribute__ ((unused));
	Datum	   *tts_values = slot_fallback->tts_values;
	bool	   *tts_isnull = slot_fallback->tts_isnull;
	char	   *tp;
	long		off;
	int			i, nattrs;
	AttrNumber	resnum;

	Assert(src_anum_min > FirstLowInvalidHeapAttributeNumber);
	Assert(src_anum_max <= tupdesc->natts);
	fallback_nattrs = slot_fallback->tts_tupleDescriptor->natts;

	/*
	 * Fill up the destination by NULL, if no tuple was supplied.
	 */
	if (!tupitem)
	{
		for (i = src_anum_min; i <= src_anum_max; i++)
		{
			resnum = tuple_dst_resno[i-FirstLowInvalidHeapAttributeNumber-1];
			if (resnum)
			{
				Assert(resnum > 0 && resnum <= fallback_nattrs);
				tts_values[resnum - 1] = (Datum) 0;
				tts_isnull[resnum - 1] = true;
			}
		}
		return;
	}

	htup = &tupitem->htup;
	hasnulls = ((htup->t_infomask & HEAP_HASNULL) != 0);

	/*
	 * Extract system columns if any
	 */
	if (src_anum_min < 0)
	{
		/* ctid */
		resnum = tuple_dst_resno[SelfItemPointerAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1] = PointerGetDatum(&tupitem->t_self);
			tts_isnull[resnum - 1] = false;
		}

		/* cmax */
		resnum = tuple_dst_resno[MaxCommandIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* xmax */
		resnum = tuple_dst_resno[MaxTransactionIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= TransactionIdGetDatum(HeapTupleHeaderGetRawXmax(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* cmin */
		resnum = tuple_dst_resno[MinCommandIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* xmin */
		resnum = tuple_dst_resno[MinTransactionIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= TransactionIdGetDatum(HeapTupleHeaderGetRawXmin(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* oid */
		resnum = tuple_dst_resno[ObjectIdAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1]
				= ObjectIdGetDatum(HeapTupleHeaderGetOid(htup));
			tts_isnull[resnum - 1] = false;
		}

		/* tableoid */
		resnum = tuple_dst_resno[TableOidAttributeNumber -
								 FirstLowInvalidHeapAttributeNumber - 1];
		if (resnum)
		{
			Assert(resnum > 0 && resnum <= fallback_nattrs);
			tts_values[resnum - 1] = ObjectIdGetDatum(table_oid);
			tts_isnull[resnum - 1] = false;
		}
	}

	/*
	 * Extract user defined columns, according to the logic in
	 * heap_deform_tuple(), but implemented by ourselves for performance.
	 */
	nattrs = HeapTupleHeaderGetNatts(htup);
	nattrs = Min3(nattrs, tupdesc->natts, src_anum_max);

	tp = (char *) htup + htup->t_hoff;
	off = 0;
	for (i=0; i < nattrs; i++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];

		resnum = tuple_dst_resno[i - FirstLowInvalidHeapAttributeNumber];
		if (hasnulls && att_isnull(i, htup->t_bits))
		{
			if (resnum > 0)
			{
				Assert(resnum <= fallback_nattrs);
				tts_values[resnum - 1] = (Datum) 0;
				tts_isnull[resnum - 1] = true;
			}
			continue;
		}

		/* elsewhere field is not null */
		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_isnull[resnum - 1] = false;
		}

		if (attr->attlen == -1)
			off = att_align_pointer(off, attr->attalign, -1, tp + off);
		else
			off = att_align_nominal(off, attr->attalign);

		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_values[resnum - 1] = fetchatt(attr, tp + off);
		}
		off = att_addlength_pointer(off, attr->attlen, tp + off);
	}

	/*
     * If tuple doesn't have all the atts indicated by src_anum_max,
	 * read the rest as null
	 */
	for (; i < src_anum_max; i++)
	{
		resnum = tuple_dst_resno[i - FirstLowInvalidHeapAttributeNumber];
		if (resnum > 0)
		{
			Assert(resnum <= fallback_nattrs);
			tts_values[resnum - 1] = (Datum) 0;
			tts_isnull[resnum - 1] = true;
		}
	}
}

static bool
gpujoin_fallback_inner_recurse(GpuJoinState *gjs,
							   TupleTableSlot *slot_fallback,
							   pgstrom_gpujoin *pgjoin,
							   int depth,
							   cl_bool do_right_outer_join)
{
	ExprContext		   *econtext = gjs->gts.css.ss.ps.ps_ExprContext;
	pgstrom_multirels  *pmrels = pgjoin->pmrels;
	innerState		   *istate = &gjs->inners[depth-1];
	TupleTableSlot	   *slot_in = istate->state->ps_ResultTupleSlot;
	TupleDesc			tupdesc = slot_in->tts_tupleDescriptor;
	kern_data_store	   *kds_in;
	kern_join_scale	   *jscale;
	bool				reload_inner_next;

	Assert(depth > 0 && depth <= gjs->num_rels);
	kds_in = &pmrels->inner_chunks[depth-1]->kds;
	jscale = &pgjoin->kern.jscale[depth];

	reload_inner_next = (istate->fallback_inner_index < 0 ||
						 depth == gjs->num_rels);
	for (;;)
	{
		cl_uint			i, kds_index;
		cl_uint			nvalids;

		if (reload_inner_next)
		{
			kern_tupitem   *tupitem = NULL;
			kern_hashitem  *khitem;

			ResetExprContext(econtext);

			if (do_right_outer_join)
			{
				/* already reached end of the inner relation */
				if (istate->fallback_inner_index == UINT_MAX)
					return false;

				kds_index = Max(jscale->window_orig,
								istate->fallback_inner_index + 1);
				if (istate->join_type == JOIN_RIGHT ||
					istate->join_type == JOIN_FULL)
				{
					cl_bool	   *host_ojmap = pmrels->h_ojmaps;

					Assert(host_ojmap != NULL);
					host_ojmap += pmrels->kern.chunks[depth-1].ojmap_offset;
					nvalids = Min(kds_in->nitems,
								  jscale->window_base + jscale->window_size);
					/*
					 * Make half-null tuples according to the outer join map,
					 * then kick inner join on the later depth.
					 * Once we reached end of the OJMap, walk down into the
					 * deeper depth.
					 */
					while (kds_index < nvalids)
					{
						if (!host_ojmap[kds_index])
						{
							ExecStoreAllNullTuple(slot_fallback);

							tupitem = KERN_DATA_STORE_TUPITEM(kds_in,
															  kds_index);
							istate->fallback_inner_index = kds_index;
							goto inner_fillup;
						}
						kds_index++;
					}
				}
				/* no need to walk down into deeper depth */
				if (depth == gjs->num_rels)
					return false;

				tupitem = NULL;
				istate->fallback_inner_index = UINT_MAX;
				istate->fallback_right_outer = true;
			}
			else if (!istate->hash_outer_keys)
			{
				/*
				 * Case of GpuNestLoop
				 */
				kds_index = Max(jscale->window_orig,
								istate->fallback_inner_index + 1);
				nvalids = Min(kds_in->nitems,
							  jscale->window_base + jscale->window_size);
				if (kds_index >= nvalids)
					return false;	/* end of inner/left join */
				tupitem = KERN_DATA_STORE_TUPITEM(kds_in, kds_index);
				istate->fallback_inner_index = kds_index;
				istate->fallback_inner_matched = false;
			}
			else if (istate->fallback_inner_index < 0)
			{
				/*
				 * Case of GpuHashJoin (first item)
				 */
				cl_uint		hash;
				bool		is_null_keys;

				hash = get_tuple_hashvalue(istate,
										   false,
										   slot_fallback,
										   &is_null_keys);
				/* all-NULL keys will never match to inner rows */
				if (is_null_keys)
				{
					if (istate->join_type == JOIN_LEFT ||
						istate->join_type == JOIN_FULL)
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}

				/* Is the hash-value in range of the kds_in? */
				if (hash < kds_in->hash_min || hash > kds_in->hash_max)
					return false;

				khitem = KERN_HASH_FIRST_ITEM(kds_in, hash);
				if (!khitem)
				{
					if (istate->join_type == JOIN_LEFT ||
						istate->join_type == JOIN_FULL)
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}
				kds_index = khitem->rowid;
				istate->fallback_inner_hash = hash;
				istate->fallback_inner_index = kds_index;
				istate->fallback_inner_matched = false;

				/* khitem is not visible if rowid is out of window range */
				if (khitem->rowid < jscale->window_base ||
					khitem->rowid >= jscale->window_base + jscale->window_size)
					continue;

				/* quick check whether khitem shall match */
				if (khitem->hash != istate->fallback_inner_hash)
					continue;

				tupitem = &khitem->t;
			}
			else if (istate->fallback_inner_index < UINT_MAX)
			{
				/*
				 * Case of GpuHashJoin (second or later item)
				 */
				kds_index = istate->fallback_inner_index;
				khitem = KERN_DATA_STORE_HASHITEM(kds_in, kds_index);
				Assert(khitem != NULL);
				khitem = KERN_HASH_NEXT_ITEM(kds_in, khitem);
				if (!khitem)
				{
					if (!istate->fallback_inner_matched &&
						(istate->join_type == JOIN_LEFT ||
						 istate->join_type == JOIN_FULL))
					{
						istate->fallback_inner_index = UINT_MAX;
						tupitem = NULL;
						goto inner_fillup;
					}
					return false;
				}
				kds_index = khitem->rowid;
				istate->fallback_inner_index = kds_index;

				/* khitem is not visible if rowid is out of window range */
				if (khitem->rowid < jscale->window_orig ||
					khitem->rowid >= jscale->window_base + jscale->window_size)
					continue;

				/* quick check whether khitem shall match */
				if (khitem->hash != istate->fallback_inner_hash)
					continue;

				tupitem = &khitem->t;
			}
			else
			{
				/*
				 * A dummy fallback_inner_index shall be set when a half-NULLs
				 * tuple is constructed on LEFT/FULL OUTER JOIN. It means this
				 * depth has no more capable to fetch next joined rows.
				 */
				Assert(istate->join_type == JOIN_LEFT ||
					   istate->join_type == JOIN_FULL);
				return false;
			}

			/*
			 * Extract inner columns to the slot_fallback
			 */
		inner_fillup:
			gpujoin_fallback_tuple_extract(slot_fallback,
										   tupdesc,
										   kds_in->table_oid,
										   tupitem,
										   istate->inner_dst_resno,
										   istate->inner_src_anum_min,
										   istate->inner_src_anum_max);
			/*
			 * Evaluation of the join_quals, if inner matched
			 */
			if (tupitem && !do_right_outer_join)
			{
				if (!ExecQual(istate->join_quals, econtext, false))
					continue;

				/* No RJ/FJ tuple is needed for this inner item */
				if (istate->join_type == JOIN_RIGHT ||
					istate->join_type == JOIN_FULL)
				{
					cl_bool	   *host_ojmaps = pmrels->h_ojmaps;

					Assert(host_ojmaps != NULL);
					host_ojmaps += pmrels->kern.chunks[depth-1].ojmap_offset;

					Assert(kds_index >= 0 && kds_index < kds_in->nitems);
					host_ojmaps[kds_index] = true;
				}
				/* No LJ/FJ tuple is needed for this outer item */
				istate->fallback_inner_matched = true;
			}

			/*
			 * Evaluation of the other_quals, if any
			 */
			if (!ExecQual(istate->other_quals, econtext, false))
				continue;

			/* Rewind the position of deeper levels */
			for (i = depth; i < gjs->num_rels; i++)
			{
				gjs->inners[i].fallback_inner_index = -1;
				gjs->inners[i].fallback_right_outer = false;
			}
		}

		/*
		 * Walk down into the next depth, if we have deeper level any more.
		 * If no more rows in deeper level, rewind them and try to pick up
		 * next tuple in this level.
		 */
		if (depth < gjs->num_rels &&
			!gpujoin_fallback_inner_recurse(gjs, slot_fallback,
											pgjoin, depth + 1,
											istate->fallback_right_outer))
		{
			reload_inner_next = true;
			continue;
		}
		break;
	}
	return true;
}

static TupleTableSlot *
gpujoin_next_tuple_fallback(GpuJoinState *gjs, pgstrom_gpujoin *pgjoin)
{
	ExprContext		   *econtext = gjs->gts.css.ss.ps.ps_ExprContext;
	TupleDesc			tupdesc;
	ExprDoneCond		is_done;

	/* tuple descriptor of the outer relation */
	if (gjs->gts.css.ss.ss_currentRelation)
		tupdesc = RelationGetDescr(gjs->gts.css.ss.ss_currentRelation);
	else
		tupdesc = outerPlanState(gjs)->ps_ResultTupleSlot->tts_tupleDescriptor;

	/*
	 * tuple-table-slot to be constructed by CPU fallback.
	 *
	 * MEMO: For performance benefit, we reuse the contents of tts_values
	 * and tts_isnull unless its source tuple is not reloaded. The prior
	 * execution may create slot_fallback->tts_tuple based on the old values,
	 * so we have to clear it for each iteration. ExecClearTuple() also set
	 * zero on tts_nvalid, not only release of tts_tuple, so we enlarge
	 * 'tts_nvalid' by ExecStoreVirtualTuple(); which does not touch values
	 * of tts_values/tts_isnull.
	 */
	Assert(gjs->slot_fallback != NULL);
	ExecClearTuple(gjs->slot_fallback);
	ExecStoreVirtualTuple(gjs->slot_fallback);

	if (pgjoin->pds_src)
	{
		kern_data_store	   *kds_src = &pgjoin->pds_src->kds;
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		kern_tupitem	   *tupitem;
		bool				reload_outer_next;

		reload_outer_next = (gjs->fallback_outer_index < 0);
		for (;;)
		{
			econtext->ecxt_scantuple = gjs->slot_fallback;
			ResetExprContext(econtext);

			if (reload_outer_next)
			{
				cl_uint		i, kds_index;
				cl_uint		nvalids;

				kds_index = Max(jscale->window_orig,
								gjs->fallback_outer_index + 1);
				/* Do we still have any other rows more? */
				nvalids = Min(kds_src->nitems,
							  jscale->window_base + jscale->window_size);
				if (kds_index >= nvalids)
				{
					/*
					 * NOTE: detach of the inner pmrels buffer was postponed
					 * to the point of CPU fallback end, if needed. So, we
					 * have to detach here.
					 */
					multirels_detach_buffer(gjs, pgjoin->pmrels, true);
					pgjoin->pmrels = NULL;
					return NULL;
				}
				gjs->fallback_outer_index = kds_index;

				/* Fills up fields of the fallback_slot with outer columns */
				tupitem = KERN_DATA_STORE_TUPITEM(kds_src, kds_index);
				gpujoin_fallback_tuple_extract(gjs->slot_fallback,
											   tupdesc,
											   kds_src->table_oid,
											   tupitem,
											   gjs->outer_dst_resno,
											   gjs->outer_src_anum_min,
											   gjs->outer_src_anum_max);
				/* evaluation of the outer qual if any */
				if (!ExecQual(gjs->outer_quals, econtext, false))
					continue;
				/* ok, rewind the deeper levels prior to walk down */
				for (i=0; i < gjs->num_rels; i++)
				{
					gjs->inners[i].fallback_inner_index = -1;
					gjs->inners[i].fallback_right_outer = false;
				}
			}

			/* walk down to the deeper depth */
			if (!gpujoin_fallback_inner_recurse(gjs, gjs->slot_fallback,
												pgjoin, 1, false))
			{
				reload_outer_next = true;
				continue;
			}
			break;
		}
	}
	else
	{
		/*
		 * pds_src == NULL means the final chunk of RIGHT/FULL OUTER JOIN.
		 * We have to fill up outer columns with NULLs, then walk down into
		 * the inner depths.
		 */
		econtext->ecxt_scantuple = gjs->slot_fallback;
		ResetExprContext(econtext);

		if (gjs->fallback_outer_index < 0)
		{
			gpujoin_fallback_tuple_extract(gjs->slot_fallback,
										   tupdesc,
										   InvalidOid,
										   NULL,
										   gjs->outer_dst_resno,
										   gjs->outer_src_anum_min,
										   gjs->outer_src_anum_max);
			gjs->fallback_outer_index = 0;
			/* XXX - Do we need to rewind inners? Likely, No */
			/* gpujoin_switch_task() should rewind them already */
		}
		/* walk down into the deeper depth */
		if (!gpujoin_fallback_inner_recurse(gjs, gjs->slot_fallback,
											pgjoin, 1, true))
			return NULL;
	}

	Assert(!TupIsNull(gjs->slot_fallback));
	if (gjs->proj_fallback)
		return ExecProject(gjs->proj_fallback, &is_done);

	return gjs->slot_fallback;	/* no projection is needed? */
}

/* ----------------------------------------------------------------
 *
 * GpuTask handlers of GpuJoin
 *
 * ----------------------------------------------------------------
 */
static void
gpujoin_cleanup_cuda_resources(pgstrom_gpujoin *pgjoin)
{
	if (pgjoin->with_nvme_strom && pgjoin->m_kds_src)
		gpuMemFreeIOMap(pgjoin->task.gcontext, pgjoin->m_kds_src);
	if (pgjoin->m_kgjoin)
		gpuMemFree(pgjoin->task.gcontext, pgjoin->m_kgjoin);
	if (pgjoin->m_kmrels)
		multirels_put_buffer(pgjoin);

	/* clear the pointers */
	pgjoin->kern_main = NULL;
	pgjoin->m_kgjoin = 0UL;
	pgjoin->m_kds_src = 0UL;
	pgjoin->m_kds_dst = 0UL;
	pgjoin->m_kmrels = 0UL;
}

void
gpujoin_release_task(GpuTask_v2 *gtask)
{
	pgstrom_gpujoin	   *pgjoin = (pgstrom_gpujoin *) gtask;

	/* release all the cuda resources, if any */
	gpujoin_cleanup_cuda_resources(pgjoin);
	/* detach multi-relations buffer, if any */
	if (pgjoin->pmrels)
		multirels_detach_buffer((GpuJoinState *)gtask->gts,
								pgjoin->pmrels, false);
	/* unlink source data store */
	if (pgjoin->pds_src)
		PDS_release(pgjoin->pds_src);
	/* unlink destination data store */
	if (pgjoin->pds_dst)
		PDS_release(pgjoin->pds_dst);
	/* release this gpu-task itself */
	dmaBufferFree(pgjoin);
}

int
gpujoin_complete_task(GpuTask_v2 *gtask)
{
	pgstrom_gpujoin	   *pgjoin = (pgstrom_gpujoin *) gtask;
	pgstrom_multirels  *pmrels = pgjoin->pmrels;

	if (pgjoin->task.kerror.errcode == StromError_Success)
	{
		pgstrom_data_store *pds_dst = pgjoin->pds_dst;
		runtimeStat		   *rt_stat = pgjoin->rt_stat;
		kern_join_scale	   *jscale = pgjoin->kern.jscale;
		cl_int				i, num_rels = pmrels->kern.nrels;

		/*
		 * Update run-time statistics information according to the number
		 * of rows actually processed by this GpuJoin task.
		 * In case of OUTER JOIN task, we don't count source items because
		 * it is generated as result of unmatched tuples.
		 */
		SpinLockAcquire(&rt_stat->lock);
		rt_stat->source_ntasks++;
		rt_stat->source_nitems += (jscale[0].window_base +
								   jscale[0].window_size -
								   jscale[0].window_orig);

		for (i=0; i <= num_rels; i++)
		{
			rt_stat->inner_nitems[i] += jscale[i].inner_nitems;
			rt_stat->right_nitems[i] += jscale[i].right_nitems;
			if (jscale[i].row_dist_score > 0.0)
			{
				rt_stat->row_dist_score_valid = true;
				rt_stat->row_dist_score[i] += jscale[i].row_dist_score;
			}
		}
		rt_stat->results_nitems += pds_dst->kds.nitems;
		rt_stat->results_usage += pds_dst->kds.usage;
		SpinLockRelease(&rt_stat->lock);

		/*
		 * In case of CPU fallback, we have to move the entire outer-
		 * join map into the host side, prior to fallback execution.
		 */
		if (!pgjoin->pds_src && pgjoin->task.cpu_fallback)
			colocate_outer_join_maps_to_host(pgjoin->pmrels);
	}

	/*
	 * Release device memory and event objects acquired by the task.
	 * For the better reuse of the inner multirels buffer, it has to
	 * be after the above re-enqueue in case of retry.
	 */
	gpujoin_cleanup_cuda_resources(pgjoin);

	return 0;
}

static void
gpujoin_task_respond(CUstream stream, CUresult status, void *private)
{
	pgstrom_gpujoin	   *pgjoin = private;
	bool				is_urgent;

	/* OK, routine is called back in the usual context */
	if (status == CUDA_SUCCESS)
	{
		pgjoin->task.kerror = pgjoin->kern.kerror;

		/* Takes CPU fallback instead of the CpuReCheck error */
		if (pgstrom_cpu_fallback_enabled &&
			pgjoin->task.kerror.errcode == StromError_CpuReCheck)
		{
			pgjoin->task.kerror.errcode = StromError_Success;
			pgjoin->task.cpu_fallback = true;
		}
		is_urgent = (pgjoin->task.kerror.errcode != StromError_Success);
	}
	else
	{
		if (!pgjoin->task.kerror.errcode)
		{
			pgjoin->task.kerror.errcode = status;
			pgjoin->task.kerror.kernel = StromKernel_CudaRuntime;
			pgjoin->task.kerror.lineno = 0;
		}
		is_urgent = true;
	}
	gpuservCompleteGpuTask(&pgjoin->task, is_urgent);
}

static int
__gpujoin_process_task(pgstrom_gpujoin *pgjoin,
					   CUmodule cuda_module, CUstream cuda_stream)
{
	GpuContext_v2	   *gcontext = pgjoin->task.gcontext;
	pgstrom_data_store *pds_src = pgjoin->pds_src;
	pgstrom_data_store *pds_dst = pgjoin->pds_dst;
	Size			offset;
	Size			length;
	Size			pos;
	CUresult		rc;
	void		   *kern_args[10];

	/*
	 * sanity checks
	 */
	Assert(pds_src == NULL ||
		   pds_src->kds.format == KDS_FORMAT_ROW ||
		   pds_src->kds.format == KDS_FORMAT_BLOCK);
	Assert(pds_dst->kds.format == KDS_FORMAT_ROW ||
		   pds_dst->kds.format == KDS_FORMAT_SLOT);

	/*
	 * GPU kernel function lookup
	 */
	rc = cuModuleGetFunction(&pgjoin->kern_main,
							 cuda_module,
							 "gpujoin_main");
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuModuleGetFunction: %s", errorText(rc));

	/*
	 * Allocation of device memory for each chunks
	 */
	length = pos = GPUMEMALIGN(pgjoin->kern.kresults_2_offset +
							   pgjoin->kern.kresults_2_offset -
							   pgjoin->kern.kresults_1_offset);
	if (pgjoin->with_nvme_strom)
	{
		Assert(pds_src->kds.format == KDS_FORMAT_BLOCK);
		rc = gpuMemAllocIOMap(pgjoin->task.gcontext,
							  &pgjoin->m_kds_src,
							  GPUMEMALIGN(pds_src->kds.length));
		if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		{
			PDS_fillup_blocks(pds_src, pgjoin->task.peer_fdesc);
			pgjoin->m_kds_src = 0UL;
			pgjoin->with_nvme_strom = false;
			length += GPUMEMALIGN(pds_src->kds.length);
		}
		else if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuMemAllocIOMap: %s",
				 errorText(rc));
	}
	else if (pds_src)
		length += GPUMEMALIGN(pds_src->kds.length);

	length += GPUMEMALIGN(pds_dst->kds.length);

	rc = gpuMemAlloc(gcontext, &pgjoin->m_kgjoin, length);
	if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		goto out_of_resource;
	else if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuMemAlloc: %s", errorText(rc));

	if (pds_src && pgjoin->m_kds_src == 0UL)
	{
		pgjoin->m_kds_src = pgjoin->m_kgjoin + pos;
		pos += GPUMEMALIGN(pds_src->kds.length);
	}
	pgjoin->m_kds_dst = pgjoin->m_kgjoin + pos;

	/*
	 * OK, all the device memory and kernel objects are successfully
	 * constructed. Let's enqueue DMA send/recv and kernel invocations.
	 */

	/* inner multi relations */
	if (!multirels_get_buffer(pgjoin, cuda_stream))
		goto out_of_resource;

	/* kern_gpujoin + static portion of kern_resultbuf */
	length = KERN_GPUJOIN_HEAD_LENGTH(&pgjoin->kern);
	rc = cuMemcpyHtoDAsync(pgjoin->m_kgjoin,
						   &pgjoin->kern,
						   length,
						   cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));

	if (pds_src)
	{
		/* source outer relation */
		if (!pgjoin->with_nvme_strom)
		{
			rc = cuMemcpyHtoDAsync(pgjoin->m_kds_src,
								   &pds_src->kds,
								   pds_src->kds.length,
								   cuda_stream);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
		}
		else
		{
			Assert(pds_src->kds.format == KDS_FORMAT_BLOCK);
			gpuMemCopyFromSSDAsync(&pgjoin->task,
								   pgjoin->m_kds_src,
                                   pds_src,
                                   cuda_stream);
            gpuMemCopyFromSSDWait(&pgjoin->task,
                                  cuda_stream);
		}
	}
	else
	{
		/* colocation of the outer join map */
		//HOGE: we can skip colocation if no CPU fallback happen
		colocate_outer_join_maps_to_device(pgjoin, cuda_module, cuda_stream);
	}

	/* kern_data_store (dst of head) */
	rc = cuMemcpyHtoDAsync(pgjoin->m_kds_dst,
						   &pds_dst->kds,
						   pds_dst->kds.length,
						   cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));

	/* Lunch:
	 * KERNEL_FUNCTION(void)
	 * gpujoin_main(kern_gpujoin *kgjoin,
	 *              kern_multirels *kmrels,
	 *              cl_bool *outer_join_map,
	 *              kern_data_store *kds_src,
	 *              kern_data_store *kds_dst,
	 *              cl_int cuda_index)
	 */
	kern_args[0] = &pgjoin->m_kgjoin;
	kern_args[1] = &pgjoin->m_kmrels;
	kern_args[2] = &pgjoin->m_ojmaps;
	kern_args[3] = &pgjoin->m_kds_src;
	kern_args[4] = &pgjoin->m_kds_dst;
	kern_args[5] = &gpuserv_cuda_dindex;

	rc = cuLaunchKernel(pgjoin->kern_main,
						1, 1, 1,
						1, 1, 1,
						sizeof(kern_errorbuf),
						cuda_stream,
						kern_args,
						NULL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuLaunchKernel: %s", errorText(rc));

	/* DMA Recv: kern_gpujoin *kgjoin */
	length = offsetof(kern_gpujoin, jscale[pgjoin->kern.num_rels + 1]);
	rc = cuMemcpyDtoHAsync(&pgjoin->kern,
						   pgjoin->m_kgjoin,
						   length,
						   cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "cuMemcpyDtoHAsync: %s", errorText(rc));

	/* DMA Recv: kern_data_store *kds_dst */
	rc = cuMemcpyDtoHAsync(&pds_dst->kds,
						   pgjoin->m_kds_dst,
						   pds_dst->kds.length,
						   cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "cuMemcpyDtoHAsync: %s", errorText(rc));

	/*
	 * DMA Recv: kern_data_store *kds_src, if NVMe-Strom is used and join
	 * results contains varlena/indirect datum
	 */
	if (pds_src &&
		pds_src->kds.format == KDS_FORMAT_BLOCK &&
		pds_src->nblocks_uncached > 0 &&
		pds_dst->kds.has_notbyval)
	{
		cl_uint	nr_loaded = pds_src->kds.nitems - pds_src->nblocks_uncached;

		offset = ((char *)KERN_DATA_STORE_BLOCK_PGPAGE(&pds_src->kds,
                                                       nr_loaded) -
				  (char *)&pds_src->kds);
		length = pds_src->nblocks_uncached * BLCKSZ;
		rc = cuMemcpyDtoHAsync((char *)&pds_src->kds + offset,
							   pgjoin->m_kds_src + offset,
							   length,
							   cuda_stream);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));

		/*
		 * NOTE: Once GPU-to-RAM DMA gets completed, "uncached" blocks are
		 * filled up with valid blocks, so we can clear @nblocks_uncached
		 * not to write back GPU RAM twice even if CPU fallback.
		 */
		pds_src->nblocks_uncached = 0;
	}

	/*
	 * Register the callback
	 */
	rc = cuStreamAddCallback(cuda_stream,
							 gpujoin_task_respond,
							 pgjoin, 0);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "cuStreamAddCallback: %s", errorText(rc));

	return 0;

out_of_resource:
	gpujoin_cleanup_cuda_resources(pgjoin);
	return 1;
}

int
gpujoin_process_task(GpuTask_v2 *gtask,
					 CUmodule cuda_module,
					 CUstream cuda_stream)
{
	pgstrom_gpujoin *pgjoin = (pgstrom_gpujoin *) gtask;
	int			status;

	PG_TRY();
	{
		status = __gpujoin_process_task(pgjoin, cuda_module, cuda_stream);
	}
	PG_CATCH();
	{
		gpujoin_cleanup_cuda_resources(pgjoin);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return status;
}

/* ================================================================
 *
 * Routines to preload inner relations (heap/hash)
 *
 * ================================================================
 */

/*
 * add_extra_randomness
 *
 * BUG#211 - In case when we have to split inner relations virtually,
 * extra randomness is significant to avoid singularity. In theorem,
 * rowid of KDS (assigned sequentially on insertion) is independent
 * concept from the join key. However, people usually insert tuples
 * according to the key value (referenced by join) sequentially.
 * It eventually leads unexpected results - A particular number of
 * outer rows generates unexpected number of results rows. Even if
 * CPU reduced inner_size according to the run-time statistics, retry
 * shall be repeated until the virtual inner relation boundary goes
 * across the problematic key value.
 * This extra randomness makes distribution of the join keys flatten.
 * Because rowid of KDS items are randomized, we can expect reduction
 * of inner_size[] will reduce scale of the join result as expectation
 * of statistical result.
 *
 * NOTE: we may be able to add this extra randomness only when inner_size
 * is smaller than kds->nitems and not yet randomized. However, we also
 * pay attention the case when NVRTC support dynamic parallelism then
 * GPU kernel get capability to control inner_size[] inside GPU kernel.
 */
static void
add_extra_randomness(pgstrom_data_store *pds)
{
	kern_data_store	   *kds = &pds->kds;
	cl_uint				x, y, temp;

	return;		//????

	if (pds->kds.format == KDS_FORMAT_ROW ||
		pds->kds.format == KDS_FORMAT_HASH)
	{
		cl_uint	   *row_index = KERN_DATA_STORE_ROWINDEX(kds);
		cl_uint		nitems = pds->kds.nitems;

		for (x=0; x < nitems; x++)
		{
			y = rand() % nitems;
			if (x == y)
				continue;

			if (pds->kds.format == KDS_FORMAT_HASH)
			{
				kern_hashitem  *khitem_x = KERN_DATA_STORE_HASHITEM(kds, x);
				kern_hashitem  *khitem_y = KERN_DATA_STORE_HASHITEM(kds, y);
				Assert(khitem_x->rowid == x);
				Assert(khitem_y->rowid == y);
				khitem_x->rowid = y;	/* swap */
				khitem_y->rowid = x;	/* swap */
			}
			temp = row_index[x];
			row_index[x] = row_index[y];
			row_index[y] = temp;
		}
	}
	else
		elog(ERROR, "Unexpected data chunk format: %u", kds->format);
}

/*
 * gpujoin_inner_unload - it release inner relations and its data stores.
 *
 * TODO: We like to retain a part of inner relations if it is not
 * parametalized.
 */
static void
gpujoin_inner_unload(GpuJoinState *gjs, bool needs_rescan)
{
	ListCell   *lc;
	cl_int		i;

	for (i=0; i < gjs->num_rels; i++)
	{
		innerState *istate = &gjs->inners[i];

		/*
		 * If chgParam of subnode is not null then plan will be
		 * re-scanned by next ExecProcNode.
		 */
		if (needs_rescan && istate->state->chgParam == NULL)
			ExecReScan(istate->state);
		foreach (lc, istate->pds_list)
			PDS_release((pgstrom_data_store *) lfirst(lc));
		istate->pds_list = NIL;
		istate->pds_index = 0;
		istate->pds_limit = 0;
		istate->consumed = 0;
		istate->ntuples = 0;
		istate->tupstore = NULL;
	}
	gjs->inner_preloaded = false;
}

/*
 * calculation of the hash-value
 */
static pg_crc32
get_tuple_hashvalue(innerState *istate,
					bool is_inner_hashkeys,
					TupleTableSlot *slot,
					bool *p_is_null_keys)
{
	ExprContext	   *econtext = istate->econtext;
	pg_crc32		hash;
	List		   *hash_keys_list;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	ListCell	   *lc4;
	bool			is_null_keys = true;

	if (is_inner_hashkeys)
	{
		hash_keys_list = istate->hash_inner_keys;
		econtext->ecxt_innertuple = slot;
	}
	else
	{
		hash_keys_list = istate->hash_outer_keys;
		econtext->ecxt_scantuple = slot;
	}

	/* calculation of a hash value of this entry */
	INIT_LEGACY_CRC32(hash);
	forfour (lc1, hash_keys_list,
			 lc2, istate->hash_keylen,
			 lc3, istate->hash_keybyval,
			 lc4, istate->hash_keytype)
	{
		ExprState  *clause = lfirst(lc1);
		int			keylen = lfirst_int(lc2);
		bool		keybyval = lfirst_int(lc3);
		Oid			keytype = lfirst_oid(lc4);
		Datum		value;
		bool		isnull;

		value = ExecEvalExpr(clause, istate->econtext, &isnull, NULL);
		if (isnull)
			continue;
		is_null_keys = false;	/* key is non-NULL valid */

		/* fixup host representation to special internal format. */
		if (keytype == NUMERICOID)
		{
			kern_context	dummy;
			pg_numeric_t	temp;

			/*
			 * FIXME: If NUMERIC value is out of range, we cannot execute
			 * GpuJoin in the kernel space, so needs a fallback routine.
			 */
			temp = pg_numeric_from_varlena(&dummy, (struct varlena *)
										   DatumGetPointer(value));
			COMP_LEGACY_CRC32(hash, &temp.value, sizeof(temp.value));
		}
		else if (keytype == BPCHAROID)
		{
			/*
			 * whitespace is the tail end of CHAR(n) data shall be ignored
			 * when we calculate hash-value, to match same text exactly.
			 */
			cl_char	   *s = VARDATA_ANY(value);
			cl_int		i, len = VARSIZE_ANY_EXHDR(value);

			for (i = len - 1; i >= 0 && s[i] == ' '; i--)
				;
			COMP_LEGACY_CRC32(hash, VARDATA_ANY(value), i+1);
		}
		else if (keybyval)
			COMP_LEGACY_CRC32(hash, &value, keylen);
		else if (keylen > 0)
			COMP_LEGACY_CRC32(hash, DatumGetPointer(value), keylen);
		else
			COMP_LEGACY_CRC32(hash,
							  VARDATA_ANY(value),
							  VARSIZE_ANY_EXHDR(value));
	}
	FIN_LEGACY_CRC32(hash);

	*p_is_null_keys = is_null_keys;

	return hash;
}

/*
 * gpujoin_inner_hash_preload_TC
 *
 * It preloads a part of inner relation, within a particular range of
 * hash-values, to the data store with hash-format, for hash-join
 * execution. Its source is preliminary materialized within tuple-store
 * of PostgreSQL.
 */
static void
gpujoin_inner_hash_preload_TS(GpuJoinState *gjs,
							  innerState *istate)
{
	PlanState		   *scan_ps = istate->state;
	TupleTableSlot	   *scan_slot = scan_ps->ps_ResultTupleSlot;
	TupleDesc			scan_desc = scan_slot->tts_tupleDescriptor;
	Tuplestorestate	   *tupstore = istate->tupstore;
	pgstrom_data_store *pds_hash;
	List			   *pds_list = NIL;
	List			   *hash_max_list = NIL;
	Size				curr_size = 0;
	Size				curr_nitems = 0;
	Size				kds_length;
	pg_crc32			hash_min;
	pg_crc32			hash_max;
	ListCell		   *lc1;
	ListCell		   *lc2;
	cl_uint				i;

	/* tuplestore must be built */
	Assert(tupstore != NULL);

	hash_min = 0;
	for (i=0; i < istate->hgram_width; i++)
	{
		Size	next_size = istate->hgram_size[i];
		Size	next_nitems = istate->hgram_nitems[i];
		Size	next_length;

		next_length = KDS_CALCULATE_HASH_LENGTH(scan_desc->natts,
												curr_nitems + next_nitems,
												curr_size + next_size);
		if (next_length > istate->pds_limit)
		{
			if (curr_size == 0)
				elog(ERROR, "Too extreme hash-key distribution");

			kds_length = KDS_CALCULATE_HASH_LENGTH(scan_desc->natts,
												   curr_nitems,
												   curr_size);
			hash_max = i * (1U << istate->hgram_shift) - 1;
			pds_hash = PDS_create_hash(gjs->gts.gcontext,
									   scan_desc,
									   kds_length);
			pds_hash->kds.hash_min = hash_min;
			pds_hash->kds.hash_max = hash_max;

			pds_list = lappend(pds_list, pds_hash);
			hash_max_list = lappend_int(hash_max_list, (int) hash_max);
			/* reset counter */
			hash_min = hash_max + 1;
			curr_size = 0;
			curr_nitems = 0;
		}
		curr_size += next_size;
		curr_nitems += next_nitems;
	}
	/*
	 * The last partitioned chunk
	 */
	kds_length = KDS_CALCULATE_HASH_LENGTH(scan_desc->natts,
										   curr_nitems,
										   curr_size + BLCKSZ);
	pds_hash = PDS_create_hash(gjs->gts.gcontext,
							   scan_desc,
							   kds_length);
	pds_hash->kds.hash_min = hash_min;
	pds_hash->kds.hash_max = UINT_MAX;
	pds_list = lappend(pds_list, pds_hash);
	hash_max_list = lappend_int(hash_max_list, (int) UINT_MAX);

	/*
	 * Load from the tuplestore
	 */
	while (tuplestore_gettupleslot(tupstore, true, false, scan_slot))
	{
		pg_crc32	hash;
		bool		is_null_keys;

		hash = get_tuple_hashvalue(istate, true, scan_slot, &is_null_keys);

		/*
		 * It is obvious all-NULLs keys shall not match any outer tuples.
		 * In case INNER or RIGHT join, this tuple shall be never referenced,
		 * so we drop these tuples from the inner buffer.
		 */
		if (is_null_keys && (istate->join_type == JOIN_INNER ||
							 istate->join_type == JOIN_LEFT))
			continue;

		forboth (lc1, pds_list,
				 lc2, hash_max_list)
		{
			pgstrom_data_store *pds = lfirst(lc1);
			pg_crc32			hash_max = (pg_crc32)lfirst_int(lc2);

			if (hash <= hash_max)
			{
				if (PDS_insert_hashitem(pds, scan_slot, hash))
					break;
				elog(ERROR, "Bug? GpuHashJoin Histgram was not correct");
			}
		}
	}

	foreach (lc1, pds_list)
	{
		pgstrom_data_store *pds_in = lfirst(lc1);
		PDS_shrink_size(pds_in);
	}
	Assert(istate->pds_list == NIL);
	istate->pds_list = pds_list;

	/* no longer tuple-store is needed */
	tuplestore_end(istate->tupstore);
	istate->tupstore = NULL;
}

/*
 * gpujoin_inner_hash_preload
 *
 * Preload inner relation to the data store with hash-format, for hash-
 * join execution.
 */
static bool
gpujoin_inner_hash_preload(GpuJoinState *gjs,
						   innerState *istate,
						   Size *p_total_usage)
{
	PlanState		   *scan_ps = istate->state;
	TupleTableSlot	   *scan_slot;
	TupleDesc			scan_desc;
	HeapTuple			tuple;
	Size				consumption;
	pgstrom_data_store *pds_hash = NULL;
	pg_crc32			hash;
	bool				is_null_keys;
	cl_int				index;
	ListCell		   *lc;

next:
	scan_slot = ExecProcNode(istate->state);
	if (TupIsNull(scan_slot))
	{
		if (istate->tupstore)
			gpujoin_inner_hash_preload_TS(gjs, istate);
		/* put an empty hash table if no rows read */
		if (istate->pds_list == NIL)
		{
			Size	empty_len;

			scan_slot = scan_ps->ps_ResultTupleSlot;
			scan_desc = scan_slot->tts_tupleDescriptor;
			empty_len = KDS_CALCULATE_HASH_LENGTH(scan_desc->natts, 0, 0);
			pds_hash = PDS_create_hash(gjs->gts.gcontext,
									   scan_desc,
									   empty_len);
			istate->pds_list = list_make1(pds_hash);
		}
		/* add extra randomness for better key distribution */
		foreach (lc, istate->pds_list)
		{
			pgstrom_data_store *pds = lfirst(lc);
			add_extra_randomness(pds);
			PDS_build_hashtable(pds);
		}
		return false;
	}

	tuple = ExecFetchSlotTuple(scan_slot);
	hash = get_tuple_hashvalue(istate, true, scan_slot, &is_null_keys);

	/*
	 * If join keys are NULLs, it is obvious that inner tuple shall not
	 * match with outer tuples. Unless it is not referenced in outer join,
	 * we don't need to keep this tuple in the 
	 */
	if (is_null_keys && (istate->join_type == JOIN_INNER ||
						 istate->join_type == JOIN_LEFT))
		goto next;

	scan_desc = scan_slot->tts_tupleDescriptor;
	if (istate->pds_list != NIL)
		pds_hash = (pgstrom_data_store *) llast(istate->pds_list);
	else if (!istate->tupstore)
	{
		Size	ichunk_size = Max(istate->ichunk_size,
								  pgstrom_chunk_size() / 4);
		pds_hash = PDS_create_hash(gjs->gts.gcontext,
								   scan_desc,
								   ichunk_size);
		istate->pds_list = list_make1(pds_hash);
		istate->ntuples = 0;
		istate->consumed = KDS_CALCULATE_HEAD_LENGTH(scan_desc->natts);
	}

	/*
	 * Update Histgram
	 */
	consumption = MAXALIGN(offsetof(kern_hashitem, t.htup) + tuple->t_len);
	index = (hash >> istate->hgram_shift);
	istate->hgram_size[index] += consumption;
	istate->hgram_nitems[index]++;

	/*
	 * XXX - If join type is LEFT or FULL OUTER, each PDS has to be
	 * strictly partitioned by the hash-value, thus, we saves entire
	 * relation on the tuple-store, then reconstruct PDS later.
	 */
retry:
	if (istate->tupstore)
	{
		tuplestore_puttuple(istate->tupstore, tuple);
		*p_total_usage += KDS_HASH_USAGE_GROWTH(istate->ntuples,
												consumption);
		istate->ntuples++;
		istate->consumed += consumption;

		return true;
	}

	if (istate->pds_limit > 0 &&
		istate->pds_limit <= KDS_CALCULATE_HASH_LENGTH(scan_desc->natts,
													   istate->ntuples + 1,
													   istate->consumed +
													   consumption))
	{
		if (istate->join_type == JOIN_INNER ||
			istate->join_type == JOIN_LEFT)
		{
			PDS_shrink_size(pds_hash);

			pds_hash = PDS_create_hash(gjs->gts.gcontext,
									   scan_desc,
									   istate->pds_limit);
			istate->pds_list = lappend(istate->pds_list, pds_hash);
			istate->ntuples = 0;
			istate->consumed = KDS_CALCULATE_HEAD_LENGTH(scan_desc->natts);
		}
		else
		{
			/*
			 * NOTE: If join type requires inner-side is well partitioned
			 * by hash-value, we once needs to move all the entries to
			 * the tuple-store, then reconstruct them as PDS.
			 */
			kern_data_store	   *kds_hash = &pds_hash->kds;
			kern_hashitem	   *khitem;
			HeapTupleData		tupData;

			istate->tupstore = tuplestore_begin_heap(false, false, work_mem);
			for (index = 0; index < kds_hash->nslots; index++)
			{
				for (khitem = KERN_HASH_FIRST_ITEM(kds_hash, index);
					 khitem != NULL;
					 khitem = KERN_HASH_NEXT_ITEM(kds_hash, khitem))
				{
					tupData.t_len = khitem->t.t_len;
					tupData.t_data = &khitem->t.htup;
					tuplestore_puttuple(istate->tupstore, &tupData);
				}
			}
			Assert(list_length(istate->pds_list) == 1);
			PDS_release(pds_hash);
			istate->pds_list = NULL;
			/*
			 * NOTE: istate->ntuples and istate->consumed shall be updated on
			 * the if-block just after the retry: label.
			 */
			goto retry;
		}
	}

	if (!PDS_insert_hashitem(pds_hash, scan_slot, hash))
	{
		pds_hash = PDS_expand_size(gjs->gts.gcontext,
								   pds_hash,
								   2 * pds_hash->kds.length);
		llast(istate->pds_list) = pds_hash;
		goto retry;
	}
	*p_total_usage += KDS_HASH_USAGE_GROWTH(istate->ntuples,
											consumption);
	istate->ntuples++;
	istate->consumed += consumption;

	return true;
}

/*
 * gpujoin_inner_heap_preload
 *
 * Preload inner relation to the data store with row-format, for nested-
 * loop execution.
 */
static bool
gpujoin_inner_heap_preload(GpuJoinState *gjs,
						   innerState *istate,
						   Size *p_total_usage)
{
	PlanState	   *scan_ps = istate->state;
	TupleTableSlot *scan_slot;
	TupleDesc		scan_desc;
	HeapTuple		tuple;
	Size			consumption;
	pgstrom_data_store *pds_heap;

	/* fetch next tuple from inner relation */
	scan_slot = ExecProcNode(scan_ps);
	if (TupIsNull(scan_slot))
	{
		ListCell   *lc;

		/* put an empty heap table if no rows read */
		if (istate->pds_list == NIL)
		{
			Size	empty_len;

			scan_slot = scan_ps->ps_ResultTupleSlot;
			scan_desc = scan_slot->tts_tupleDescriptor;
			empty_len = STROMALIGN(offsetof(kern_data_store,
											colmeta[scan_desc->natts]));
			pds_heap = PDS_create_row(gjs->gts.gcontext,
									  scan_desc,
									  empty_len);
			istate->pds_list = list_make1(pds_heap);
		}
		/* add extra randomness for better key distribution */
		foreach (lc, istate->pds_list)
			add_extra_randomness((pgstrom_data_store *) lfirst(lc));
		return false;
	}
	scan_desc = scan_slot->tts_tupleDescriptor;

	if (istate->pds_list != NIL)
		pds_heap = (pgstrom_data_store *) llast(istate->pds_list);
	else
	{
		Size	ichunk_size = Max(istate->ichunk_size,
								  pgstrom_chunk_size() / 4);
		pds_heap = PDS_create_row(gjs->gts.gcontext,
								  scan_desc,
								  ichunk_size);
		istate->pds_list = list_make1(pds_heap);
		istate->consumed = KDS_CALCULATE_HEAD_LENGTH(scan_desc->natts);
		istate->ntuples = 0;
	}

	tuple = ExecFetchSlotTuple(scan_slot);
	consumption = sizeof(cl_uint) +		/* for offset table */
		LONGALIGN(offsetof(kern_tupitem, htup) + tuple->t_len);

	/*
	 * Switch to the new chunk, if current one exceeds the limitation
	 */
	if (istate->pds_limit > 0 &&
		istate->pds_limit <= KDS_CALCULATE_ROW_LENGTH(scan_desc->natts,
													  istate->ntuples + 1,
													  istate->consumed +
													  consumption))
	{
		pds_heap = PDS_create_row(gjs->gts.gcontext,
								  scan_desc,
								  pds_heap->kds.length);
		istate->pds_list = lappend(istate->pds_list, pds_heap);
		istate->consumed = KDS_CALCULATE_HEAD_LENGTH(scan_desc->natts);
		istate->ntuples = 0;
	}

retry:
	if (!PDS_insert_tuple(pds_heap, scan_slot))
	{
		PDS_expand_size(gjs->gts.gcontext,
						pds_heap,
						2 * pds_heap->kds.length);
		goto retry;
	}
	*p_total_usage += KDS_ROW_USAGE_GROWTH(istate->ntuples,
										   consumption);
	istate->ntuples++;
	istate->consumed += consumption;

	return true;
}

/*
 * gpujoin_create_multirels
 *
 * It construct an empty pgstrom_multirels
 */
static pgstrom_multirels *
gpujoin_create_multirels(GpuJoinState *gjs)
{
	GpuContext_v2  *gcontext = gjs->gts.gcontext;
	pgstrom_multirels  *pmrels;
	Size			ojmap_length = 0;
	Size			head_length;
	Size			required;
	int				i;
	char		   *pos;

	/* calculation of outer-join map length */
	for (i=0; i < gjs->num_rels; i++)
	{
		innerState	   *istate = &gjs->inners[i];

		if (istate->join_type == JOIN_RIGHT ||
			istate->join_type == JOIN_FULL)
		{
			pgstrom_data_store *pds = list_nth(istate->pds_list,
											   istate->pds_index - 1);
			ojmap_length += STROMALIGN(pds->kds.nitems);
		}
	}

	/* calculate total length and allocate */
	head_length = STROMALIGN(offsetof(pgstrom_multirels,
									  kern.chunks[gjs->num_rels]));
	required = head_length +
		STROMALIGN(sizeof(pgstrom_data_store *) * gjs->num_rels) +
		2 * sizeof(cl_bool) * STROMALIGN(ojmap_length);
	pmrels = dmaBufferAlloc(gcontext, required);
	memset(pmrels, 0, required);
	pos = (char *)pmrels + head_length;

	pmrels->gjs = gjs;	// deprecated
	pmrels->head_length = head_length;
	pmrels->usage_length = head_length;
	pmrels->inner_chunks = (pgstrom_data_store **) pos;
	pos += STROMALIGN(sizeof(pgstrom_data_store *) * gjs->num_rels);
	SpinLockInit(&pmrels->lock);
	pmrels->n_attached = 1;
	pmrels->refcnt = 0;
	pmrels->m_kmrels = 0UL;
	pmrels->ev_loaded = NULL;
	pmrels->m_ojmaps = 0UL;
	pmrels->h_ojmaps = (cl_bool *)(ojmap_length > 0 ? pos : NULL);

	memcpy(pmrels->kern.pg_crc32_table,
		   pg_crc32_table,
		   sizeof(cl_uint) * 256);
	pmrels->kern.nrels = gjs->num_rels;
	pmrels->kern.ojmap_length = 0;
	memset(pmrels->kern.chunks,
		   0,
		   offsetof(pgstrom_multirels, kern.chunks[gjs->num_rels]) -
		   offsetof(pgstrom_multirels, kern.chunks[0]));

	for (i=0; i < gjs->num_rels; i++)
	{
		innerState			   *istate = &gjs->inners[i];
		pgstrom_data_store	   *pds = list_nth(istate->pds_list,
											   istate->pds_index - 1);

		pmrels->inner_chunks[i] = PDS_retain(pds);
		pmrels->kern.chunks[i].chunk_offset = pmrels->usage_length;
		pmrels->usage_length += STROMALIGN(pds->kds.length);

		if (!istate->hash_outer_keys)
			pmrels->kern.chunks[i].is_nestloop = true;

		if (istate->join_type == JOIN_RIGHT ||
			istate->join_type == JOIN_FULL)
		{
			pmrels->kern.chunks[i].right_outer = true;
			pmrels->kern.chunks[i].ojmap_offset = pmrels->kern.ojmap_length;
			pmrels->kern.ojmap_length += STROMALIGN(pds->kds.nitems);
			pmrels->needs_outer_join = true;
		}
		if (istate->join_type == JOIN_LEFT ||
			istate->join_type == JOIN_FULL)
			pmrels->kern.chunks[i].left_outer = true;
	}
	Assert(pmrels->kern.ojmap_length == ojmap_length);
	return pmrels;
}

/*
 * gpujoin_inner_preload
 *
 * It preload inner relation to the GPU DMA buffer once, even if larger
 * than device memory. If size is over the capacity, inner chunks are
 * splitted into multiple portions.
 */
static bool
gpujoin_inner_preload(GpuJoinState *gjs)
{
	innerState	  **istate_buf;
	cl_int			istate_nums = gjs->num_rels;
	Size			total_limit;
	Size			total_usage;
	bool			kmrels_size_fixed = false;
	int				i;

	/*
	 * Half of the max allocatable GPU memory (and minus some margin) is
	 * the current hard limit of the inner relations buffer.
	 */
	total_limit = Min(gpuMemMaxAllocSize() / 2,
					  dmaBufferMaxAllocSize()) - BLCKSZ * gjs->num_rels;
	total_usage = STROMALIGN(offsetof(kern_multirels,
									  chunks[gjs->num_rels]));
	istate_buf = palloc0(sizeof(innerState *) * gjs->num_rels);
	for (i=0; i < istate_nums; i++)
		istate_buf[i] = &gjs->inners[i];

	/* load tuples from the inner relations with round-robin policy */
	while (istate_nums > 0)
	{
		for (i=0; i < istate_nums; i++)
		{
			innerState *istate = istate_buf[i];

			if (!(istate->hash_inner_keys != NIL
				  ? gpujoin_inner_hash_preload(gjs, istate, &total_usage)
				  : gpujoin_inner_heap_preload(gjs, istate, &total_usage)))
			{
				memmove(istate_buf + i,
						istate_buf + i + 1,
						sizeof(innerState *) * (istate_nums - (i+1)));
				istate_nums--;
				i--;
			}
		}

		if (!kmrels_size_fixed && total_usage >= total_limit)
		{
			/*
			 * NOTE: current usage becomes limitation, so next call of
			 * gpujoin_inner_XXXX_preload will make its second chunk.
			 */
			for (i=0; i < gjs->num_rels; i++)
			{
				innerState	   *istate = istate_buf[i];
				TupleTableSlot *scan_slot = istate->state->ps_ResultTupleSlot;
				TupleDesc	 	scan_desc = scan_slot->tts_tupleDescriptor;

				gjs->inners[i].pds_limit =
					(istate->hash_inner_keys != NIL
					 ? KDS_CALCULATE_HASH_LENGTH(scan_desc->natts,
												 istate->ntuples,
												 istate->consumed)
					 : KDS_CALCULATE_ROW_LENGTH(scan_desc->natts,
												istate->ntuples,
												istate->consumed));
			}
			kmrels_size_fixed = true;
		}
	}

	/*
	 * XXX - It is ideal case; all the inner chunk can be loaded to
	 * a single multi-relations buffer.
	 */
	if (!kmrels_size_fixed)
	{
		for (i=0; i < gjs->num_rels; i++)
			gjs->inners[i].pds_limit = gjs->inners[i].consumed;
	}
	pfree(istate_buf);

	/*
	 * NOTE: Special optimization case. In case when any chunk has no items,
	 * and all deeper level is inner join, it is obvious no tuples shall be
	 * produced in this GpuJoin. We can omit outer relation load that shall
	 * be eventually dropped.
	 */
	for (i=gjs->num_rels; i > 0; i--)
	{
		innerState	   *istate = &gjs->inners[i-1];

		/* outer join can produce something from empty */
		if (istate->join_type != JOIN_INNER)
			break;

		if (list_length(istate->pds_list) == 1)
		{
			pgstrom_data_store *pds_in = linitial(istate->pds_list);

			if (pds_in->kds.nitems == 0)
				return false;
		}
	}

	/* How much chunks actually needed? */
	for (i=0; i < gjs->num_rels; i++)
	{
		int		nbatches_exec = list_length(gjs->inners[i].pds_list);

		Assert(nbatches_exec > 0);
		gjs->inners[i].nbatches_exec = nbatches_exec;
	}
	return true;
}

/*
 * gpujoin_inner_getnext
 *
 * It constructs the next inner buffer based on current index of inner
 * relations.
 */
static pgstrom_multirels *
gpujoin_inner_getnext(GpuJoinState *gjs)
{
	int		i, j;

	if (!gjs->inner_preloaded)
	{
		if (!gpujoin_inner_preload(gjs))
			return NULL;	/* no join result is expected */
		gjs->inner_preloaded = true;
		/* setup initial inner index position */
		for (i=0; i < gjs->num_rels; i++)
			gjs->inners[i].pds_index = 1;
	}
	else
	{
		/*
		 * Make advance the index of inner chunks
		 */
		for (i=gjs->num_rels; i > 0; i--)
		{
			innerState	   *istate = &gjs->inners[i-1];

			if (istate->pds_index < list_length(istate->pds_list))
			{
				istate->pds_index++;
				for (j=i; j < gjs->num_rels; j++)
					gjs->inners[j].pds_index = 1;
				break;
			}
		}
		if (i == 0)
			return NULL;	/* end of inner chunks */
	}

	/*
	 * OK, makes next pgstrom_multirels buffer
	 */
	return gpujoin_create_multirels(gjs);
}

/*
 * multirels_attach_buffer
 *
 * It attache multirels buffer on a particular gpujoin task.
 */
static pgstrom_multirels *
multirels_attach_buffer(pgstrom_multirels *pmrels)
{
	SpinLockAcquire(&pmrels->lock);
	/* attach this pmrels */
	Assert(pmrels->n_attached > 0);
	pmrels->n_attached++;
	SpinLockRelease(&pmrels->lock);

	return pmrels;
}

/*
 * multirels_get_buffer
 *
 *
 */
static inline bool
__multirels_get_buffer(pgstrom_gpujoin *pgjoin,
					   pgstrom_multirels *pmrels,
					   CUstream cuda_stream)
{
	GpuContext_v2  *gcontext = pgjoin->task.gcontext;
	CUdeviceptr		m_kmrels = 0UL;
	CUdeviceptr		m_ojmaps = 0UL;
	CUevent			ev_loaded;
	Size			length;
	Size			offset;
	Size			total_length = 0;
	int				i;
	CUresult		rc;

	/* buffer allocation for the inner multi-relations */
	rc = gpuMemAlloc(gcontext, &m_kmrels, pmrels->usage_length);
	if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		return false;
	else if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuMemAlloc: %s", errorText(rc));

	/* buffer allocation for the OUTER JOIN maps, if needed */
	if (pmrels->kern.ojmap_length > 0 && !pmrels->m_ojmaps)
	{
		length = 2 * sizeof(cl_bool) * STROMALIGN(pmrels->kern.ojmap_length);
		rc = gpuMemAlloc(gcontext, &m_ojmaps, length);
		if (rc == CUDA_ERROR_OUT_OF_MEMORY)
		{
			gpuMemFree(gcontext, m_kmrels);
			return false;
		}
		else if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuMemAlloc: %s", errorText(rc));

		/* Zero clear of the LEFT OUTER map */
		rc = cuMemsetD32Async(m_ojmaps, 0, length / sizeof(int),
							  cuda_stream);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemsetD32Async: %s", errorText(rc));
	}
	/* Synchronization Event for other concurrent tasks */
	rc = cuEventCreate(&ev_loaded, CU_EVENT_DEFAULT);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuEventCreate: %s", errorText(rc));

	/* DMA send to the kern_multirels buffer */
	length = offsetof(kern_multirels, chunks[pmrels->kern.nrels]);
	rc = cuMemcpyHtoDAsync(m_kmrels, &pmrels->kern, length, cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
	total_length += length;

	for (i=0; i < pmrels->kern.nrels; i++)
	{
		pgstrom_data_store *pds = pmrels->inner_chunks[i];

		offset = pmrels->kern.chunks[i].chunk_offset;
		rc = cuMemcpyHtoDAsync(m_kmrels + offset, &pds->kds, pds->kds.length,
							   cuda_stream);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));
		total_length += pds->kds.length;
	}

	/* DMA send synchronization */
	rc = cuEventRecord(ev_loaded, cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuEventRecord: %s", errorText(rc));
	/* Save the event object and device memory */
	pmrels->ev_loaded = ev_loaded;
	pmrels->m_kmrels = m_kmrels;
	pmrels->m_ojmaps = m_ojmaps;
	pgjoin->m_kmrels = m_kmrels;
	pgjoin->m_ojmaps = m_ojmaps;

	return true;
}

static bool
multirels_get_buffer(pgstrom_gpujoin *pgjoin, CUstream cuda_stream)
{
	pgstrom_multirels *pmrels = pgjoin->pmrels;
	bool		status = true;

	PG_TRY();
	{
		SpinLockAcquire(&pmrels->lock);
		Assert(pmrels->n_attached > 0);
		Assert(pmrels->refcnt >= 0);
		if (pmrels->refcnt++ == 0)
		{
			if (__multirels_get_buffer(pgjoin, pmrels, cuda_stream))
				pgjoin->is_inner_loader = true;
			else
			{
				pmrels->refcnt--;
				status = false;
			}
		}
		else
		{
			CUevent		ev_loaded = pmrels->ev_loaded;
			CUresult	rc;

			Assert(ev_loaded != NULL);
			rc = cuStreamWaitEvent(cuda_stream, ev_loaded, 0);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on cuStreamWaitEvent: %s", errorText(rc));
			/* this task is not inner loader */
			pgjoin->m_kmrels = pmrels->m_kmrels;
			pgjoin->m_ojmaps = pmrels->m_ojmaps;
			pgjoin->is_inner_loader = false;
		}
		SpinLockRelease(&pmrels->lock);
	}
	PG_CATCH();
	{
		SpinLockRelease(&pmrels->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return status;
}

static void
multirels_put_buffer(pgstrom_gpujoin *pgjoin)
{
	pgstrom_multirels *pmrels = pgjoin->pmrels;
	CUresult	rc;

	SpinLockAcquire(&pmrels->lock);
	Assert(pmrels->n_attached > 0);
	Assert(pmrels->refcnt > 0);
	if (--pmrels->refcnt == 0)
	{
		/*
		 * OK, it looks no concurrent tasks didn't reference the inner-
		 * relations buffer any more, so release the device memory and
		 * set NULL on the pointer.
		 */
		Assert(pmrels->m_kmrels != 0UL);
		rc = gpuMemFree(pgjoin->task.gcontext, pmrels->m_kmrels);
		if (rc != CUDA_SUCCESS)
			elog(WARNING, "failed on gpuMemFree: %s", errorText(rc));
		pmrels->m_kmrels = 0UL;

		/*
		 * TODO: We have to care about outer-join map, if no concurrent
		 * task does not exist.
		 * colocation is not a lightweight task and oj-map is small chunk,
		 * so it is an option to keep it on the device side.
		 * In this case, who should own the region?
		 */

		/*
		 * Also destroy the event object
		 */
		rc = cuEventDestroy(pmrels->ev_loaded);
		if (rc != CUDA_SUCCESS)
			elog(WARNING, "failed on cuEventDestroy: %s", errorText(rc));
		pmrels->ev_loaded = NULL;
	}
	pgjoin->m_kmrels = 0UL;
	pgjoin->m_ojmaps = 0UL;
	SpinLockRelease(&pmrels->lock);
}

static void
colocate_outer_join_maps_to_host(pgstrom_multirels *pmrels)
{
//	GpuContext *gcontext = pmrels->gjs->gts.gcontext;
	Size		ojmap_length = pmrels->kern.ojmap_length;
	cl_bool	   *host_ojmaps = pmrels->h_ojmaps;
	cl_bool	   *recv_ojmaps = pmrels->h_ojmaps + ojmap_length;
	cl_int		i, n;
	CUresult	rc;

	Assert(ojmap_length % sizeof(cl_ulong) == 0);
	if (ojmap_length > 0)
	{
		rc = cuMemcpyDtoH(recv_ojmaps,
						  pmrels->m_ojmaps,
						  sizeof(cl_bool) * ojmap_length);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on cuMemcpyDtoH: %s", errorText(rc));

		n = ojmap_length / sizeof(cl_ulong);
		for (i=0; i < n; i++)
		{
			cl_ulong   *dest = (cl_ulong *)host_ojmaps + i;
			cl_ulong   *recv = (cl_ulong *)recv_ojmaps + i;

			*dest |= *recv;
		}
	}
}

static void
colocate_outer_join_maps_to_device(pgstrom_gpujoin *pgjoin,
								   CUmodule cuda_module,
								   CUstream cuda_stream)
{
	pgstrom_multirels *pmrels = pgjoin->pmrels;
	cl_uint			ojmap_length = pmrels->kern.ojmap_length;
	CUdeviceptr		dst_ojmaps;
	CUfunction		kern_colocate;
	void		   *kern_args[2];
	size_t			grid_size;
	size_t			block_size;
	CUresult		rc;

	Assert(pmrels->m_ojmaps != 0UL);

	/* Lookup GPU kernel function */
	rc = cuModuleGetFunction(&kern_colocate, cuda_module,
							 "gpujoin_colocate_outer_join_map");
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuModuleGetFunction: %s", errorText(rc));

	/* calculation of the optimal number of threads */
	optimal_workgroup_size(&grid_size,
						   &block_size,
						   kern_colocate,
						   gpuserv_cuda_device,
						   ojmap_length / sizeof(cl_uint),
						   0, 0);	/* no shared memory usage */

	/* destination address on the device side */
	dst_ojmaps = pmrels->m_ojmaps + sizeof(cl_bool) * ojmap_length;

	/* host-to-device colocation */
	rc = cuMemcpyHtoDAsync(dst_ojmaps,
						   pmrels->h_ojmaps,
						   sizeof(cl_bool) * ojmap_length,
						   cuda_stream);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuMemcpyHtoDAsync: %s", errorText(rc));

	/*
	 * KERNEL_FUNCTION(void)
	 * gpujoin_colocate_outer_join_map(kern_multirels *kmrels,
	 *                                 cl_bool *outer_join_map)
	 */
	kern_args[0] = &pgjoin->m_kmrels;
	kern_args[1] = &pgjoin->m_ojmaps;

	rc = cuLaunchKernel(kern_colocate,
						grid_size, 1, 1,
						block_size, 1, 1,
						0,	/* no shmem usage */
						cuda_stream,
						kern_args,
						NULL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on cuLaunchKernel: %s", errorText(rc));
}

static void
multirels_detach_buffer(GpuJoinState *gjs,
						pgstrom_multirels *pmrels,
						bool may_kick_outer_join)
{
	pgstrom_gpujoin *pgjoin_new;
	int		i;

	Assert(!IsGpuServerProcess());
retry:
	SpinLockAcquire(&pmrels->lock);
	Assert(pmrels->n_attached > 0);
	/*
	 * NOTE: Invocation of multirels_detach_buffer with n_attached==1 means
	 * release of pgstrom_multirels buffer. If GpuJoin contains RIGHT or
	 * FULL OUTER JOIN, we need to kick OUTER JOIN task prior on the last.
	 * pgstrom_gpujoin task with pds_src==NULL means OUTER JOIN launch.
	 */
	if (may_kick_outer_join &&
		pmrels->n_attached == 1 &&
		pmrels->needs_outer_join)
	{
		/* no need to kick OUTER JOIN task twice */
		pmrels->needs_outer_join = false;
		SpinLockRelease(&pmrels->lock);

		/* construct a GpuJoin task for OUTER JOIN, then send a request */

		//HOGE: do we need to give GJS here?
		pgjoin_new = (pgstrom_gpujoin *)
			gpujoin_create_task(gjs, pmrels, NULL, -1, NULL);
		gpuservSendGpuTask(gjs->gts.gcontext, &pgjoin_new->task);

		goto retry;
	}

	/*
	 * Last GpuJoin task dettached @pmrels, so release relevant resources
	 */
	if (--pmrels->n_attached > 0)
		SpinLockRelease(&pmrels->lock);
	else
	{
		/*
		 * Nobody should reference the device memory no longer.
		 */
		Assert(pmrels->refcnt == 0);
		Assert(pmrels->m_kmrels == 0UL);
		Assert(pmrels->ev_loaded == NULL);
		Assert(pmrels->m_ojmaps == 0UL);

		for (i=0; i < pmrels->kern.nrels; i++)
			PDS_release(pmrels->inner_chunks[i]);
		SpinLockRelease(&pmrels->lock);

		dmaBufferFree(pmrels);
	}
}

/*
 * pgstrom_init_gpujoin
 *
 * Entrypoint of GpuJoin
 */
void
pgstrom_init_gpujoin(void)
{
	/* turn on/off gpunestloop */
	DefineCustomBoolVariable("pg_strom.enable_gpunestloop",
							 "Enables the use of GpuNestLoop logic",
							 NULL,
							 &enable_gpunestloop,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off gpuhashjoin */
	DefineCustomBoolVariable("pg_strom.enable_gpuhashjoin",
							 "Enables the use of GpuHashJoin logic",
							 NULL,
							 &enable_gpuhashjoin,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* setup path methods */
	gpujoin_path_methods.CustomName				= "GpuJoin";
	gpujoin_path_methods.PlanCustomPath			= PlanGpuJoinPath;

	/* setup plan methods */
	gpujoin_plan_methods.CustomName				= "GpuJoin";
	gpujoin_plan_methods.CreateCustomScanState	= gpujoin_create_scan_state;
	RegisterCustomScanMethods(&gpujoin_plan_methods);

	/* setup exec methods */
	gpujoin_exec_methods.CustomName				= "GpuJoin";
	gpujoin_exec_methods.BeginCustomScan		= ExecInitGpuJoin;
	gpujoin_exec_methods.ExecCustomScan			= ExecGpuJoin;
	gpujoin_exec_methods.EndCustomScan			= ExecEndGpuJoin;
	gpujoin_exec_methods.ReScanCustomScan		= ExecReScanGpuJoin;
	gpujoin_exec_methods.MarkPosCustomScan		= NULL;
	gpujoin_exec_methods.RestrPosCustomScan		= NULL;
	gpujoin_exec_methods.EstimateDSMCustomScan  = ExecGpuJoinEstimateDSM;
	gpujoin_exec_methods.InitializeDSMCustomScan = ExecGpuJoinInitDSM;
	gpujoin_exec_methods.InitializeWorkerCustomScan = ExecGpuJoinInitWorker;
	gpujoin_exec_methods.ExplainCustomScan		= ExplainGpuJoin;

	/* hook registration */
	set_join_pathlist_next = set_join_pathlist_hook;
	set_join_pathlist_hook = gpujoin_add_join_path;
}
