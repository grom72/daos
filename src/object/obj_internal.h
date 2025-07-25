/**
 * (C) Copyright 2016-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of daos_sr
 *
 * src/object/obj_internal.h
 */
#ifndef __DAOS_OBJ_INTENRAL_H__
#define __DAOS_OBJ_INTENRAL_H__

#include <stdint.h>
#include <daos/common.h>
#include <daos/event.h>
#include <daos/tse.h>
#include <daos/task.h>
#include <daos/placement.h>
#include <daos/btree.h>
#include <daos/btree_class.h>
#include <daos/object.h>
#include <daos/cont_props.h>
#include <daos/container.h>
#include <daos/tls.h>

#include "obj_rpc.h"
#include "obj_ec.h"

/**
 * This environment is mostly for performance evaluation.
 */
#define IO_BYPASS_ENV	"DAOS_IO_BYPASS"

struct obj_io_context;

/**
 * Bypass client I/O RPC, it means the client stack will complete the
 * fetch/update RPC immediately, nothing will be submitted to remote server.
 * This mode is for client I/O stack performance benchmark.
 */
extern bool	cli_bypass_rpc;
/** Switch of server-side IO dispatch */
extern unsigned int	srv_io_mode;
extern unsigned int	obj_coll_thd;
extern btr_ops_t	dbtree_coll_ops;

/** See comments in obj_sgls_dup(), tune iov merge conditions */
extern unsigned int     iov_frag_count;
extern unsigned int     iov_frag_size;

/* Whether check redundancy group validation when DTX resync. */
extern bool	tx_verify_rdg;

/** client object shard */
struct dc_obj_shard {
	/** refcount */
	unsigned int		do_ref;
	uint32_t		do_target_rank;
	/** object id */
	daos_unit_oid_t		do_id;
	/** container ptr */
	struct dc_cont		*do_co;
	struct pl_obj_shard	do_pl_shard;
	/** point back to object */
	struct dc_object	*do_obj;
	uint32_t		do_shard_idx;
	uint8_t			do_target_idx;	/* target VOS index in node */
};

#define do_shard	do_pl_shard.po_shard
#define do_target_id	do_pl_shard.po_target
#define do_fseq		do_pl_shard.po_fseq
#define do_rebuilding	do_pl_shard.po_rebuilding
#define do_reintegrating do_pl_shard.po_reintegrating

/** client object layout */
struct dc_obj_layout {
	/** The reference for the shards that are opened (in-using). */
	unsigned int		do_open_count;
	struct dc_obj_shard	do_shards[0];
};

/** Client stack object */
struct dc_object {
	/** link chain in the global handle hash table */
	struct d_hlink		 cob_hlink;
	/**
	 * Object metadata stored in the OI table. For those object classes
	 * and have no metadata in OI table, DAOS only stores OID and pool map
	 * version in it.
	 */
	struct daos_obj_md	 cob_md;
	/** object class attribute */
	struct daos_oclass_attr	 cob_oca;
	/** container ptr */
	struct dc_cont		*cob_co;
	/** pool ptr */
	struct dc_pool		*cob_pool;
	/** cob_spin protects obj_shards' do_ref */
	pthread_spinlock_t	 cob_spin;

	/* cob_lock protects layout and shard objects ptrs */
	pthread_rwlock_t	 cob_lock;

	/** object open mode */
	unsigned int		 cob_mode;
	unsigned int		 cob_version;
	unsigned int		 cob_shards_nr;
	unsigned int		 cob_grp_size;
	unsigned int		 cob_grp_nr;
	/* The max rank# on which some object shard exists. */
	uint32_t		 cob_min_rank;
	/* The min rank# on which some object shard exists. */
	uint32_t		 cob_max_rank;
	/* How many ranks on which some object shard resides. */
	uint32_t		 cob_rank_nr;
	/**
	 * The array for the latest time (in second) of
	 * being asked to fetch from leader.
	 */
	uint64_t		*cob_time_fetch_leader;
	/** shard object ptrs */
	struct dc_obj_layout	*cob_shards;

	/* The current layout version for the object. */
	uint32_t		cob_layout_version;
};

/* to record EC singv fetch stat from different shards */
struct shard_fetch_stat {
	/* iod_size for array; or iod_size for EC singv on shard 0 or parity shards, those shards
	 * always be updated when EC singv being overwritten.
	 */
	daos_size_t		sfs_size;
	/* iod_size on other shards, possibly be missed when EC singv overwritten. */
	daos_size_t		sfs_size_other;
	/* rc on shard 0 or parity shards */
	int32_t			sfs_rc;
	/* rc on other data shards */
	int32_t			sfs_rc_other;
};

/**
 * Reassembled obj request.
 * User input iod/sgl possibly need to be reassembled at client before sending
 * to server, for example:
 * 1) merge adjacent recxs, or sort out-of-order recxs and generate new sgl to
 *    match with it;
 * 2) For EC obj, split iod/recxs to each target, generate new sgl to match with
 *    it, create oiod/siod to specify each shard/tgt's IO req.
 */
struct obj_reasb_req {
	/* object ID */
	daos_obj_id_t			 orr_oid;
	/* epoch for IO (now only used for fetch */
	struct dtx_epoch		 orr_epoch;
	/* original obj IO API args */
	daos_obj_rw_t			*orr_args;
	/* original user input iods/sgls */
	daos_iod_t			*orr_uiods;
	d_sg_list_t			*orr_usgls;
	/* reassembled iods/sgls */
	daos_iod_t			*orr_iods;
	d_sg_list_t			*orr_sgls;
	struct obj_io_desc		*orr_oiods;
	struct obj_ec_recx_array	*orr_recxs;
	struct obj_ec_seg_sorter	*orr_sorters;
	struct dcs_layout		*orr_singv_los;
	/* to record returned data size from each targets */
	daos_size_t			*orr_data_sizes;
	/* number of targets this IO req involves */
	uint32_t			 orr_tgt_nr;
	/* number of targets that with IOM handled */
	uint32_t			 orr_iom_tgt_nr;
	/* number of iom extends */
	uint32_t			 orr_iom_nr;
	/* #iods of IO req */
	uint32_t			 orr_iod_nr;
	struct daos_oclass_attr		*orr_oca;
	struct obj_ec_codec		*orr_codec;
	pthread_mutex_t			 orr_mutex;
	/* target bitmap, one bit for each target (from first data cell to last parity cell. */
	uint8_t				*tgt_bitmap;
	/* fetch stat, one per iod */
	struct shard_fetch_stat		*orr_fetch_stat;
	struct obj_tgt_oiod		*tgt_oiods;
	/* IO failure information */
	struct obj_ec_fail_info		*orr_fail;
	/* parity recx list (to compare parity ext/epoch when data recovery) */
	struct daos_recx_ep_list	*orr_parity_lists;
	uint32_t			 orr_parity_list_nr;
	/* for data recovery flag */
	uint32_t			 orr_recov:1,
	/* for snapshot data recovery flag */
					 orr_recov_snap:1,
	/* for iod_size fetching flag */
					 orr_size_fetch:1,
	/* for iod_size fetched flag */
					 orr_size_fetched:1,
	/* only with single data target flag */
					 orr_single_tgt:1,
	/* only for single-value IO flag */
					 orr_singv_only:1,
	/* the flag of IOM re-allocable (used for EC IOM merge) */
					 orr_iom_realloc:1,
	/* orr_fail allocated flag, recovery task's orr_fail is inherited */
					 orr_fail_alloc:1,
	/* The fetch data/sgl is rebuilt by EC parity rebuild */
					 orr_recov_data:1;
};

static inline void
enum_anchor_copy(daos_anchor_t *dst, daos_anchor_t *src)
{
	memcpy(dst, src, sizeof(*dst));
}

struct obj_ec_parity {
	unsigned char	**p_bufs;
	unsigned int	  p_nr;
};

enum obj_rpc_opc;

typedef int (*shard_io_cb_t)(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
			     void *shard_args,
			     struct daos_shard_tgt *fw_shard_tgts,
			     uint32_t fw_cnt, tse_task_t *task);

struct obj_coll_disp_cursor {
	/*
	 * The length of daos_coll_target array. The obj_coll_disp_cursor may be inside some
	 * {obj,shard}_auxi_xxx structure, has some size limitation. So the daos_coll_target
	 * array is not contained inside the obj_coll_disp_cursor.
	 */
	uint32_t	tgt_nr;
	/*
	 * The "grp" is not object redundancy group, instead, it is the set of some engine(s).
	 * If there is only one engine in the group, then send RPC to such engine. Otherwise,
	 * choose a relay engine from such group and send RPC to such relay engine that will
	 * help to forward the RPC to other engines in such group.
	 */
	uint16_t	grp_nr;
	/* The count of engine groups that the RPC will be dispatched to. */
	uint16_t	pending_grps;
	/* Current position in the daos_coll_target array. */
	uint32_t	cur_pos;
	/* How many engines in the group corresponding to cur_pos. As the process going, the
	 * count of engines in current group may be smaller than the engines in former group
	 * unless fixed_step is set.
	 */
	uint16_t	cur_step;
	uint16_t	fixed_step:1;
};

/* shard update/punch auxiliary args, must be the first field of
 * shard_rw_args and shard_punch_args.
 */
struct shard_auxi_args {
	struct obj_auxi_args	*obj_auxi;
	shard_io_cb_t		 shard_io_cb;
	struct dtx_epoch	 epoch;
	uint32_t		 shard;
	uint32_t		 target;
	uint32_t		 map_ver;
	/* only for EC, the target idx [0, k + p) */
	uint16_t		 ec_tgt_idx;
	/* group index within the req_tgts->ort_shard_tgts */
	uint16_t		 grp_idx;
	/* only for EC, the start shard of the EC stripe */
	uint32_t		 start_shard;
	uint32_t		 flags;
	/* for retried RPC */
	uint64_t		 enqueue_id;
};

struct sgl_merge_ctx {
	d_sg_list_t *sgls_dup;
	d_sg_list_t *sgls_orig;
	uint64_t   **merged_bitmaps;
	uint64_t   **alloc_bitmaps;
};

struct shard_rw_args {
	struct shard_auxi_args	 auxi;
	struct sgl_merge_ctx    *merge_ctx;
	struct dtx_id		 dti;
	crt_bulk_t		*bulks;
	struct obj_io_desc	*oiods;
	uint64_t		*offs;
	struct dcs_csum_info	*dkey_csum;
	struct dcs_iod_csums	*iod_csums;
	struct obj_reasb_req	*reasb_req;
	uint16_t		 csum_retry_cnt;
};

struct coll_sparse_targets {
	struct btr_root		 cst_tree_root;
	daos_handle_t		 cst_tree_hdl;
};

struct coll_oper_args {
	struct shard_auxi_args	 coa_auxi;
	int			 coa_dct_nr;
	uint32_t		 coa_dct_cap;
	union {
		/* Save obj->cob_min_rank for verification during obj_coll_prep_one(). */
		uint32_t	 coa_min_rank;
		/* Only can be used since obj_coll_oper_args_collapse() after object layout scan. */
		uint32_t	 coa_max_dct_sz;
	};
	uint8_t			 coa_max_shard_nr;
	uint8_t			 coa_max_bitmap_sz;
	uint8_t			 coa_for_modify:1,
				 coa_raw_sparse:1,
				 coa_sparse:1;
	uint8_t			 coa_target_nr;
	/*
	 * The target ID for the top four healthy shards.
	 * Please check comment for DTX_COLL_INLINE_TARGETS.
	 */
	uint32_t		 coa_targets[DTX_COLL_INLINE_TARGETS];
	union {
		struct daos_coll_target		*coa_dcts;
		struct coll_sparse_targets	*coa_tree;
	};
};

struct shard_punch_args {
	union {
		struct shard_auxi_args	pa_auxi;
		struct coll_oper_args	pa_coa;
	};
	struct dtx_id			pa_dti;
	uint32_t			pa_opc;
};

struct shard_sub_anchor {
	daos_anchor_t	ssa_anchor;
	/* These two extra anchors are for migration enumeration */
	daos_anchor_t	*ssa_akey_anchor;
	daos_anchor_t	*ssa_recx_anchor;
	d_sg_list_t	ssa_sgl;
	daos_key_desc_t	*ssa_kds;
	daos_recx_t	*ssa_recxs;
	uint32_t	ssa_shard;
};

/**
 * This structure is attached to daos_anchor_t->da_sub_anchor for
 * tracking multiple shards enumeration, for example degraded EC
 * enumeration or EC parity rotate enumeration.
 */
struct shard_anchors {
	d_list_t		sa_merged_list;
	int			sa_nr;
	int			sa_anchors_nr;
	struct shard_sub_anchor	sa_anchors[0];
};

struct shard_list_args {
	struct shard_auxi_args	 la_auxi;
	struct dtx_id		 la_dti;
	daos_recx_t		*la_recxs;
	uint32_t		la_nr;
	d_sg_list_t		*la_sgl;
	daos_key_desc_t		*la_kds;
	daos_anchor_t		*la_anchor;
	daos_anchor_t		*la_akey_anchor;
	daos_anchor_t		*la_dkey_anchor;
};

struct obj_auxi_list_recx {
	daos_recx_t	recx;
	daos_epoch_t	recx_eph;
	d_list_t	recx_list;
};

struct obj_auxi_list_key {
	d_iov_t		key;
	struct ktr_hkey	hkey;
	d_list_t	key_list;
};

struct obj_auxi_list_obj_enum {
	d_iov_t		dkey;
	d_list_t	enum_list;
	daos_iod_t	*iods;
	d_list_t	*recx_lists;
	int		iods_nr;
};

struct shard_sync_args {
	struct shard_auxi_args	 sa_auxi;
	daos_epoch_t		*sa_epoch;
};

struct shard_k2a_args {
	struct shard_auxi_args	 ka_auxi;
	struct dtx_id		 ka_dti;
	daos_anchor_t		*ka_anchor;
};

#define OBJ_TGT_INLINE_NR	9
#define OBJ_INLINE_BTIMAP	4

struct obj_req_tgts {
	/* to save memory allocation if #targets <= OBJ_TGT_INLINE_NR */
	struct daos_shard_tgt	 ort_tgts_inline[OBJ_TGT_INLINE_NR];
	/* Shard target array, with (ort_grp_nr * ort_grp_size) targets.
	 * If #targets <= OBJ_TGT_INLINE_NR then it points to ort_tgts_inline.
	 * Within the array, [0, ort_grp_size - 1] is for the first group,
	 * [ort_grp_size, ort_grp_size * 2 - 1] is the 2nd group and so on.
	 * If (ort_srv_disp == 1) then within each group the first target is the
	 * leader shard and following (ort_grp_size - 1) targets are the forward
	 * non-leader shards.
	 * Now there is only one case for (ort_grp_nr > 1) that for object
	 * punch, all other cases with (ort_grp_nr == 1).
	 */
	struct daos_shard_tgt	*ort_shard_tgts;
	uint32_t		 ort_grp_nr;
	/* ort_grp_size is the size of the group that is sent as forwarded
	 * shards
	 */
	uint32_t		 ort_grp_size;
	/* ort_start_shard is only for EC object, it is the start shard number
	 * of the EC stripe. To facilitate calculate the offset of different
	 * shards in the stripe.
	 */
	uint32_t		 ort_start_shard;
	/* flag of server dispatch */
	uint32_t		 ort_srv_disp:1;
};

struct obj_auxi_tgt_list {
	/** array of target ID */
	uint32_t	*tl_tgts;
	/** number of ranks & tgts */
	uint32_t	tl_nr;
};

struct coll_query_args {
	union {
		struct shard_auxi_args	cqa_auxi;
		struct coll_oper_args	cqa_coa;
	};
	struct obj_coll_disp_cursor	cqa_cur;
};

/* Auxiliary args for object I/O */
struct obj_auxi_args {
	tse_task_t			*obj_task;
	daos_handle_t			 th;
	struct dc_object		*obj;
	int				 opc;
	int				 result;
	uint32_t			 map_ver_req;
	uint32_t			 map_ver_reply;
	/* max delay seconds for retry */
	uint32_t			 max_delay;
	/* flags for the obj IO task.
	 * ec_wait_recov -- obj fetch wait another EC recovery task,
	 * ec_in_recov -- a EC recovery task
	 */
	uint32_t			 io_retry:1,
					 args_initialized:1,
					 to_leader:1,
					 spec_shard:1,
					 spec_group:1,
					 req_reasbed:1,
					 is_ec_obj:1,
					 csum_retry:1,
					 csum_report:1,
					 tx_uncertain:1,
					 nvme_io_err:1,
					 no_retry:1,
					 ec_wait_recov:1,
					 ec_in_recov:1,
					 new_shard_tasks:1,
					 reset_param:1,
					 force_degraded:1,
					 shards_scheded:1,
					 sub_anchors:1,
					 ec_degrade_fetch:1,
					 tx_convert:1,
					 cond_modify:1,
					 /* conf_fetch split to multiple sub-tasks */
					 cond_fetch_split:1,
					 reintegrating:1,
					 tx_renew:1,
					 rebuilding:1,
					 for_migrate:1,
					 req_dup_sgl:1;
	/* request flags. currently only: ORF_RESEND */
	uint32_t			 specified_shard;
	uint32_t			 flags;
	uint16_t			 retry_cnt;
	uint16_t			 inprogress_cnt;
	/* Last timestamp (in second) when report retry warning message. */
	uint32_t                         retry_warn_ts;
	struct obj_req_tgts		 req_tgts;
	d_sg_list_t			*sgls_dup;
	crt_bulk_t			*bulks;
	uint32_t			 iod_nr;
	uint32_t			 initial_shard;
	d_list_t			 shard_task_head;
	struct obj_reasb_req		 reasb_req;
	struct obj_auxi_tgt_list	*failed_tgt_list;
	uint64_t			 dkey_hash;
	/* one shard_args embedded to save one memory allocation if the obj
	 * request only targets for one shard.
	 */
	union {
		struct shard_rw_args		rw_args;
		struct shard_punch_args		p_args;
		struct shard_list_args		l_args;
		struct shard_k2a_args		k_args;
		struct shard_sync_args		s_args;
		struct coll_query_args		cq_args;
	};
};

/**
 * task memory space should enough to use -
 * obj API task with daos_task_args + obj_auxi_args,
 * shard sub-task with shard_auxi_args + obj_auxi_args.
 * When it exceed the limit, can reduce OBJ_TGT_INLINE_NR or enlarge tse_task.
 */
D_CASSERT(sizeof(struct obj_auxi_args) + sizeof(struct shard_auxi_args) <=
	  TSE_TASK_ARG_LEN);
D_CASSERT(sizeof(struct obj_auxi_args) + sizeof(struct daos_task_args) <=
	  TSE_TASK_ARG_LEN);


typedef int (*obj_enum_process_cb_t)(daos_key_desc_t *kds, void *ptr,
				     unsigned int size, void *arg);
int
obj_enum_iterate(daos_key_desc_t *kdss, d_sg_list_t *sgl, int nr,
		 unsigned int type, obj_enum_process_cb_t cb,
		 void *cb_arg);
#define CLI_OBJ_IO_PARMS	8

int
merge_recx(d_list_t *head, uint64_t offset, uint64_t size, daos_epoch_t eph,
	   uint64_t boundary);

struct ec_bulk_spec {
	uint64_t is_skip:	1;
	uint64_t len:		63;
};
D_CASSERT(sizeof(struct ec_bulk_spec) == sizeof(uint64_t));

static inline void
ec_bulk_spec_set(uint64_t len, bool skip, int index,
		 struct ec_bulk_spec **skip_list)
{
	(*skip_list)[index].is_skip = skip;
	(*skip_list)[index].len = len;
}

static inline uint64_t
ec_bulk_spec_get_len(int index, struct ec_bulk_spec *skip_list)
{
	return skip_list[index].len;
}

static inline bool
ec_bulk_spec_get_skip(int index, struct ec_bulk_spec *skip_list)
{
	return skip_list[index].is_skip;
}

static inline bool
is_ec_data_shard(struct dc_object *obj, uint64_t dkey_hash, uint32_t shard)
{
	D_ASSERT(daos_oclass_is_ec(&obj->cob_oca));
	return obj_ec_shard_off(obj, dkey_hash, shard) < obj_ec_data_tgt_nr(&obj->cob_oca);
}

static inline bool
is_ec_parity_shard(struct dc_object *obj, uint64_t dkey_hash, uint32_t shard)
{
	D_ASSERT(daos_oclass_is_ec(&obj->cob_oca));
	return obj_ec_shard_off(obj, dkey_hash, shard) >= obj_ec_data_tgt_nr(&obj->cob_oca);
}

static inline bool
daos_obj_id_is_ec(daos_obj_id_t oid)
{
	return daos_obj_id2ord(oid) >= OR_RS_2P1 && daos_obj_id2ord(oid) <= OR_RS_16P3;
}

#define obj_ec_parity_rotate_enabled(obj)	(obj->cob_layout_version > 0)
#define obj_ec_parity_rotate_enabled_by_version(layout_ver)	(layout_ver > 0)
#define DOVA_NUM	32
#define DOVA_BUF_LEN	4096

struct dc_obj_verify_cursor {
	daos_key_t		 dkey;
	daos_iod_t		 iod;
	daos_recx_t		 recx;
	uint32_t		 gen;
	uint32_t		 type;
	uint32_t		 kds_idx;
	uint32_t		 iod_off;
	void			*ptr;
};

struct dc_obj_verify_args {
	daos_handle_t			 oh;
	daos_handle_t			 th;
	daos_size_t			 size;
	uint32_t			 num;
	unsigned int			 eof:1,
					 non_exist:1,
					 data_fetched:1;
	daos_key_desc_t			 kds[DOVA_NUM];
	d_sg_list_t			 list_sgl;
	d_sg_list_t			 fetch_sgl;
	daos_anchor_t			 anchor;
	daos_anchor_t			 dkey_anchor;
	daos_anchor_t			 akey_anchor;
	d_iov_t				 list_iov;
	d_iov_t				 fetch_iov;
	daos_size_t			 list_buf_len;
	daos_size_t			 fetch_buf_len;
	char				*list_buf;
	char				*fetch_buf;
	char				 inline_buf[DOVA_BUF_LEN];
	uint32_t			 current_shard;
	struct dc_obj_verify_cursor	 cursor;
};

/*
 * Report latency on a per-I/O size.
 * Buckets starts at [0; 256B[ and are increased by power of 2
 * (i.e. [256B; 512B[, [512B; 1KB[) up to [4MB; infinity[
 * Since 4MB = 2^22 and 256B = 2^8, this means
 * (22 - 8 + 1) = 15 buckets plus the 4MB+ bucket, so
 * 16 buckets in total.
 */
#define NR_LATENCY_BUCKETS 16

struct dc_obj_tls {
	/** Measure update/fetch latency based on I/O size (type = gauge) */
	struct d_tm_node_t *cot_update_lat[NR_LATENCY_BUCKETS];
	struct d_tm_node_t *cot_fetch_lat[NR_LATENCY_BUCKETS];

	/** Measure per-operation latency in us (type = gauge) */
	struct d_tm_node_t *cot_op_lat[OBJ_PROTO_CLI_COUNT];
	/** Count number of per-opcode active requests (type = gauge) */
	struct d_tm_node_t *cot_op_active[OBJ_PROTO_CLI_COUNT];
};

int
obj_latency_tm_init(uint32_t opc, int tgt_id, struct d_tm_node_t **tm, char *op, char *desc,
		    bool server);
extern struct daos_module_key dc_obj_module_key;

static inline struct dc_obj_tls *
dc_obj_tls_get()
{
	struct daos_thread_local_storage *dtls;

	dtls = dc_tls_get(dc_obj_module_key.dmk_tags);
	D_ASSERT(dtls != NULL);
	return daos_module_key_get(dtls, &dc_obj_module_key);
}

struct obj_pool_metrics {
	/** Count number of total per-opcode requests (type = counter) */
	struct d_tm_node_t *opm_total[OBJ_PROTO_CLI_COUNT];
	/** Total number of bytes fetched (type = counter) */
	struct d_tm_node_t *opm_fetch_bytes;
	/** Total number of bytes updated (type = counter) */
	struct d_tm_node_t *opm_update_bytes;

	/** Total number of silently restarted updates (type = counter) */
	struct d_tm_node_t *opm_update_restart;
	/** Total number of resent update operations (type = counter) */
	struct d_tm_node_t *opm_update_resent;
	/** Total number of retry update operations (type = counter) */
	struct d_tm_node_t *opm_update_retry;
	/** Total number of EC full-stripe update operations (type = counter) */
	struct d_tm_node_t *opm_update_ec_full;
	/** Total number of EC partial update operations (type = counter) */
	struct d_tm_node_t *opm_update_ec_partial;
	/** Total number of EC agg conflicts with VOS aggregation or discard */
	struct d_tm_node_t *opm_ec_agg_blocked;
};

void
obj_metrics_free(void *data);
int
obj_metrics_count(void);
void *
obj_metrics_alloc_internal(const char *path, int tgt_id, bool server);

static inline unsigned int
lat_bucket(uint64_t size)
{
	int nr;

	if (size <= 256)
		return 0;

	/** return number of leading zero-bits */
	nr = __builtin_clzl(size - 1);

	/** >4MB, return last bucket */
	if (nr < 42)
		return NR_LATENCY_BUCKETS - 1;

	return 56 - nr;
}

static inline int
dc_cont2uuid(struct dc_cont *dc_cont, uuid_t *hdl_uuid, uuid_t *uuid)
{
	if (!dc_cont)
		return -DER_NO_HDL;

	if (hdl_uuid != NULL)
		uuid_copy(*hdl_uuid, dc_cont->dc_cont_hdl);
	if (uuid != NULL)
		uuid_copy(*uuid, dc_cont->dc_uuid);
	return 0;
}

int dc_set_oclass(uint32_t rf, int domain_nr, int target_nr, enum daos_otype_t otype,
		  daos_oclass_hints_t hints, enum daos_obj_redun *ord, uint32_t *nr);


int dc_obj_shard_open(struct dc_object *obj, daos_unit_oid_t id,
		      unsigned int mode, struct dc_obj_shard *shard);
void dc_obj_shard_close(struct dc_obj_shard *shard);

int dc_obj_shard_rw(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
		    void *shard_args, struct daos_shard_tgt *fw_shard_tgts,
		    uint32_t fw_cnt, tse_task_t *task);

int
ec_obj_update_encode(tse_task_t *task, daos_obj_id_t oid,
		     struct daos_oclass_attr *oca, uint64_t *tgt_set);

int dc_obj_shard_punch(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
		       void *shard_args, struct daos_shard_tgt *fw_shard_tgts,
		       uint32_t fw_cnt, tse_task_t *task);

int dc_obj_shard_coll_punch(struct dc_obj_shard *shard, struct shard_punch_args *args,
			    struct dtx_memberships *mbs, uint32_t mbs_max_size, crt_bulk_t *bulks,
			    uint32_t bulk_sz, struct daos_coll_target *tgts, uint32_t tgt_nr,
			    uint32_t max_tgt_size, struct dtx_epoch *epoch, uint64_t api_flags,
			    uint32_t rpc_flags, uint32_t map_ver, uint32_t *rep_ver,
			    tse_task_t *task);

int dc_obj_shard_list(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
		      void *shard_args, struct daos_shard_tgt *fw_shard_tgts,
		      uint32_t fw_cnt, tse_task_t *task);

int dc_obj_shard_key2anchor(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
			    void *shard_args, struct daos_shard_tgt *fw_shard_tgts,
			    uint32_t fw_cnt, tse_task_t *task);

int dc_obj_shard_query_key(struct dc_obj_shard *shard, struct dtx_epoch *epoch, uint32_t flags,
			   uint32_t req_map_ver, struct dc_object *obj,
			   daos_key_t *dkey, daos_key_t *akey, daos_recx_t *recx,
			   daos_epoch_t *max_epoch, const uuid_t coh_uuid, const uuid_t cont_uuid,
			   struct dtx_id *dti, uint32_t *map_ver, daos_handle_t th,
			   tse_task_t *task, uint32_t *max_delay, uint64_t *queue_id);

int dc_obj_shard_coll_query(struct dc_obj_shard *shard, struct dtx_epoch *epoch, uint32_t flags,
			    uint32_t req_map_ver, struct dc_object *obj, daos_key_t *dkey,
			    daos_key_t *akey, daos_recx_t *recx, daos_epoch_t *max_epoch,
			    const uuid_t coh_uuid, const uuid_t cont_uuid, struct dtx_id *dti,
			    uint32_t *map_ver, struct daos_coll_target *tgts, uint32_t tgt_nr,
			    uint32_t max_tgt_size, uint32_t disp_width, daos_handle_t th,
			    tse_task_t *task, uint32_t *max_delay, uint64_t *queue_id);

int dc_obj_shard_sync(struct dc_obj_shard *shard, enum obj_rpc_opc opc,
		      void *shard_args, struct daos_shard_tgt *fw_shard_tgts,
		      uint32_t fw_cnt, tse_task_t *task);

int dc_obj_verify_rdg(struct dc_object *obj, struct dc_obj_verify_args *dova,
		      uint32_t rdg_idx, uint32_t reps, daos_epoch_t epoch);
bool obj_op_is_ec_fetch(struct obj_auxi_args *obj_auxi);
int obj_recx_ec2_daos(struct daos_oclass_attr *oca, uint32_t tgt_off,
		      daos_recx_t **recxs_p, daos_epoch_t **recx_ephs_p,
		      unsigned int *nr, bool convert_parity);
int obj_reasb_req_init(struct obj_reasb_req *reasb_req, struct dc_object *obj,
		       daos_iod_t *iods, uint32_t iod_nr);
void obj_reasb_req_fini(struct obj_reasb_req *reasb_req, uint32_t iod_nr);
int obj_bulk_prep(d_sg_list_t *sgls, unsigned int nr, bool bulk_bind,
		  crt_bulk_perm_t bulk_perm, tse_task_t *task,
		  crt_bulk_t **p_bulks);
struct daos_oclass_attr *obj_get_oca(struct dc_object *obj);
bool obj_is_ec(struct dc_object *obj);
int obj_get_replicas(struct dc_object *obj);
int obj_shard_open(struct dc_object *obj, unsigned int shard,
		   unsigned int map_ver, struct dc_obj_shard **shard_ptr);
int obj_dkey2grpidx(struct dc_object *obj, uint64_t hash, unsigned int map_ver);
int obj_pool_query_task(tse_sched_t *sched, struct dc_object *obj,
			unsigned int map_ver, tse_task_t **taskp);
bool obj_csum_dedup_candidate(struct cont_props *props, daos_iod_t *iods,
			      uint32_t iod_nr);

int queue_shard_query_key_task(tse_task_t *api_task, struct obj_auxi_args *obj_auxi,
			       struct dtx_epoch *epoch, int shard, unsigned int map_ver,
			       struct dc_object *obj, struct dtx_id *dti, uuid_t coh_uuid,
			       uuid_t cont_uuid, struct daos_coll_target *dcts, uint32_t dct_nr);

int obj_grp_leader_get(struct dc_object *obj, int grp_idx, uint64_t dkey_hash,
		       bool has_condition, unsigned int map_ver, uint8_t *bit_map);
#define obj_shard_close(shard)	dc_obj_shard_close(shard)
int obj_recx_ec_daos2shard(struct daos_oclass_attr *oca, uint32_t tgt_off,
			   daos_recx_t **recxs_p, daos_epoch_t **recx_ephs_p,
			   unsigned int *iod_nr);
int obj_ec_singv_encode_buf(daos_unit_oid_t oid, uint16_t layout_ver, struct daos_oclass_attr *oca,
			    uint64_t dkey_hash, daos_iod_t *iod, d_sg_list_t *sgl,
			    d_iov_t *e_iov);
int obj_ec_singv_split(daos_unit_oid_t oid, uint16_t layout_ver, struct daos_oclass_attr *oca,
		       uint64_t dkey_hash, daos_size_t iod_size, d_sg_list_t *sgl);

int
obj_singv_ec_rw_filter(daos_unit_oid_t oid, struct daos_oclass_attr *oca,
		       uint32_t tgt_off, daos_iod_t *iods, uint64_t *offs,
		       daos_epoch_t epoch, uint32_t flags, uint32_t nr,
		       bool for_update, bool deg_fetch,
		       struct daos_recx_ep_list **recov_lists_ptr);
int
obj_ec_encode_buf(daos_obj_id_t oid, struct daos_oclass_attr *oca,
		  daos_size_t iod_size, unsigned char *buffer,
		  unsigned char *p_bufs[]);

int
obj_ec_parity_alive(daos_handle_t oh, uint64_t dkey_hash, uint32_t *shard);

static inline struct pl_obj_shard*
obj_get_shard(void *data, int idx)
{
	struct dc_object	*obj = data;

	return &obj->cob_shards->do_shards[idx].do_pl_shard;
}

static inline bool
obj_retry_error(int err)
{
	return err == -DER_TIMEDOUT || err == -DER_STALE || err == -DER_INPROGRESS ||
	       err == -DER_GRPVER || err == -DER_EXCLUDED || err == -DER_CSUM ||
	       err == -DER_TX_BUSY || err == -DER_TX_UNCERTAIN || err == -DER_NEED_TX ||
	       err == -DER_NOTLEADER || err == -DER_UPDATE_AGAIN || err == -DER_NVME_IO ||
	       err == -DER_CHKPT_BUSY || err == -DER_OVERLOAD_RETRY || daos_crt_network_error(err);
}

static inline bool
obj_retriable_migrate(int err)
{
	return err == -DER_CSUM || err == -DER_NVME_IO;
}

static inline daos_handle_t
obj_ptr2hdl(struct dc_object *obj)
{
	daos_handle_t oh;

	daos_hhash_link_key(&obj->cob_hlink, &oh.cookie);
	return oh;
}

static inline int
shard_task_abort(tse_task_t *task, void *arg)
{
	int	rc = *((int *)arg);

	tse_task_list_del(task);
	tse_task_complete(task, rc);

	tse_task_decref(task);
	return 0;
}

static inline void
dc_io_epoch_set(struct dtx_epoch *epoch, uint32_t opc)
{
	epoch->oe_value = DAOS_EPOCH_MAX;
	epoch->oe_first = epoch->oe_value;
	epoch->oe_flags = 0;
}

static inline void
dc_sgl_out_set(d_sg_list_t *sgl, daos_size_t data_size)
{
	d_iov_t		*iov;
	daos_size_t	 buf_size;
	uint32_t	 i;

	if (data_size == 0) {
		sgl->sg_nr_out = 0;
		return;
	}
	buf_size = 0;
	for (i = 0; i < sgl->sg_nr; i++) {
		iov = &sgl->sg_iovs[i];
		buf_size += iov->iov_buf_len;
		if (buf_size < data_size) {
			iov->iov_len = iov->iov_buf_len;
			sgl->sg_nr_out = i + 1;
			continue;
		}

		iov->iov_len = iov->iov_buf_len -
			       (buf_size - data_size);
		sgl->sg_nr_out = i + 1;
		break;
	}
}

void obj_shard_decref(struct dc_obj_shard *shard);
void obj_shard_addref(struct dc_obj_shard *shard);
struct dc_object *obj_addref(struct dc_object *obj);
void obj_decref(struct dc_object *obj);
int obj_get_grp_size(struct dc_object *obj);
struct dc_object *obj_hdl2ptr(daos_handle_t oh);
uint32_t
dc_obj_retry_delay(tse_task_t *task, uint32_t opc, int err, uint16_t *retry_cnt,
		   uint16_t *inprogress_cnt, uint32_t timeout_secs);

/* handles, pointers for handling I/O */
struct obj_io_context {
	struct ds_cont_hdl	*ioc_coh;
	struct ds_cont_child	*ioc_coc;
	crt_rpc_t		*ioc_rpc;
	struct daos_oclass_attr	 ioc_oca;
	daos_handle_t		 ioc_vos_coh;
	uint32_t		 ioc_layout_ver;
	uint32_t		 ioc_map_ver;
	uint32_t		 ioc_opc;
	uint64_t		 ioc_start_time;
	uint64_t		 ioc_io_size;
	uint32_t		 ioc_began:1,
				 ioc_update_ec_ts:1,
				 ioc_free_sgls:1,
				 ioc_lost_reply:1,
				 ioc_fetch_snap:1;
};

static inline void
obj_ptr2shards(struct dc_object *obj, uint32_t *start_shard, uint32_t *shard_nr,
	       uint32_t *grp_nr)
{
	*start_shard = 0;
	*shard_nr = obj->cob_shards_nr;
	*grp_nr = obj->cob_shards_nr / obj_get_grp_size(obj);

	D_ASSERTF(*grp_nr == obj->cob_grp_nr, "Unmatched grp nr for "DF_OID": %u/%u\n",
		  DP_OID(obj->cob_md.omd_id), *grp_nr, obj->cob_grp_nr);
}

static inline uint64_t
obj_dkey2hash(daos_obj_id_t oid, daos_key_t *dkey)
{
	/* return 0 for NULL dkey, for example obj punch and list dkey */
	if (dkey == NULL)
		return 0;

	if (daos_is_dkey_uint64(oid))
		return *(uint64_t *)dkey->iov_buf;

	return d_hash_murmur64((unsigned char *)dkey->iov_buf,
			       dkey->iov_len, 5731);
}

static inline int
recx_compare(const void *rank1, const void *rank2)
{
	const daos_recx_t *r1 = rank1;
	const daos_recx_t *r2 = rank2;

	D_ASSERT(r1 != NULL && r2 != NULL);
	if (r1->rx_idx < r2->rx_idx)
		return -1;
	else if (r1->rx_idx == r2->rx_idx)
		return 0;
	else /** r1->rx_idx < r2->rx_idx */
		return 1;
}

static inline void
daos_iom_sort(daos_iom_t *map)
{
	if (map == NULL)
		return;
	qsort(map->iom_recxs, map->iom_nr_out,
	      sizeof(*map->iom_recxs), recx_compare);
}

static inline void
daos_iom_dump(daos_iom_t *iom)
{
	uint32_t	i;

	if (iom == NULL)
		return;

	if (iom->iom_type == DAOS_IOD_ARRAY)
		D_PRINT("iom_type array\n");
	else if (iom->iom_type == DAOS_IOD_SINGLE)
		D_PRINT("iom_type single\n");
	else
		D_PRINT("iom_type bad (%d)\n", iom->iom_type);

	D_PRINT("iom_nr %d, iom_nr_out %d, iom_flags %d\n",
		iom->iom_nr, iom->iom_nr_out, iom->iom_flags);
	D_PRINT("iom_size "DF_U64"\n", iom->iom_size);
	D_PRINT("iom_recx_lo - "DF_RECX"\n", DP_RECX(iom->iom_recx_lo));
	D_PRINT("iom_recx_hi - "DF_RECX"\n", DP_RECX(iom->iom_recx_hi));

	if (iom->iom_recxs == NULL) {
		D_PRINT("NULL iom_recxs array\n");
		return;
	}

	D_PRINT("iom_recxs array -\n");
	for (i = 0; i < iom->iom_nr_out; i++) {
		D_PRINT("[%d] "DF_RECX" ", i, DP_RECX(iom->iom_recxs[i]));
		if (i % 8 == 7)
			D_PRINT("\n");
	}
	D_PRINT("\n");
}

static inline void
daos_recx_ep_list_set(struct daos_recx_ep_list *lists, unsigned int nr,
		      daos_epoch_t epoch, bool snapshot)
{
	struct daos_recx_ep_list	*list;
	struct daos_recx_ep		*recx_ep;
	unsigned int			 i, j;

	for (i = 0; i < nr; i++) {
		list = &lists[i];
		list->re_ep_valid = 1;
		if (epoch == 0)
			continue;
		if (snapshot)
			list->re_snapshot = 1;
		for (j = 0; j < list->re_nr; j++) {
			recx_ep = &list->re_items[j];
			if (snapshot)
				recx_ep->re_ep = epoch;
			else
				recx_ep->re_ep = max(recx_ep->re_ep, epoch);
		}
	}
}

static inline bool
daos_recx_ep_list_ep_valid(struct daos_recx_ep_list *list)
{
	return (list->re_ep_valid == 1);
}

int obj_class_init(void);
void obj_class_fini(void);

/*
 * Consider efficiency, we will not make one leader (or relay) engine to forward
 * too many collective requests to other engines. But it also needs to guarantee
 * that the payload size for each dispatch group is small enough to be packed in
 * RPC body to avoid transferring via RDAM.
 *
 * On the other hand, parent engine may need to children's feedback before reply
 * parent's upper level engine. So making parent engine to forward more requests
 * than each child engine does is more efficient because current collective task
 * on parent engine is scheduled earlier than on child engine. Otherwise, parent
 * engine may wait more time.
 */
#define COLL_DISP_WIDTH_DEF	20
#define COLL_DISP_WIDTH_MIN	8
#define COLL_DISP_WIDTH_DIF	4

#define OBJ_COLL_THD_MIN	COLL_DISP_WIDTH_DEF
#define COLL_BTREE_ORDER	COLL_DISP_WIDTH_DEF

#define IOV_FRAG_SIZE_DEF       4095
#define IOV_FRAG_COUNT_DEF      128
#define IOV_FRAG_COUNT_MIN      16

static inline void
obj_init_iov_fragment_params()
{
	d_getenv_uint("DAOS_IOV_FRAG_SIZE", &iov_frag_size);
	d_getenv_uint("DAOS_IOV_FRAG_COUNT", &iov_frag_count);
	if (iov_frag_count < IOV_FRAG_COUNT_MIN) {
		D_WARN("Invalid IOV fragment count threshold: %u (minimum %u). Using default: %u\n",
		       iov_frag_count, IOV_FRAG_COUNT_MIN, IOV_FRAG_COUNT_DEF);
		iov_frag_count = IOV_FRAG_COUNT_DEF;
	}
	if (iov_frag_size == 0)
		D_INFO("IOV fragment merging is disabled. Fragment count threshold: %u\n",
		       iov_frag_count);
	else
		D_INFO("IOV fragment merging enabled (size threshold: %u, count threshold: %u)\n",
		       iov_frag_size, iov_frag_count);
}

struct obj_query_merge_args {
	struct daos_oclass_attr	*oqma_oca;
	daos_unit_oid_t		 oqma_oid;
	daos_epoch_t		 oqma_src_epoch;
	daos_key_t		*oqma_in_dkey;
	daos_key_t		*oqma_src_dkey;
	daos_key_t		*oqma_tgt_dkey; /* output */
	daos_key_t		*oqma_src_akey;
	daos_key_t		*oqma_tgt_akey; /* output */
	daos_recx_t		*oqma_src_recx;
	daos_recx_t		*oqma_tgt_recx; /* output */
	daos_epoch_t		*oqma_tgt_epoch; /* output */
	uint32_t		*oqma_tgt_map_ver; /* output */
	uint32_t		*oqma_shard; /* output */
	uint32_t		*oqma_max_delay; /* output */
	uint64_t		*oqma_queue_id; /* output */
	crt_rpc_t		*oqma_rpc;
	uint64_t		 oqma_flags;
	uint32_t		 oqma_opc;
	uint32_t		 oqma_src_map_ver;
	int			 oqma_ret;
	uint32_t		 oqma_raw_recx:1;
};

/* obj_utils.c */
void obj_ec_recx_vos2daos(struct daos_oclass_attr *oca, daos_unit_oid_t oid, daos_key_t *dkey,
			  daos_recx_t *recx, bool get_max);
int daos_obj_query_merge(struct obj_query_merge_args *oqma);
void obj_coll_disp_init(uint32_t tgt_nr, uint32_t max_tgt_size, uint32_t inline_size,
			uint32_t start, uint32_t max_width, struct obj_coll_disp_cursor *ocdc);
void obj_coll_disp_dest(struct obj_coll_disp_cursor *ocdc, struct daos_coll_target *tgts,
			crt_endpoint_t *tgt_ep, daos_obj_id_t oid);
void obj_coll_disp_move(struct obj_coll_disp_cursor *ocdc);
int obj_utils_init(void);
void obj_utils_fini(void);

/* obj_tx.c */
int
dc_tx_check_pmv(daos_handle_t th);

int
dc_tx_hdl2epoch_and_pmv(daos_handle_t th, struct dtx_epoch *epoch,
			uint32_t *pmv);

/* cli_coll.c */
bool
obj_need_coll(struct dc_object *obj, uint32_t *start_shard, uint32_t *shard_nr,
	      uint32_t *grp_nr);

int
obj_coll_oper_args_init(struct coll_oper_args *coa, struct dc_object *obj, bool for_modify);

void
obj_coll_oper_args_fini(struct coll_oper_args *coa);

int
obj_coll_prep_one(struct coll_oper_args *coa, struct dc_object *obj,
		  uint32_t map_ver, uint32_t idx);

int
dc_obj_coll_punch(tse_task_t *task, struct dc_object *obj, struct dtx_epoch *epoch,
		  uint32_t map_ver, daos_obj_punch_t *args, struct obj_auxi_args *auxi);

int
queue_coll_query_task(tse_task_t *api_task, struct obj_auxi_args *obj_auxi, struct dc_object *obj,
		      struct dtx_id *xid, struct dtx_epoch *epoch, uint32_t map_ver);

/** See dc_tx_get_epoch. */
enum dc_tx_get_epoch_rc {
	DC_TX_GE_CHOSEN,
	DC_TX_GE_CHOOSING,
	DC_TX_GE_REINITED
};

int
dc_tx_get_epoch(tse_task_t *task, daos_handle_t th, struct dtx_epoch *epoch);

int
dc_tx_op_end(tse_task_t *task, daos_handle_t th, struct dtx_epoch *req_epoch,
	     int rep_rc, daos_epoch_t rep_epoch);

int
dc_tx_get_dti(daos_handle_t th, struct dtx_id *dti);

int
dc_tx_attach(daos_handle_t th, struct dc_object *obj, enum obj_rpc_opc opc, tse_task_t *task,
	     uint32_t backoff, bool comp);

int
dc_tx_convert(struct dc_object *obj, enum obj_rpc_opc opc, tse_task_t *task);

int
iov_alloc_for_csum_info(d_iov_t *iov, struct dcs_csum_info *csum_info);

/* obj_layout.c */
int
obj_pl_grp_idx(uint32_t layout_gl_ver, uint64_t hash, uint32_t grp_nr);

int
obj_pl_place(struct pl_map *map, uint16_t layout_ver, struct daos_obj_md *md,
	     unsigned int mode, struct daos_obj_shard_md *shard_md,
	     struct pl_obj_layout **layout_pp);

#endif /* __DAOS_OBJ_INTENRAL_H__ */
