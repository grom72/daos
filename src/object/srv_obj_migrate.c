/**
 * (C) Copyright 2019-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * migrate: migrate objects between servers.
 *
 */
#define D_LOGFAC	DD_FAC(server)

#include <daos_srv/pool.h>
#include <daos/btree_class.h>
#include <daos/pool_map.h>
#include <daos/rpc.h>
#include <daos/object.h>
#include <daos/container.h>
#include <daos/pool.h>
#include <daos_srv/container.h>
#include <daos_srv/daos_engine.h>
#include <daos_srv/vos.h>
#include <daos_srv/dtx_srv.h>
#include <daos_srv/srv_csum.h>
#include <daos_srv/rebuild.h>
#include <daos_srv/object.h>
#include "obj_rpc.h"
#include "srv_internal.h"

#if D_HAS_WARNING(4, "-Wframe-larger-than=")
	#pragma GCC diagnostic ignored "-Wframe-larger-than="
#endif

/* Max in-flight data size per xstream */
/* Set the total in-flight size to be 25% of MAX DMA size for
 * the moment, will adjust it later if needed.
 */
#define MIGRATE_MAX_SIZE	(1 << 28)
/* Max migrate ULT number on the server */
#define MIGRATE_DEFAULT_MAX_ULT	4096
#define ENV_MIGRATE_ULT_CNT	"D_MIGRATE_ULT_CNT"
struct migrate_one {
	daos_key_t		 mo_dkey;
	uint64_t		 mo_dkey_hash;
	uuid_t			 mo_pool_uuid;
	uuid_t			 mo_cont_uuid;
	daos_unit_oid_t		 mo_oid;
	daos_epoch_t		 mo_obj_punch_eph;
	daos_epoch_t		 mo_dkey_punch_eph;

	/* minimum epoch from mo_iods & mo_iods_from_parity, used
	 * as the updated epoch for replication extent rebuild.
	*/
	daos_epoch_t             mo_min_epoch;
	daos_epoch_t             mo_epoch;

	/* Epochs per recx in mo_iods, used for parity extent rebuild. */
	daos_epoch_t           **mo_iods_update_ephs;

	/* IOD for replicate recxs migration */
	daos_iod_t		*mo_iods;

	/* IOD for recxs gotten from parity rebuild. During EC rebuild, it
	 * will first rebuild mo_iods_from_parity then rebuild mo_iods to
	 * avoid parity corruption.
	*/
	daos_iod_t		*mo_iods_from_parity;
	daos_epoch_t		**mo_iods_update_ephs_from_parity;


	daos_iod_t		*mo_punch_iods;

	daos_epoch_t		*mo_akey_punch_ephs;
	daos_epoch_t		 mo_rec_punch_eph;

	struct dcs_iod_csums	*mo_iods_csums;
	d_sg_list_t		*mo_sgls;
	struct daos_oclass_attr	 mo_oca;
	unsigned int		 mo_iod_num;
	unsigned int		 mo_punch_iod_num;
	unsigned int		 mo_iod_alloc_num;
	unsigned int		 mo_rec_num;
	uint64_t		 mo_size;
	uint64_t		 mo_version;
	uint32_t		 mo_pool_tls_version;
	uint32_t		 mo_iods_num_from_parity;
	uint32_t		 mo_layout_version;
	uint32_t		 mo_generation;
	d_list_t		 mo_list;
	d_iov_t			 mo_csum_iov;
	uint32_t                  mo_opc;
};

struct migrate_obj_key {
	daos_unit_oid_t oid;
	daos_epoch_t	eph;
	uint32_t	tgt_idx;
};

/* Argument for container iteration and migrate */
struct iter_cont_arg {
	struct migrate_pool_tls *pool_tls;
	uuid_t			pool_uuid;
	uuid_t			pool_hdl_uuid;
	uuid_t			cont_uuid;
	uuid_t			cont_hdl_uuid;
	struct tree_cache_root	*cont_root;
	unsigned int		yield_freq;
	uint64_t		*snaps;
	uint32_t		snap_cnt;
	uint32_t		version;
	uint32_t		ref_cnt;
};

/* Argument for object iteration and migrate */
struct iter_obj_arg {
	uuid_t			pool_uuid;
	uuid_t			cont_uuid;
	daos_unit_oid_t		oid;
	daos_epoch_t		epoch;
	daos_epoch_t		punched_epoch;
	unsigned int		shard;
	unsigned int		tgt_idx;
	uint64_t		*snaps;
	uint32_t		snap_cnt;
	uint32_t		version;
	uint32_t		generation;
};

static int
obj_tree_destory_cb(daos_handle_t ih, d_iov_t *key_iov,
		    d_iov_t *val_iov, void *data)
{
	struct tree_cache_root *root = val_iov->iov_buf;
	int			rc;

	rc = dbtree_destroy(root->tcr_root_hdl, NULL);
	if (rc)
		D_ERROR("dbtree_destroy, cont "DF_UUID" failed: "DF_RC"\n",
			DP_UUID(*(uuid_t *)key_iov->iov_buf), DP_RC(rc));
	return rc;
}

int
obj_tree_destroy(daos_handle_t btr_hdl)
{
	int	rc;

	rc = dbtree_iterate(btr_hdl, DAOS_INTENT_PUNCH, false,
			    obj_tree_destory_cb, NULL);
	if (rc) {
		D_ERROR("dbtree iterate failed: "DF_RC"\n", DP_RC(rc));
		goto out;
	}

	rc = dbtree_destroy(btr_hdl, NULL);

out:
	return rc;
}

static int
obj_tree_create(daos_handle_t toh, void *key, size_t key_size,
		uint32_t class, uint64_t feats, struct tree_cache_root **rootp)
{
	d_iov_t			key_iov;
	d_iov_t			val_iov;
	struct umem_attr	uma;
	struct tree_cache_root	root = { 0 };
	struct tree_cache_root	*tmp_root;
	int			rc, rc2;

	d_iov_set(&key_iov, key, key_size);
	d_iov_set(&val_iov, &root, sizeof(root));
	rc = dbtree_update(toh, &key_iov, &val_iov);
	if (rc)
		return rc;

	d_iov_set(&val_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &val_iov);
	if (rc)
		D_GOTO(out, rc);

	tmp_root = val_iov.iov_buf;

	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;
	rc = dbtree_create_inplace(class, feats, 32, &uma, &tmp_root->tcr_btr_root,
				   &tmp_root->tcr_root_hdl);
	if (rc) {
		D_ERROR("failed to create rebuild tree: "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	*rootp = tmp_root;
out:
	if (rc < 0) {
		rc2 = dbtree_delete(toh, BTR_PROBE_EQ, &key_iov, NULL);
		if (rc2)
			D_WARN("failed to delete "DF_KEY": "DF_RC"\n",
			       DP_KEY(&key_iov), DP_RC(rc2));
	}
	return rc;
}

int
obj_tree_lookup(daos_handle_t toh, uuid_t co_uuid, daos_unit_oid_t oid,
		d_iov_t *val_iov)
{
	struct tree_cache_root	*cont_root = NULL;
	d_iov_t			key_iov;
	d_iov_t			tmp_iov;
	int			rc;

	/* locate the container first */
	d_iov_set(&key_iov, co_uuid, sizeof(uuid_t));
	d_iov_set(&tmp_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &tmp_iov);
	if (rc < 0) {
		if (rc != -DER_NONEXIST)
			D_ERROR("lookup cont "DF_UUID" failed, "DF_RC"\n",
				DP_UUID(co_uuid), DP_RC(rc));
		else
			D_DEBUG(DB_TRACE, "Container "DF_UUID" not exist\n",
				DP_UUID(co_uuid));
		return rc;
	}

	cont_root = tmp_iov.iov_buf;
	/* Then try to lookup the object under the container */
	d_iov_set(&key_iov, &oid, sizeof(oid));
	rc = dbtree_lookup(cont_root->tcr_root_hdl, &key_iov, val_iov);
	if (rc < 0) {
		if (rc != -DER_NONEXIST)
			D_ERROR(DF_UUID"/"DF_UOID" "DF_RC"\n",
				DP_UUID(co_uuid), DP_UOID(oid), DP_RC(rc));
		else
			D_DEBUG(DB_TRACE, DF_UUID"/"DF_UOID " not exist\n",
				DP_UUID(co_uuid), DP_UOID(oid));
	}

	return rc;
}

int
obj_tree_insert(daos_handle_t toh, uuid_t co_uuid, uint64_t tgt_id, daos_unit_oid_t oid,
		d_iov_t *val_iov)
{
	struct tree_cache_root	*cont_root = NULL;
	d_iov_t			key_iov;
	d_iov_t			tmp_iov;
	int			rc;

	/* locate the container first */
	d_iov_set(&key_iov, co_uuid, sizeof(uuid_t));
	d_iov_set(&tmp_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &tmp_iov);
	if (rc < 0) {
		if (rc != -DER_NONEXIST) {
			D_ERROR("lookup cont "DF_UUID" failed: "DF_RC"\n",
				DP_UUID(co_uuid), DP_RC(rc));
			return rc;
		}

		D_DEBUG(DB_TRACE, "Create cont "DF_UUID" tree\n", DP_UUID(co_uuid));
		if (tgt_id != (uint64_t)(-1))
			rc = obj_tree_create(toh, co_uuid, sizeof(uuid_t), DBTREE_CLASS_IFV,
					     BTR_FEAT_UINT_KEY, &cont_root);
		else
			rc = obj_tree_create(toh, co_uuid, sizeof(uuid_t), DBTREE_CLASS_NV,
					     BTR_FEAT_DIRECT_KEY, &cont_root);
		if (rc) {
			D_ERROR("tree_create cont "DF_UUID" failed: "DF_RC"\n",
				DP_UUID(co_uuid), DP_RC(rc));
			return rc;
		}
	} else {
		cont_root = tmp_iov.iov_buf;
	}

	/* Then try to insert the object under the container */
	if (tgt_id != (uint64_t)(-1)) {
		d_iov_set(&key_iov, &tgt_id, sizeof(tgt_id));
		d_iov_set(&tmp_iov, NULL, 0);
		rc = dbtree_lookup(cont_root->tcr_root_hdl, &key_iov, &tmp_iov);
		if (rc < 0) {
			if (rc != -DER_NONEXIST) {
				D_ERROR("lookup tgt "DF_U64" failed: "DF_RC"\n",
					tgt_id, DP_RC(rc));
				return rc;
			}

			D_DEBUG(DB_TRACE, "Create tgt "DF_U64" tree\n", tgt_id);
			rc = obj_tree_create(cont_root->tcr_root_hdl, &tgt_id, sizeof(tgt_id),
					     DBTREE_CLASS_NV, BTR_FEAT_DIRECT_KEY, &cont_root);
			if (rc) {
				D_ERROR("tree_create tgt "DF_U64" failed: "DF_RC"\n",
					tgt_id, DP_RC(rc));
				return rc;
			}
		} else {
			cont_root = tmp_iov.iov_buf;
		}
	}

	/* Then try to insert the object under the container */
	d_iov_set(&key_iov, &oid, sizeof(oid));
	rc = dbtree_lookup(cont_root->tcr_root_hdl, &key_iov, val_iov);
	if (rc == 0) {
		D_DEBUG(DB_TRACE, DF_UOID "/" DF_UUID " already exists\n", DP_UOID(oid),
			DP_UUID(co_uuid));
		return -DER_EXIST;
	}

	rc = dbtree_update(cont_root->tcr_root_hdl, &key_iov, val_iov);
	if (rc < 0) {
		D_ERROR("failed to insert "DF_UOID": "DF_RC"\n",
			DP_UOID(oid), DP_RC(rc));
		return rc;
	}
	cont_root->tcr_count++;
	D_DEBUG(DB_TRACE, "insert "DF_UOID"/"DF_UUID"/"DF_U64" in"
		" root %p count %d\n", DP_UOID(oid),
		DP_UUID(co_uuid), tgt_id, cont_root, cont_root->tcr_count);

	return rc;
}

static int
migrate_cont_open(struct migrate_pool_tls *tls, uuid_t cont_uuid, unsigned int flag,
		  daos_handle_t *coh)
{
	struct migrate_cont_hdl *mch;
	int			rc;

	d_list_for_each_entry(mch, &tls->mpt_cont_hdl_list, mch_list) {
		if (uuid_compare(mch->mch_uuid, cont_uuid) == 0) {
			*coh = mch->mch_hdl;
			return 0;
		}
	}

	rc = dsc_cont_open(tls->mpt_pool_hdl, cont_uuid, tls->mpt_coh_uuid, flag, coh);
	if (rc) {
		D_ERROR("dsc_cont_open failed: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	D_ALLOC_PTR(mch);
	if (mch == NULL) {
		dsc_cont_close(tls->mpt_pool_hdl, *coh);
		*coh = DAOS_HDL_INVAL;
		return -DER_NOMEM;
	}

	d_list_add(&mch->mch_list, &tls->mpt_cont_hdl_list);
	uuid_copy(mch->mch_uuid, cont_uuid);
	mch->mch_hdl = *coh;

	return 0;
}

static void
migrate_cont_close_all(struct migrate_pool_tls *tls)
{
	struct migrate_cont_hdl *mch;
	struct migrate_cont_hdl *tmp;

	d_list_for_each_entry_safe(mch, tmp, &tls->mpt_cont_hdl_list, mch_list) {
		dsc_cont_close(tls->mpt_pool_hdl, mch->mch_hdl);
		d_list_del(&mch->mch_list);
		D_FREE(mch);
	}
}

void
migrate_pool_tls_destroy(struct migrate_pool_tls *tls)
{
	if (!tls)
		return;

	migrate_cont_close_all(tls);
	if (daos_handle_is_valid(tls->mpt_pool_hdl))
		dsc_pool_close(tls->mpt_pool_hdl);

	if (tls->mpt_obj_ult_cnts)
		D_FREE(tls->mpt_obj_ult_cnts);
	if (tls->mpt_dkey_ult_cnts)
		D_FREE(tls->mpt_dkey_ult_cnts);
	d_list_del(&tls->mpt_list);
	D_DEBUG(DB_REBUILD, DF_RB ": TLS destroy\n", DP_RB_MPT(tls));
	if (tls->mpt_pool)
		ds_pool_child_put(tls->mpt_pool);
	if (tls->mpt_svc_list.rl_ranks)
		D_FREE(tls->mpt_svc_list.rl_ranks);
	if (tls->mpt_done_eventual)
		ABT_eventual_free(&tls->mpt_done_eventual);
	if (tls->mpt_inflight_cond)
		ABT_cond_free(&tls->mpt_inflight_cond);
	if (tls->mpt_inflight_mutex)
		ABT_mutex_free(&tls->mpt_inflight_mutex);
	if (tls->mpt_init_cond)
		ABT_cond_free(&tls->mpt_init_cond);
	if (tls->mpt_init_mutex)
		ABT_mutex_free(&tls->mpt_init_mutex);
	if (daos_handle_is_valid(tls->mpt_root_hdl))
		obj_tree_destroy(tls->mpt_root_hdl);
	if (daos_handle_is_valid(tls->mpt_migrated_root_hdl))
		obj_tree_destroy(tls->mpt_migrated_root_hdl);
	D_FREE(tls);
}

void
migrate_pool_tls_get(struct migrate_pool_tls *tls)
{
	if (!tls)
		return;
	tls->mpt_refcount++;
}

void
migrate_pool_tls_put(struct migrate_pool_tls *tls)
{
	if (!tls)
		return;
	tls->mpt_refcount--;
	if (tls->mpt_fini && tls->mpt_refcount == 1)
		ABT_eventual_set(tls->mpt_done_eventual, NULL, 0);
	if (tls->mpt_refcount == 0)
		migrate_pool_tls_destroy(tls);
}

struct migrate_pool_tls *
migrate_pool_tls_lookup(uuid_t pool_uuid, unsigned int ver, uint32_t gen)
{
	struct obj_tls	*tls = obj_tls_get();
	struct migrate_pool_tls *pool_tls;
	struct migrate_pool_tls *found = NULL;

	D_ASSERT(tls != NULL);
	/* Only 1 thread will access the list, no need lock */
	d_list_for_each_entry(pool_tls, &tls->ot_pool_list, mpt_list) {
		if (uuid_compare(pool_tls->mpt_pool_uuid, pool_uuid) == 0 &&
		    (ver == (unsigned int)(-1) || ver == pool_tls->mpt_version) &&
		    (gen == (unsigned int)(-1) || gen == pool_tls->mpt_generation)) {
			migrate_pool_tls_get(pool_tls);
			found = pool_tls;
			break;
		}
	}

	return found;
}

#define MPT_CREATE_TGT_INLINE	(32)
struct migrate_pool_tls_create_arg {
	uuid_t		 pool_uuid;
	uuid_t		 pool_hdl_uuid;
	uuid_t		 co_hdl_uuid;
	d_rank_list_t	*svc_list;
	uint8_t		*tgt_status;
	uint8_t		 tgt_status_inline[MPT_CREATE_TGT_INLINE];
	uint32_t	*tgt_in_ver;
	uint32_t	 tgt_in_ver_inline[MPT_CREATE_TGT_INLINE];
	ATOMIC uint32_t	*obj_ult_cnts;
	ATOMIC uint32_t	*dkey_ult_cnts;
	uint64_t	 max_eph;
	unsigned int	 version;
	unsigned int	 generation;
	uint32_t	 opc;
	uint32_t	 new_layout_ver;
	uint32_t	 max_ult_cnt;
};

int
migrate_pool_tls_create_one(void *data)
{
	struct migrate_pool_tls_create_arg *arg = data;
	struct obj_tls			   *tls = obj_tls_get();
	uint32_t			    tgt_id;
	struct migrate_pool_tls		   *pool_tls;
	struct ds_pool_child		   *pool_child = NULL;
	int				    rc = 0;

	pool_tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (pool_tls != NULL) {
		/* Some one else already created, because collective function
		 * might yield xstream.
		 */
		migrate_pool_tls_put(pool_tls);
		return 0;
	}

	pool_child = ds_pool_child_lookup(arg->pool_uuid);
	if (pool_child == NULL) {
		/* Local ds_pool_child isn't started yet, return a retry-able error */
		if (dss_get_module_info()->dmi_xs_id != 0) {
			D_INFO(DF_UUID ": Local VOS pool isn't ready yet.\n",
			       DP_UUID(arg->pool_uuid));
			return -DER_STALE;
		}
	} else if (unlikely(pool_child->spc_no_storage)) {
		D_DEBUG(DB_REBUILD, DF_UUID" "DF_UUID" lost pool shard, ver %d, skip.\n",
			DP_UUID(arg->pool_uuid), DP_UUID(arg->pool_hdl_uuid), arg->version);
		D_GOTO(out, rc = 0);
	}

	D_ALLOC_PTR(pool_tls);
	if (pool_tls == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	D_INIT_LIST_HEAD(&pool_tls->mpt_cont_hdl_list);
	pool_tls->mpt_pool_hdl = DAOS_HDL_INVAL;
	D_INIT_LIST_HEAD(&pool_tls->mpt_list);

	rc = ABT_eventual_create(0, &pool_tls->mpt_done_eventual);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	rc = ABT_cond_create(&pool_tls->mpt_inflight_cond);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	rc = ABT_mutex_create(&pool_tls->mpt_inflight_mutex);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	uuid_copy(pool_tls->mpt_pool_uuid, arg->pool_uuid);
	uuid_copy(pool_tls->mpt_poh_uuid, arg->pool_hdl_uuid);
	uuid_copy(pool_tls->mpt_coh_uuid, arg->co_hdl_uuid);
	pool_tls->mpt_version = arg->version;
	pool_tls->mpt_generation = arg->generation;
	pool_tls->mpt_rec_count = 0;
	pool_tls->mpt_obj_count = 0;
	pool_tls->mpt_size = 0;
	pool_tls->mpt_root_hdl = DAOS_HDL_INVAL;
	pool_tls->mpt_max_eph = arg->max_eph;
	pool_tls->mpt_new_layout_ver = arg->new_layout_ver;
	pool_tls->mpt_opc = arg->opc;
	if (dss_get_module_info()->dmi_xs_id == 0) {
		int i;

		pool_tls->mpt_inflight_max_size = MIGRATE_MAX_SIZE;
		pool_tls->mpt_inflight_max_ult = arg->max_ult_cnt;
		D_ALLOC_ARRAY(pool_tls->mpt_obj_ult_cnts, dss_tgt_nr);
		D_ALLOC_ARRAY(pool_tls->mpt_dkey_ult_cnts, dss_tgt_nr);
		if (pool_tls->mpt_obj_ult_cnts == NULL || pool_tls->mpt_dkey_ult_cnts == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		for (i = 0; i < dss_tgt_nr; i++) {
			atomic_init(&pool_tls->mpt_obj_ult_cnts[i], 0);
			atomic_init(&pool_tls->mpt_dkey_ult_cnts[i], 0);
		}
	} else {
		tgt_id = dss_get_module_info()->dmi_tgt_id;

		pool_tls->mpt_pool = ds_pool_child_lookup(arg->pool_uuid);
		if (pool_tls->mpt_pool == NULL)
			D_GOTO(out, rc = -DER_NO_HDL);
		pool_tls->mpt_inflight_max_size = MIGRATE_MAX_SIZE / dss_tgt_nr;
		pool_tls->mpt_inflight_max_ult = arg->max_ult_cnt / dss_tgt_nr;
		pool_tls->mpt_tgt_obj_ult_cnt = &arg->obj_ult_cnts[tgt_id];
		pool_tls->mpt_tgt_dkey_ult_cnt = &arg->dkey_ult_cnts[tgt_id];

		if (pool_child->spc_pool->sp_incr_reint && arg->opc == RB_OP_REBUILD &&
		    arg->tgt_status[tgt_id] == PO_COMP_ST_UP &&
		    arg->tgt_in_ver[tgt_id] <= pool_tls->mpt_version)
			pool_tls->mpt_reintegrating = 1;
		D_DEBUG(DB_REBUILD, DF_RB" tgt %d status %u in version %u, mpt_reintegrating %d\n",
			DP_RB_MPT(pool_tls), tgt_id, arg->tgt_status[tgt_id],
			arg->tgt_in_ver[tgt_id], pool_tls->mpt_reintegrating);
	}

	pool_tls->mpt_inflight_size = 0;
	pool_tls->mpt_refcount = 1;
	if (arg->svc_list) {
		rc = daos_rank_list_copy(&pool_tls->mpt_svc_list, arg->svc_list);
		if (rc)
			D_GOTO(out, rc);
	}

	D_DEBUG(DB_REBUILD, DF_RB ": TLS %p create for hdls " DF_UUID "/" DF_UUID " " DF_RC "\n",
		DP_RB_MPT(pool_tls), pool_tls, DP_UUID(arg->pool_hdl_uuid),
		DP_UUID(arg->co_hdl_uuid), DP_RC(rc));
	d_list_add(&pool_tls->mpt_list, &tls->ot_pool_list);
out:
	if (rc && pool_tls)
		migrate_pool_tls_destroy(pool_tls);

	if (pool_child != NULL)
		ds_pool_child_put(pool_child);

	return rc;
}

static int
migrate_pool_tls_lookup_create(struct ds_pool *pool, unsigned int version, unsigned int generation,
			       uuid_t pool_hdl_uuid, uuid_t co_hdl_uuid, uint64_t max_eph,
			       uint32_t new_layout_ver, uint32_t opc, struct migrate_pool_tls **p_tls)
{
	struct migrate_pool_tls *tls = NULL;
	struct migrate_pool_tls_create_arg arg = { 0 };
	daos_prop_t		*prop = NULL;
	struct daos_prop_entry	*entry;
	struct pool_target	*tgts;
	uint32_t		 max_migrate_ult = MIGRATE_DEFAULT_MAX_ULT;
	d_rank_t		 rank;
	int			 i, rc = 0;

	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);
	tls = migrate_pool_tls_lookup(pool->sp_uuid, version, generation);
	if (tls) {
		if (tls->mpt_init_tls) {
			ABT_mutex_lock(tls->mpt_init_mutex);
			ABT_cond_wait(tls->mpt_init_cond, tls->mpt_init_mutex);
			ABT_mutex_unlock(tls->mpt_init_mutex);
			if (tls->mpt_init_err) {
				migrate_pool_tls_put(tls);
				rc = tls->mpt_init_err;
			}
		}

		if (rc == 0)
			*p_tls = tls;

		return rc;
	}

	d_getenv_uint(ENV_MIGRATE_ULT_CNT, &max_migrate_ult);
	D_ASSERT(generation != (unsigned int)(-1));
	uuid_copy(arg.pool_uuid, pool->sp_uuid);
	uuid_copy(arg.pool_hdl_uuid, pool_hdl_uuid);
	uuid_copy(arg.co_hdl_uuid, co_hdl_uuid);
	arg.version = version;
	arg.opc = opc;
	arg.max_eph = max_eph;
	arg.new_layout_ver = new_layout_ver;
	arg.generation = generation;
	arg.max_ult_cnt = max_migrate_ult;

	/*
	 * dss_task_collective does not do collective on sys xstrem,
	 * sys xstream need some information to track rebuild status.
	 */
	rc = migrate_pool_tls_create_one(&arg);
	if (rc)
		D_GOTO(out, rc);

	tls = migrate_pool_tls_lookup(pool->sp_uuid, version, generation);
	D_ASSERT(tls != NULL);
	pool->sp_rebuilding++;

	rc = ABT_cond_create(&tls->mpt_init_cond);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	rc = ABT_mutex_create(&tls->mpt_init_mutex);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc = dss_abterr2der(rc));

	tls->mpt_init_tls = 1;
	D_ALLOC_PTR(prop);
	if (prop == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	if (likely(dss_tgt_nr <= MPT_CREATE_TGT_INLINE)) {
		arg.tgt_status = arg.tgt_status_inline;
		arg.tgt_in_ver = arg.tgt_in_ver_inline;
	} else {
		D_ALLOC_ARRAY(arg.tgt_status, dss_tgt_nr);
		if (arg.tgt_status == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		D_ALLOC_ARRAY(arg.tgt_in_ver, dss_tgt_nr);
		if (arg.tgt_in_ver == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
	}

	rank = dss_self_rank();
	rc = pool_map_find_target_by_rank_idx(pool->sp_map, rank, -1, &tgts);
	D_ASSERT(rc == dss_tgt_nr);
	for (i = 0; i < dss_tgt_nr; i++) {
		arg.tgt_status[i] = tgts[i].ta_comp.co_status;
		arg.tgt_in_ver[i] = tgts[i].ta_comp.co_in_ver;
	}

	rc = ds_pool_iv_prop_fetch(pool, prop);
	if (rc)
		D_GOTO(out, rc);

	entry = daos_prop_entry_get(prop, DAOS_PROP_PO_SVC_LIST);
	D_ASSERT(entry != NULL);
	arg.svc_list = (d_rank_list_t *)entry->dpe_val_ptr;
	arg.obj_ult_cnts = tls->mpt_obj_ult_cnts;
	arg.dkey_ult_cnts = tls->mpt_dkey_ult_cnts;
	rc = ds_pool_task_collective(pool->sp_uuid,
				     PO_COMP_ST_NEW | PO_COMP_ST_DOWN | PO_COMP_ST_DOWNOUT,
				     migrate_pool_tls_create_one, &arg, 0);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": failed to create migrate tls on tgt xstreams",
			 DP_RB_MPT(tls));
		D_GOTO(out, rc);
	}

out:
	if (tls != NULL && tls->mpt_init_tls) {
		tls->mpt_init_tls = 0;
		/* Set init failed, so the waiting lookup(above) can be notified */
		if (rc != 0)
			tls->mpt_init_err = rc;
		ABT_mutex_lock(tls->mpt_init_mutex);
		ABT_cond_broadcast(tls->mpt_init_cond);
		ABT_mutex_unlock(tls->mpt_init_mutex);
	}
	D_DEBUG(DB_TRACE, "create tls " DF_UUID ": " DF_RC "\n", DP_UUID(pool->sp_uuid), DP_RC(rc));

	if (rc != 0) {
		if (tls != NULL)
			migrate_pool_tls_put(tls);
	} else {
		*p_tls = tls;
	}

	if (prop != NULL)
		daos_prop_free(prop);
	if (arg.tgt_status != NULL && arg.tgt_status != arg.tgt_status_inline)
		D_FREE(arg.tgt_status);
	if (arg.tgt_in_ver != NULL && arg.tgt_in_ver != arg.tgt_in_ver_inline)
		D_FREE(arg.tgt_in_ver);

	return rc;
}

static void
mrone_recx_daos_vos_internal(struct migrate_one *mrone, bool daos2vos, int shard,
			     daos_iod_t *iods, int iods_num)
{
	daos_iod_t *iod;
	int cell_nr;
	int stripe_nr;
	int j;
	int k;

	D_ASSERT(daos_oclass_is_ec(&mrone->mo_oca));

	cell_nr = obj_ec_cell_rec_nr(&mrone->mo_oca);
	stripe_nr = obj_ec_stripe_rec_nr(&mrone->mo_oca);
	/* Convert the DAOS to VOS EC offset */
	for (j = 0; j < iods_num; j++) {
		iod = &iods[j];
		if (iod->iod_type == DAOS_IOD_SINGLE)
			continue;
		for (k = 0; k < iod->iod_nr; k++) {
			daos_recx_t *recx;

			recx = &iod->iod_recxs[k];
			D_ASSERTF(recx->rx_nr <= cell_nr, DF_U64"/"DF_U64"cell nr %d "DF_UOID"\n",
				  recx->rx_idx, recx->rx_nr, cell_nr, DP_UOID(mrone->mo_oid));
			if (daos2vos)
				recx->rx_idx = obj_ec_idx_daos2vos(recx->rx_idx,
								   stripe_nr,
								   cell_nr);
			else
				recx->rx_idx = obj_ec_idx_vos2daos(recx->rx_idx,
								   stripe_nr,
								   cell_nr,
								   shard);
			D_DEBUG(DB_REBUILD, DF_RB ": j %d k %d " DF_U64 "/" DF_U64 "\n",
				DP_RB_MRO(mrone), j, k, recx->rx_idx, recx->rx_nr);
		}
	}
}

static void
mrone_recx_daos2_vos(struct migrate_one *mrone, daos_iod_t *iods, int iods_num)
{
	mrone_recx_daos_vos_internal(mrone, true, -1, iods, iods_num);
}

static void
mrone_recx_vos2_daos(struct migrate_one *mrone, int shard, daos_iod_t *iods, int iods_num)
{
	shard = obj_ec_shard_off_by_layout_ver(mrone->mo_oid.id_layout_ver, mrone->mo_dkey_hash,
					       &mrone->mo_oca, shard);
	D_ASSERT(shard < obj_ec_data_tgt_nr(&mrone->mo_oca));
	mrone_recx_daos_vos_internal(mrone, false, shard, iods, iods_num);
}

static int
mrone_obj_fetch_internal(struct migrate_one *mrone, daos_handle_t oh, d_sg_list_t *sgls,
			 daos_iod_t *iods, int iod_num, daos_epoch_t eph, uint32_t flags,
			 d_iov_t *csum_iov_fetch, struct migrate_pool_tls *tls)
{
	int rc;

retry:
	rc = dsc_obj_fetch(oh, eph, &mrone->mo_dkey, iod_num, iods, sgls,
			   NULL, flags, NULL, csum_iov_fetch);
	if (rc == -DER_TIMEDOUT &&
	    tls->mpt_version + 1 >= tls->mpt_pool->spc_map_version) {
		if (tls->mpt_fini) {
			DL_ERROR(rc, DF_RB ": dsc_obj_fetch " DF_UOID "failed when mpt_fini",
				 DP_RB_MPT(tls), DP_UOID(mrone->mo_oid));
			return rc;
		}
		/* If pool map does not change, then let's retry for timeout, instead of
		 * fail out.
		 */
		DL_WARN(rc, DF_RB ": retry " DF_UOID, DP_RB_MPT(tls), DP_UOID(mrone->mo_oid));
		D_GOTO(retry, rc);
	}

	return rc;
}

static int
mrone_obj_fetch(struct migrate_one *mrone, daos_handle_t oh, d_sg_list_t *sgls,
		daos_iod_t *iods, int iod_num, daos_epoch_t eph, uint32_t flags,
		d_iov_t *csum_iov_fetch)
{
	struct migrate_pool_tls	*tls;
	int			rc = 0;

	tls = migrate_pool_tls_lookup(mrone->mo_pool_uuid,
				      mrone->mo_pool_tls_version, mrone->mo_generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(mrone->mo_pool_uuid));
		D_GOTO(out, rc = -DER_SHUTDOWN);
	}

	if (daos_oclass_grp_size(&mrone->mo_oca) > 1)
		flags |= DIOF_TO_LEADER;

	rc = mrone_obj_fetch_internal(mrone, oh, sgls, iods, iod_num, eph,
				      flags, csum_iov_fetch, tls);
	if (rc != 0)
		D_GOTO(out, rc);

	if (csum_iov_fetch != NULL &&
	    csum_iov_fetch->iov_len > csum_iov_fetch->iov_buf_len) {
		/** retry dsc_obj_fetch with appropriate csum_iov
		 * buf length
		 */
		void *p;

		D_REALLOC(p, csum_iov_fetch->iov_buf,
			  csum_iov_fetch->iov_buf_len, csum_iov_fetch->iov_len);
		if (p == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
		csum_iov_fetch->iov_buf_len = csum_iov_fetch->iov_len;
		csum_iov_fetch->iov_len = 0;
		csum_iov_fetch->iov_buf = p;

		rc = mrone_obj_fetch_internal(mrone, oh, sgls, iods, iod_num,
					      eph, flags, csum_iov_fetch, tls);
	}

out:
	migrate_pool_tls_put(tls);
	return rc;
}

static int
migrate_csum_calc(struct daos_csummer *csummer, struct migrate_one *mrone, daos_iod_t *iods,
		  int iod_num, d_sg_list_t *sgls, d_iov_t *csum_iov,
		  struct dcs_iod_csums **iod_csums)
{
	d_iov_t		tmp_csum_iov;
	d_iov_t		*p_csum_iov;
	int		rc;

	if (daos_oclass_is_ec(&mrone->mo_oca)) {
		D_DEBUG(DB_CSUM, DF_RB ": " DF_C_UOID_DKEY ": Calculating csums. IOD count: %d\n",
			DP_RB_MRO(mrone), DP_C_UOID_DKEY(mrone->mo_oid, &mrone->mo_dkey), iod_num);
		rc = daos_csummer_calc_iods(csummer, sgls, iods, NULL, iod_num, false, NULL,
					    -1, iod_csums);
		return rc;
	}

	D_DEBUG(DB_CSUM, DF_RB ": " DF_C_UOID_DKEY ": Using packed csums\n", DP_RB_MRO(mrone),
		DP_C_UOID_DKEY(mrone->mo_oid, &mrone->mo_dkey));
	/** make a copy of the iov because it will be modified while
	 * iterating over the csums
	 */
	D_ASSERT(csum_iov != NULL);
	tmp_csum_iov = *csum_iov;
	p_csum_iov = &tmp_csum_iov;
	rc = daos_csummer_alloc_iods_csums_with_packed(csummer, iods, iod_num,
						       p_csum_iov, iod_csums);
	if (rc != 0)
		DL_ERROR(rc, DF_RB ": failed to alloc iod csums", DP_RB_MRO(mrone));

	return rc;
}

#define MIGRATE_STACK_SIZE	131072
#define MAX_BUF_SIZE		2048
#define CSUM_BUF_SIZE		256

/**
 * allocate the memory for the iods_csums and unpack the csum_iov into the
 * into the iods_csums.
 * Note: the csum_iov is modified so a shallow copy should be sent instead of
 * the original.
 */
static int
migrate_fetch_update_inline(struct migrate_one *mrone, daos_handle_t oh,
			    struct ds_cont_child *ds_cont)
{
	d_sg_list_t		 sgls[OBJ_ENUM_UNPACK_MAX_IODS];
	d_iov_t			 iov[OBJ_ENUM_UNPACK_MAX_IODS];
	struct daos_csummer	*csummer = NULL;
	struct dcs_iod_csums	*iod_csums = NULL;
	int			 iod_cnt = 0;
	int			 start;
	char		 iov_buf[OBJ_ENUM_UNPACK_MAX_IODS][MAX_BUF_SIZE];
	bool			 fetch = false;
	int			 i;
	int			 rc = 0;
	d_iov_t			*p_csum_iov = NULL;
	d_iov_t			 csum_iov = {0};

	D_ASSERT(mrone->mo_iod_num <= OBJ_ENUM_UNPACK_MAX_IODS);
	for (i = 0; i < mrone->mo_iod_num; i++) {
		if (mrone->mo_iods[i].iod_size == 0)
			continue;

		/* Let's do real fetch for all EC object, since the
		 * checksum needs to be re-calculated for EC rebuild,
		 * and we do not have checksum information yet.
		 */
		if ((mrone->mo_sgls != NULL && mrone->mo_sgls[i].sg_nr > 0) &&
		     !daos_oclass_is_ec(&mrone->mo_oca)) {
			sgls[i] = mrone->mo_sgls[i];
		} else {
			sgls[i].sg_nr = 1;
			sgls[i].sg_nr_out = 1;
			d_iov_set(&iov[i], iov_buf[i], MAX_BUF_SIZE);
			sgls[i].sg_iovs = &iov[i];
			fetch = true;
		}
	}

	D_DEBUG(DB_REBUILD,
		DF_RB ": " DF_UOID " mrone %p dkey " DF_KEY " nr %d eph " DF_U64 " fetch %s\n",
		DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey),
		mrone->mo_iod_num, mrone->mo_epoch, fetch ? "yes" : "no");

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_NO_UPDATE))
		return 0;

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_UPDATE_FAIL))
		return -DER_INVAL;

	if (fetch) {
		if (!daos_oclass_is_ec(&mrone->mo_oca)) {
			rc = daos_iov_alloc(&csum_iov, CSUM_BUF_SIZE, false);
			if (rc != 0)
				D_GOTO(out, rc);

			p_csum_iov = &csum_iov;
		}

		rc = mrone_obj_fetch(mrone, oh, sgls, mrone->mo_iods, mrone->mo_iod_num,
				     mrone->mo_epoch, DIOF_FOR_MIGRATION, p_csum_iov);

		if (rc) {
			DL_ERROR(rc, DF_RB ": mrone_obj_fetch", DP_RB_MRO(mrone));
			D_GOTO(out, rc);
		}
	}

	if (daos_oclass_is_ec(&mrone->mo_oca) &&
	    !is_ec_parity_shard_by_layout_ver(mrone->mo_oid.id_layout_ver, mrone->mo_dkey_hash,
					      &mrone->mo_oca, mrone->mo_oid.id_shard))
		mrone_recx_daos2_vos(mrone, mrone->mo_iods, mrone->mo_iod_num);

	csummer = dsc_cont2csummer(dc_obj_hdl2cont_hdl(oh));
	for (i = 0, start = 0; i < mrone->mo_iod_num; i++) {
		daos_iod_t *iods = mrone->mo_iods;

		if (iods[i].iod_size > 0) {
			iod_cnt++;
			continue;
		}

		/* skip empty record */
		if (iod_cnt == 0) {
			D_DEBUG(DB_TRACE, DF_RB ": i %d iod_size = 0\n", DP_RB_MRO(mrone), i);
			continue;
		}

		D_DEBUG(DB_TRACE, DF_RB ": update start %d cnt %d\n", DP_RB_MRO(mrone), start,
			iod_cnt);

		rc = migrate_csum_calc(csummer, mrone, &iods[start], iod_cnt, &sgls[start],
				       fetch ? &csum_iov : &mrone->mo_csum_iov,  &iod_csums);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": error calculating checksums", DP_RB_MRO(mrone));
			break;
		}

		rc = vos_obj_update(ds_cont->sc_hdl, mrone->mo_oid,
				    mrone->mo_min_epoch, mrone->mo_version,
				    VOS_OF_REBUILD, &mrone->mo_dkey, iod_cnt, &iods[start],
				    iod_csums, &sgls[start]);
		daos_csummer_free_ic(csummer, &iod_csums);
		if (rc) {
			DL_ERROR(rc, DF_RB ": migrate failed", DP_RB_MRO(mrone));
			break;
		}
		iod_cnt = 0;
		start = i + 1;
	}

	if (iod_cnt > 0) {
		rc = migrate_csum_calc(csummer, mrone, &mrone->mo_iods[start], iod_cnt,
				       &sgls[start], fetch ? &csum_iov : &mrone->mo_csum_iov,
				       &iod_csums);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": error calculating checksums", DP_RB_MRO(mrone));
			D_GOTO(out, rc);
		}

		rc = vos_obj_update(ds_cont->sc_hdl, mrone->mo_oid,
				    mrone->mo_min_epoch, mrone->mo_version,
				    VOS_OF_REBUILD, &mrone->mo_dkey, iod_cnt,
				    &mrone->mo_iods[start], iod_csums,
				    &sgls[start]);
		if (rc) {
			DL_ERROR(rc, DF_RB ": migrate failed", DP_RB_MRO(mrone));
			D_GOTO(out, rc);
		}
		daos_csummer_free_ic(csummer, &iod_csums);
	}

out:
	if (csum_iov.iov_buf != NULL)
		D_FREE(csum_iov.iov_buf);

	return rc;
}

static int
migrate_update_parity(struct migrate_one *mrone, daos_epoch_t parity_eph,
		      struct ds_cont_child *ds_cont, unsigned char *buffer,
		      daos_off_t offset, daos_size_t size, daos_iod_t *iod,
		      unsigned char *p_bufs[], struct daos_csummer *csummer, bool encode)
{
	struct daos_oclass_attr	*oca = &mrone->mo_oca;
	daos_size_t		 stride_nr = obj_ec_stripe_rec_nr(oca);
	daos_size_t		 cell_nr = obj_ec_cell_rec_nr(oca);
	daos_size_t		split_size;
	daos_recx_t		 tmp_recx;
	d_iov_t			 tmp_iov;
	d_sg_list_t		 tmp_sgl;
	daos_size_t		 write_nr;
	struct dcs_iod_csums	*iod_csums = NULL;
	int			rc = 0;

	split_size = encode ? stride_nr : cell_nr;
	tmp_sgl.sg_nr = tmp_sgl.sg_nr_out = 1;
	while (size > 0) {
		if (offset % split_size != 0)
			write_nr = min(roundup(offset, split_size) - offset, size);
		else
			write_nr = min(split_size, size);

		if (write_nr == stride_nr) {
			unsigned int shard;

			D_ASSERT(encode);
			shard = obj_ec_shard_off_by_layout_ver(mrone->mo_oid.id_layout_ver,
							       mrone->mo_dkey_hash, &mrone->mo_oca,
							       mrone->mo_oid.id_shard);
			D_ASSERT(shard >= obj_ec_data_tgt_nr(oca));
			shard -= obj_ec_data_tgt_nr(oca);
			D_ASSERT(shard < obj_ec_parity_tgt_nr(oca));
			rc = obj_ec_encode_buf(mrone->mo_oid.id_pub,
					       oca, iod->iod_size, buffer,
					       p_bufs);
			if (rc)
				D_GOTO(out, rc);
			tmp_recx.rx_idx = obj_ec_idx_daos2vos(offset, stride_nr,
							      cell_nr);
			tmp_recx.rx_idx |= PARITY_INDICATOR;
			tmp_recx.rx_nr = cell_nr;
			d_iov_set(&tmp_iov, p_bufs[shard],
				  cell_nr * iod->iod_size);
			D_DEBUG(DB_IO, DF_RB ": parity " DF_X64 "/" DF_U64 " " DF_U64 "\n",
				DP_RB_MRO(mrone), tmp_recx.rx_idx, tmp_recx.rx_nr, iod->iod_size);
		} else {
			tmp_recx.rx_idx = offset;
			tmp_recx.rx_nr = write_nr;
			d_iov_set(&tmp_iov, buffer, write_nr * iod->iod_size);
			D_DEBUG(DB_IO, DF_RB ": replicate " DF_U64 "/" DF_U64 " " DF_U64 "\n",
				DP_RB_MRO(mrone), tmp_recx.rx_idx, tmp_recx.rx_nr, iod->iod_size);
		}

		tmp_sgl.sg_iovs = &tmp_iov;
		iod->iod_recxs = &tmp_recx;
		iod->iod_nr = 1;
		rc = daos_csummer_calc_iods(csummer, &tmp_sgl, iod, NULL, 1,
					    false, NULL, 0, &iod_csums);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": drror calculating checksums", DP_RB_MRO(mrone));
			D_GOTO(out, rc);
		}

		rc = vos_obj_update(ds_cont->sc_hdl, mrone->mo_oid,
				    parity_eph, mrone->mo_version,
				    VOS_OF_REBUILD, &mrone->mo_dkey, 1, iod, iod_csums,
				    &tmp_sgl);
		if (rc != 0)
			D_GOTO(out, rc);

		size -= write_nr;
		offset += write_nr;
		buffer += write_nr * iod->iod_size;
	}
out:
	daos_csummer_free_ic(csummer, &iod_csums);
	return rc;
}

static int
__migrate_fetch_update_parity(struct migrate_one *mrone, daos_handle_t oh,
			      daos_iod_t *iods, daos_epoch_t fetch_eph,
			      daos_epoch_t **ephs, uint32_t iods_num,
			      struct ds_cont_child *ds_cont, bool encode)
{
	d_sg_list_t	 sgls[OBJ_ENUM_UNPACK_MAX_IODS];
	d_iov_t		 iov[OBJ_ENUM_UNPACK_MAX_IODS] = { 0 };
	char		*data;
	daos_size_t	 size;
	unsigned int	 p = obj_ec_parity_tgt_nr(&mrone->mo_oca);
	daos_size_t	stride_nr = obj_ec_stripe_rec_nr(&mrone->mo_oca);
	unsigned char	*p_bufs[OBJ_EC_MAX_P] = { 0 };
	struct daos_csummer	*csummer = NULL;
	unsigned char	*ptr;
	int		 i;
	int		 rc;

	D_ASSERT(iods_num <= OBJ_ENUM_UNPACK_MAX_IODS);
	for (i = 0; i < iods_num; i++) {
		size = daos_iods_len(&iods[i], 1);
		D_ALLOC(data, size);
		if (data == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		d_iov_set(&iov[i], data, size);
		sgls[i].sg_nr = 1;
		sgls[i].sg_nr_out = 1;
		sgls[i].sg_iovs = &iov[i];
	}

	D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " mrone %p dkey " DF_KEY " nr %d eph " DF_U64 "\n",
		DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey), iods_num,
		mrone->mo_epoch);

	rc = mrone_obj_fetch(mrone, oh, sgls, iods, iods_num, fetch_eph, DIOF_FOR_MIGRATION,
			     NULL);
	if (rc) {
		DL_ERROR(rc, DF_RB ": migrate dkey " DF_KEY " failed", DP_RB_MRO(mrone),
			 DP_KEY(&mrone->mo_dkey));
		D_GOTO(out, rc);
	}

	csummer = dsc_cont2csummer(dc_obj_hdl2cont_hdl(oh));
	for (i = 0; i < iods_num; i++) {
		daos_off_t	offset;
		daos_iod_t	tmp_iod;
		daos_epoch_t	parity_eph;
		int		j;

		offset = iods[i].iod_recxs[0].rx_idx;
		size = iods[i].iod_recxs[0].rx_nr;
		/* Use stable epoch for partial parity update to make sure
		 * these partial updates are not below stable epoch boundary,
		 * otherwise both EC and VOS aggregation might operate on
		 * the same recxs.
		 */
		parity_eph = encode ? ephs[i][0] : mrone->mo_epoch;
		tmp_iod = iods[i];
		ptr = iov[i].iov_buf;
		for (j = 1; j < iods[i].iod_nr; j++) {
			daos_recx_t	*recx = &iods[i].iod_recxs[j];

			/* Merge the recx if there are in the same stripe */
			if (offset + size == recx->rx_idx &&
			    offset / stride_nr == recx->rx_idx / stride_nr) {
				size += recx->rx_nr;
				parity_eph = max(ephs[i][j], parity_eph);
				continue;
			}

			rc = migrate_update_parity(mrone, parity_eph, ds_cont, ptr, offset,
						   size, &tmp_iod, p_bufs, csummer, encode);
			if (rc)
				D_GOTO(out, rc);
			ptr += size * iods[i].iod_size;
			offset = recx->rx_idx;
			size = recx->rx_nr;
			parity_eph = encode ? ephs[i][j] : mrone->mo_epoch;
		}

		if (size > 0)
			rc = migrate_update_parity(mrone, parity_eph, ds_cont, ptr, offset,
						   size, &tmp_iod, p_bufs, csummer, encode);
	}
out:
	for (i = 0; i < iods_num; i++) {
		if (iov[i].iov_buf)
			D_FREE(iov[i].iov_buf);
	}

	for (i = 0; i < p; i++) {
		if (p_bufs[i] != NULL)
			D_FREE(p_bufs[i]);
	}

	return rc;
}

static int
migrate_fetch_update_parity(struct migrate_one *mrone, daos_handle_t oh,
			    struct ds_cont_child *ds_cont)
{
	int i;
	int j;
	int rc = 0;

	/* If it is parity recxs from another replica, then let's encode it anyway */
	for (i = 0; i < mrone->mo_iods_num_from_parity; i++) {
		for (j = 0; j < mrone->mo_iods_from_parity[i].iod_nr; j++) {
			daos_iod_t iod = mrone->mo_iods_from_parity[i];
			daos_epoch_t fetch_eph;
			daos_epoch_t update_eph;
			daos_epoch_t *update_eph_p;

			iod.iod_nr = 1;
			iod.iod_recxs = &mrone->mo_iods_from_parity[i].iod_recxs[j];
			/* If the epoch is higher than EC aggregate boundary, then
			 * it should use stable epoch to fetch the data, since
			 * the data could be aggregated independently on parity
			 * and data shard, so it should select the minimum epoch
			 * between stable epoch and boundary epoch as the recovery
			 * epoch to make sure parity data rebuilt will not be interfered
			 * by the newer update.
			 *
			 * Otherwise there might be partial update on this rebuilding
			 * shard, so let's use the epoch from the parity shard to fetch
			 * the data here, which will make sure partial update will not
			 * be fetched here. And also EC aggregation is being disabled
			 * at the moment, so there should not be any vos aggregation
			 * impact this process as well.
			 */
			if (ds_cont->sc_ec_agg_eph_boundary >
			    mrone->mo_iods_update_ephs_from_parity[i][j])
				fetch_eph = min(ds_cont->sc_ec_agg_eph_boundary, mrone->mo_epoch);
			else
				fetch_eph = mrone->mo_iods_update_ephs_from_parity[i][j];

			update_eph = mrone->mo_iods_update_ephs_from_parity[i][j];
			update_eph_p = &update_eph;
			rc = __migrate_fetch_update_parity(mrone, oh, &iod, fetch_eph,
							   &update_eph_p, 1, ds_cont, true);
			if (rc)
				return rc;
		}
	}

	/* Otherwise, keep it as replicate recx */
	if (mrone->mo_iod_num > 0) {
		rc = __migrate_fetch_update_parity(mrone, oh, mrone->mo_iods, mrone->mo_epoch,
						   mrone->mo_iods_update_ephs,
						   mrone->mo_iod_num, ds_cont, false);
	}

	return rc;
}

static int
migrate_fetch_update_single(struct migrate_one *mrone, daos_handle_t oh,
			    struct ds_cont_child *ds_cont)
{
	d_sg_list_t		 sgls[OBJ_ENUM_UNPACK_MAX_IODS];
	d_iov_t			 iov[OBJ_ENUM_UNPACK_MAX_IODS] = { 0 };
	struct dcs_layout	 los[OBJ_ENUM_UNPACK_MAX_IODS] = { 0 };
	char			*data;
	daos_size_t		 size;
	d_iov_t			 csum_iov = {0};
	d_iov_t			*p_csum_iov = NULL;
	struct daos_csummer	*csummer = NULL;
	struct dcs_iod_csums	*iod_csums = NULL;
	uint64_t		 update_flags = VOS_OF_REBUILD;
	uint32_t		tgt_off = 0;
	int			 i;
	int			 rc;

	D_ASSERT(mrone->mo_iod_num <= OBJ_ENUM_UNPACK_MAX_IODS);
	for (i = 0; i < mrone->mo_iod_num; i++) {
		D_ASSERT(mrone->mo_iods[i].iod_type == DAOS_IOD_SINGLE);

		size = daos_iods_len(&mrone->mo_iods[i], 1);
		D_ASSERT(size != -1);
		D_ALLOC(data, size);
		if (data == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		d_iov_set(&iov[i], data, size);
		sgls[i].sg_nr = 1;
		sgls[i].sg_nr_out = 1;
		sgls[i].sg_iovs = &iov[i];
	}

	D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " mrone %p dkey " DF_KEY " nr %d eph " DF_U64 "\n",
		DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey),
		mrone->mo_iod_num, mrone->mo_epoch);

	if (!daos_oclass_is_ec(&mrone->mo_oca)) {
		rc = daos_iov_alloc(&csum_iov, CSUM_BUF_SIZE, false);
		if (rc != 0)
			D_GOTO(out, rc);
		p_csum_iov = &csum_iov;
	}

	rc = mrone_obj_fetch(mrone, oh, sgls, mrone->mo_iods, mrone->mo_iod_num,
			     mrone->mo_epoch, DIOF_FOR_MIGRATION, p_csum_iov);
	if (rc == -DER_CSUM) {
		DL_ERROR(rc,
			 DF_RB ": migrate dkey " DF_KEY " failed because of checksum error. "
			       "Don't fail whole rebuild",
			 DP_RB_MRO(mrone), DP_KEY(&mrone->mo_dkey));
		D_GOTO(out, rc = 0);
	}
	if (rc) {
		DL_ERROR(rc, DF_RB ": migrate dkey " DF_KEY " failed", DP_RB_MRO(mrone),
			 DP_KEY(&mrone->mo_dkey));
		D_GOTO(out, rc);
	}

	if (daos_oclass_is_ec(&mrone->mo_oca))
		tgt_off = obj_ec_shard_off_by_layout_ver(mrone->mo_oid.id_layout_ver,
							 mrone->mo_dkey_hash, &mrone->mo_oca,
							 mrone->mo_oid.id_shard);
	for (i = 0; i < mrone->mo_iod_num; i++) {
		daos_iod_t	*iod = &mrone->mo_iods[i];

		if (mrone->mo_iods[i].iod_size == 0) {
			/* zero size iod will cause assertion failure
			 * in VOS, so let's check here.
			 * So the object is being destroyed between
			 * object enumeration and object fetch on
			 * the remote target, which is usually caused
			 * by container destroy or snapshot deletion.
			 * Since this is rare, let's simply return
			 * failure for this rebuild, then reschedule
			 * the rebuild and retry.
			 */
			rc = -DER_DATA_LOSS;
			D_DEBUG(DB_REBUILD,
				DF_RB ": " DF_UOID " %p dkey " DF_KEY " " DF_KEY
				      " nr %d/%d eph " DF_U64 " " DF_RC "\n",
				DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone,
				DP_KEY(&mrone->mo_dkey), DP_KEY(&mrone->mo_iods[i].iod_name),
				mrone->mo_iod_num, i, mrone->mo_epoch, DP_RC(rc));
			D_GOTO(out, rc);
		}

		if (!daos_oclass_is_ec(&mrone->mo_oca))
			continue;

		if (obj_ec_singv_one_tgt(iod->iod_size, &sgls[i], &mrone->mo_oca)) {
			D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " one tgt.\n", DP_RB_MRO(mrone),
				DP_UOID(mrone->mo_oid));
			los[i].cs_even_dist = 0;
			continue;
		}

		if (is_ec_parity_shard_by_layout_ver(mrone->mo_oid.id_layout_ver,
						     mrone->mo_dkey_hash,
						     &mrone->mo_oca, mrone->mo_oid.id_shard)) {
			rc = obj_ec_singv_encode_buf(mrone->mo_oid, mrone->mo_oid.id_layout_ver,
						     &mrone->mo_oca, mrone->mo_dkey_hash,
						     iod, &sgls[i],
						     &sgls[i].sg_iovs[0]);
			if (rc)
				D_GOTO(out, rc);
		} else {
			rc = obj_ec_singv_split(mrone->mo_oid, mrone->mo_oid.id_layout_ver,
						&mrone->mo_oca, mrone->mo_dkey_hash,
						iod->iod_size, &sgls[i]);
			if (rc)
				D_GOTO(out, rc);
		}

		obj_singv_ec_rw_filter(mrone->mo_oid, &mrone->mo_oca, tgt_off, iod,
				       NULL, mrone->mo_epoch, ORF_EC, 1, true, false, NULL);
		los[i].cs_even_dist = 1;
		los[i].cs_bytes = obj_ec_singv_cell_bytes(
					mrone->mo_iods[i].iod_size,
					&mrone->mo_oca);
		los[i].cs_nr = obj_ec_tgt_nr(&mrone->mo_oca);
		D_DEBUG(DB_CSUM, DF_RB ": los[%d]: " DF_LAYOUT "\n", DP_RB_MRO(mrone), i,
			DP_LAYOUT(los[i]));
	}

	csummer = dsc_cont2csummer(dc_obj_hdl2cont_hdl(oh));
	rc = migrate_csum_calc(csummer, mrone, mrone->mo_iods, mrone->mo_iod_num, sgls,
			       p_csum_iov, &iod_csums);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": unable to calculate iod csums", DP_RB_MRO(mrone));
		goto out;
	}

	if (daos_oclass_is_ec(&mrone->mo_oca))
		update_flags |= VOS_OF_EC;

	rc = vos_obj_update(ds_cont->sc_hdl, mrone->mo_oid,
			    mrone->mo_min_epoch, mrone->mo_version,
			    update_flags, &mrone->mo_dkey, mrone->mo_iod_num,
			    mrone->mo_iods, iod_csums, sgls);
out:
	for (i = 0; i < mrone->mo_iod_num; i++) {
		if (iov[i].iov_buf)
			D_FREE(iov[i].iov_buf);

		/* since iod_recxs is being used by single value update somehow,
		 * let's reset it after update.
		 */
		if (mrone->mo_iods[i].iod_type == DAOS_IOD_SINGLE)
			mrone->mo_iods[i].iod_recxs = NULL;
	}

	daos_csummer_free_ic(csummer, &iod_csums);
	daos_iov_free(&csum_iov);

	return rc;
}

static int
__migrate_fetch_update_bulk(struct migrate_one *mrone, daos_handle_t oh,
			    daos_iod_t *iods, int iod_num, daos_epoch_t fetch_eph,
			    daos_epoch_t update_eph,
			    uint32_t flags, struct ds_cont_child *ds_cont)
{
	d_sg_list_t		 sgls[OBJ_ENUM_UNPACK_MAX_IODS];
	daos_handle_t		 ioh;
	int			 rc, rc1, i, sgl_cnt = 0;
	d_iov_t			csum_iov = {0};
	struct daos_csummer	*csummer = NULL;
	struct dcs_iod_csums	*iod_csums = NULL;
	d_iov_t			*p_csum_iov = NULL;
	bool                     stale      = false;

	if (daos_oclass_is_ec(&mrone->mo_oca))
		mrone_recx_daos2_vos(mrone, iods, iod_num);

	D_ASSERT(iod_num <= OBJ_ENUM_UNPACK_MAX_IODS);
	rc = vos_update_begin(ds_cont->sc_hdl, mrone->mo_oid, update_eph, VOS_OF_REBUILD,
			      &mrone->mo_dkey, iod_num, iods, mrone->mo_iods_csums,
			      0, &ioh, NULL);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": " DF_UOID ": preparing update failed", DP_RB_MRO(mrone),
			 DP_UOID(mrone->mo_oid));
		return rc;
	}

	rc = bio_iod_prep(vos_ioh2desc(ioh), BIO_CHK_TYPE_REBUILD, NULL,
			  CRT_BULK_RW);
	if (rc) {
		DL_ERROR(rc, DF_RB ": prepare EIOD for " DF_UOID " error", DP_RB_MRO(mrone),
			 DP_UOID(mrone->mo_oid));
		goto end;
	}

	for (i = 0; i < iod_num; i++) {
		struct bio_sglist	*bsgl;

		bsgl = vos_iod_sgl_at(ioh, i);
		D_ASSERT(bsgl != NULL);

		rc = bio_sgl_convert(bsgl, &sgls[i]);
		if (rc)
			goto post;
		sgl_cnt++;
	}

	D_DEBUG(DB_REBUILD,
		DF_RB ": " DF_UOID " mrone %p dkey " DF_KEY " nr %d eph " DF_X64 "/" DF_X64 "\n",
		DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey), iod_num,
		mrone->mo_epoch, update_eph);

	if (daos_oclass_is_ec(&mrone->mo_oca))
		mrone_recx_vos2_daos(mrone, mrone->mo_oid.id_shard, iods, iod_num);

	if (!daos_oclass_is_ec(&mrone->mo_oca)) {
		rc = daos_iov_alloc(&csum_iov, CSUM_BUF_SIZE, false);
		if (rc != 0)
			D_GOTO(post, rc);
		p_csum_iov = &csum_iov;
	}

	rc = mrone_obj_fetch(mrone, oh, sgls, iods, iod_num, fetch_eph, flags, p_csum_iov);
	if (rc) {
		DL_ERROR(rc, DF_RB ": migrate dkey " DF_KEY " failed", DP_RB_MRO(mrone),
			 DP_KEY(&mrone->mo_dkey));
		D_GOTO(post, rc);
	}

	csummer = dsc_cont2csummer(dc_obj_hdl2cont_hdl(oh));
	rc = migrate_csum_calc(csummer, mrone, iods, iod_num, sgls, p_csum_iov, &iod_csums);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": failed to calculate iod csums", DP_RB_MRO(mrone));
		D_GOTO(post, rc);
	}

	vos_set_io_csum(ioh, iod_csums);
post:
	for (i = 0; i < sgl_cnt; i++)
		d_sgl_fini(&sgls[i], false);

	if (daos_oclass_is_ec(&mrone->mo_oca))
		mrone_recx_daos2_vos(mrone, iods, iod_num);

	rc = bio_iod_post(vos_ioh2desc(ioh), rc);
	if (rc)
		DL_ERROR(rc, DF_RB ": post EIOD for " DF_UOID " error", DP_RB_MRO(mrone),
			 DP_UOID(mrone->mo_oid));

	for (i = 0; rc == 0 && i < iod_num; i++) {
		if (iods[i].iod_size == 0) {
			/* zero size iod will cause assertion failure
			 * in VOS, so let's check here.
			 * So the object is being destroyed between
			 * object enumeration and object fetch on
			 * the remote target, which is usually caused
			 * by container destroy or snapshot deletion.
			 * Since this is rare, let's simply return
			 * failure for this rebuild, then reschedule
			 * the rebuild and retry.
			 */
			rc = -DER_STALE;
			D_INFO(DF_RB ": " DF_UOID " %p dkey " DF_KEY " " DF_KEY
				     " nr %d/%d eph " DF_U64 " " DF_RC "\n",
			       DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid), mrone,
			       DP_KEY(&mrone->mo_dkey), DP_KEY(&iods[i].iod_name), iod_num, i,
			       mrone->mo_epoch, DP_RC(rc));
			stale = true;
			D_GOTO(end, rc);
		}
	}
end:
	rc1 = vos_update_end(ioh, mrone->mo_version, &mrone->mo_dkey, rc, NULL,
			     NULL);
	daos_csummer_free_ic(csummer, &iod_csums);
	daos_iov_free(&csum_iov);
	if (rc == 0)
		rc = rc1;

	if (rc)
		DL_CDEBUG(stale, DLOG_INFO, DLOG_ERR, rc, DF_RB ": " DF_UOID " migrate error",
			  DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid));

	return rc;
}

static int
migrate_fetch_update_bulk(struct migrate_one *mrone, daos_handle_t oh,
			  struct ds_cont_child *ds_cont)
{
	int i;
	int j;
	int rc = 0;

	if (!daos_oclass_is_ec(&mrone->mo_oca))
		return __migrate_fetch_update_bulk(mrone, oh, mrone->mo_iods,
						   mrone->mo_iod_num,
						   mrone->mo_epoch,
						   mrone->mo_min_epoch,
						   DIOF_FOR_MIGRATION, ds_cont);

	/* For EC object, if the migration include both extent from parity rebuild
	 * and extent from replicate rebuild, let rebuild the extent with parity first,
	 * then extent from replication.
	 */
	for (i = 0; i < mrone->mo_iods_num_from_parity; i++) {
		for (j = 0; j < mrone->mo_iods_from_parity[i].iod_nr; j++) {
			daos_iod_t iod = mrone->mo_iods_from_parity[i];
			daos_epoch_t fetch_eph;

			iod.iod_nr = 1;
			iod.iod_recxs = &mrone->mo_iods_from_parity[i].iod_recxs[j];

			/* If the epoch is higher than EC aggregate boundary, then
			 * it should use stable epoch to fetch the data, since
			 * the data could be aggregated independently on parity
			 * and data shard, so using stable epoch could make sure
			 * the consistency view during rebuild. And also EC aggregation
			 * should already aggregate the parity, so there should not
			 * be any partial update on the parity as well.
			 *
			 * Otherwise there might be partial update on this rebuilding
			 * shard, so let's use the epoch from the parity shard to fetch
			 * the data here, which will make sure partial update will not
			 * be fetched here. And also EC aggregation is being disabled
			 * at the moment, so there should not be any vos aggregation
			 * impact this process as well.
			 */
			if (ds_cont->sc_ec_agg_eph_boundary >
			    mrone->mo_iods_update_ephs_from_parity[i][j])
				fetch_eph = mrone->mo_epoch;
			else
				fetch_eph = mrone->mo_iods_update_ephs_from_parity[i][j];
			rc = __migrate_fetch_update_bulk(mrone, oh, &iod, 1, fetch_eph,
						mrone->mo_iods_update_ephs_from_parity[i][j],
						DIOF_EC_RECOV_FROM_PARITY | DIOF_FOR_MIGRATION,
						ds_cont);
			if (rc != 0)
				D_GOTO(out, rc);
		}
	}

	/* The data, rebuilt from replication, needs to keep the same epoch during rebuild,
	 * otherwise it may mess up the relationship between parity epoch vs data shard epoch,
	 * which might cause data corruption during degraded fetch. Since VOS update does not
	 * support multiple epoch, so it can only do fetch/update recx each time.
	 */
	for (i = 0; i < mrone->mo_iod_num; i++) {
		daos_iod_t	iod;

		for (j = 0; j < mrone->mo_iods[i].iod_nr; j++) {
			iod = mrone->mo_iods[i];
			iod.iod_nr = 1;
			iod.iod_recxs = &mrone->mo_iods[i].iod_recxs[j];
			rc = __migrate_fetch_update_bulk(mrone, oh, &iod, 1,
							 mrone->mo_epoch,
							 mrone->mo_iods_update_ephs[i][j],
							 DIOF_FOR_MIGRATION, ds_cont);
			if (rc < 0) {
				if (rc == -DER_VOS_PARTIAL_UPDATE) {
					/* In some cases, EC aggregation failed to delete the
					 * replicate recx, then during rebuild these replicate
					 * recx might be rebuilt again. Since the data rebuilt
					 * from parity will be rebuilt first. so let's ignore
					 * this replicate recx for now.
					 */
					D_WARN(DF_RB ": " DF_UOID " " DF_RECX "/" DF_X64 " already "
						     "rebuilt\n",
					       DP_RB_MRO(mrone), DP_UOID(mrone->mo_oid),
					       DP_RECX(iod.iod_recxs[0]),
					       mrone->mo_iods_update_ephs[i][j]);
					rc = 0;
				} else {
					D_GOTO(out, rc);
				}
			}
		}
	}
out:
	return rc;
}

/**
 * Punch dkeys/akeys before migrate.
 */
static int
migrate_punch(struct migrate_pool_tls *tls, struct migrate_one *mrone,
	      struct ds_cont_child *cont)
{
	int	rc = 0;
	int	i;

	/* Punch dkey */
	if (mrone->mo_dkey_punch_eph != 0 && mrone->mo_dkey_punch_eph <= tls->mpt_max_eph) {
		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " punch dkey " DF_KEY "/" DF_U64 "\n",
			DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), DP_KEY(&mrone->mo_dkey),
			mrone->mo_dkey_punch_eph);
		rc = vos_obj_punch(cont->sc_hdl, mrone->mo_oid,
				   mrone->mo_dkey_punch_eph,
				   tls->mpt_version, VOS_OF_REPLAY_PC,
				   &mrone->mo_dkey, 0, NULL, NULL);
		if (rc) {
			DL_ERROR(rc, DF_RB ": " DF_UOID " punch dkey failed", DP_RB_MPT(tls),
				 DP_UOID(mrone->mo_oid));
			return rc;
		}
	}

	for (i = 0; i < mrone->mo_iod_num; i++) {
		daos_epoch_t eph;

		eph = mrone->mo_akey_punch_ephs[i];
		D_ASSERT(eph != DAOS_EPOCH_MAX);
		if (eph == 0 || eph > tls->mpt_max_eph) {
			D_DEBUG(DB_REBUILD,
				DF_RB ": " DF_UOID " skip mrone %p punch dkey " DF_KEY
				      " akey " DF_KEY " eph " DF_X64 " current " DF_X64 "\n",
				DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone,
				DP_KEY(&mrone->mo_dkey), DP_KEY(&mrone->mo_iods[i].iod_name), eph,
				mrone->mo_epoch);
			continue;
		}

		D_DEBUG(DB_REBUILD,
			DF_RB ": " DF_UOID " mrone %p punch dkey " DF_KEY "akey " DF_KEY
			      " eph " DF_U64 "\n",
			DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey),
			DP_KEY(&mrone->mo_iods[i].iod_name), eph);

		rc = vos_obj_punch(cont->sc_hdl, mrone->mo_oid,
				   eph, tls->mpt_version,
				   VOS_OF_REPLAY_PC, &mrone->mo_dkey,
				   1, &mrone->mo_iods[i].iod_name,
				   NULL);
		if (rc) {
			D_ERROR(DF_UOID" punch akey failed: "DF_RC"\n",
				DP_UOID(mrone->mo_oid), DP_RC(rc));
			return rc;
		}
	}

	/* punch records */
	if (mrone->mo_punch_iod_num > 0 && mrone->mo_rec_punch_eph <= tls->mpt_max_eph) {
		if (daos_oclass_is_ec(&mrone->mo_oca) &&
		    !is_ec_parity_shard_by_layout_ver(mrone->mo_oid.id_layout_ver,
						      mrone->mo_dkey_hash, &mrone->mo_oca,
						      mrone->mo_oid.id_shard))
			mrone_recx_daos2_vos(mrone, mrone->mo_punch_iods, mrone->mo_punch_iod_num);

		rc = vos_obj_update(cont->sc_hdl, mrone->mo_oid,
				    mrone->mo_rec_punch_eph,
				    mrone->mo_version, 0, &mrone->mo_dkey,
				    mrone->mo_punch_iod_num,
				    mrone->mo_punch_iods, NULL, NULL);
		D_DEBUG(DB_REBUILD,
			DF_RB ": " DF_UOID " mrone %p punch %d eph " DF_U64 "records: " DF_RC "\n",
			DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone, mrone->mo_punch_iod_num,
			mrone->mo_rec_punch_eph, DP_RC(rc));
	}

	return rc;
}

static int
migrate_get_cont_child(struct migrate_pool_tls *tls, uuid_t cont_uuid,
		       struct ds_cont_child **cont_p, bool create)
{
	struct ds_cont_child	*cont_child = NULL;
	int			rc;

	*cont_p = NULL;
	if (tls->mpt_pool->spc_pool->sp_stopping) {
		D_DEBUG(DB_REBUILD, DF_RB ": pool is being destroyed.\n", DP_RB_MPT(tls));
		return 0;
	}

	if (create) {
		/* Since the shard might be moved different location for any pool operation,
		 * so it may need create the container in all cases.
		 */
		rc = ds_cont_child_open_create(tls->mpt_pool_uuid, cont_uuid, &cont_child);
		if (rc != 0) {
			if (rc == -DER_SHUTDOWN || (cont_child && cont_child->sc_stopping)) {
				D_DEBUG(DB_REBUILD,
					DF_RB ": container " DF_UUID " is being "
					      "destroyed\n",
					DP_RB_MPT(tls), DP_UUID(cont_uuid));
				rc = 0;
			}
			if (cont_child)
				ds_cont_child_put(cont_child);
			return rc;
		}
	} else {
		rc = ds_cont_child_lookup(tls->mpt_pool_uuid, cont_uuid, &cont_child);
		if (rc != 0 || (cont_child && cont_child->sc_stopping)) {
			if (rc == -DER_NONEXIST || (cont_child && cont_child->sc_stopping)) {
				D_DEBUG(DB_REBUILD,
					DF_RB ": container " DF_UUID " is being "
					      "destroyed\n",
					DP_RB_MPT(tls), DP_UUID(cont_uuid));
				rc = 0;
			}

			if (cont_child)
				ds_cont_child_put(cont_child);
			return rc;
		}
	}

	*cont_p = cont_child;
	return rc;
}

static int
migrate_dkey(struct migrate_pool_tls *tls, struct migrate_one *mrone,
	     daos_size_t data_size)
{
	struct ds_cont_child	*cont = NULL;
	struct cont_props	 props;
	daos_handle_t		 coh = DAOS_HDL_INVAL;
	daos_handle_t		 oh  = DAOS_HDL_INVAL;
	int			 rc;

	D_ASSERT(dss_get_module_info()->dmi_xs_id != 0);
	rc = migrate_get_cont_child(tls, mrone->mo_cont_uuid, &cont, true);
	if (rc || cont == NULL)
		D_GOTO(cont_put, rc);

	rc = dsc_pool_open(tls->mpt_pool_uuid, tls->mpt_poh_uuid, 0,
			   NULL, tls->mpt_pool->spc_pool->sp_map,
			   &tls->mpt_svc_list, &tls->mpt_pool_hdl);
	if (rc)
		D_GOTO(cont_put, rc);

	/* Open client dc handle used to read the remote object data */
	rc = migrate_cont_open(tls, mrone->mo_cont_uuid, 0, &coh);
	if (rc)
		D_GOTO(cont_put, rc);

	/* Open the remote object */
	rc = dsc_obj_open(coh, mrone->mo_oid.id_pub, DAOS_OO_RO, &oh);
	if (rc)
		D_GOTO(cont_put, rc);

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_TGT_NOSPACE))
		D_GOTO(obj_close, rc = -DER_NOSPACE);

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_NO_REBUILD)) {
		D_DEBUG(DB_REBUILD, DF_RB ": fault injected, disable rebuild\n", DP_RB_MPT(tls));
		D_GOTO(obj_close, rc);
	}

	dsc_cont_get_props(coh, &props);
	rc = dsc_obj_id2oc_attr(mrone->mo_oid.id_pub, &props, &mrone->mo_oca);
	if (rc) {
		D_ERROR(DF_RB ": unknown object class: %u\n", DP_RB_MPT(tls),
			daos_obj_id2class(mrone->mo_oid.id_pub));
		D_GOTO(obj_close, rc);
	}

	/* punch the object */
	if (mrone->mo_obj_punch_eph) {
		rc = vos_obj_punch(cont->sc_hdl, mrone->mo_oid,
				   mrone->mo_obj_punch_eph,
				   tls->mpt_version, VOS_OF_REPLAY_PC,
				   NULL, 0, NULL, NULL);
		if (rc) {
			DL_ERROR(rc, DF_RB ": " DF_UOID " punch obj failed", DP_RB_MPT(tls),
				 DP_UOID(mrone->mo_oid));
			D_GOTO(obj_close, rc);
		}
	}

	rc = migrate_punch(tls, mrone, cont);
	if (rc)
		D_GOTO(obj_close, rc);

	if (data_size == 0) {
		D_DEBUG(DB_REBUILD, DF_RB ": empty mrone %p\n", DP_RB_MPT(tls), mrone);
		D_GOTO(obj_close, rc);
	}

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_UPDATE_FAIL))
		D_GOTO(obj_close, rc = -DER_INVAL);

	if (mrone->mo_iods[0].iod_type == DAOS_IOD_SINGLE)
		rc = migrate_fetch_update_single(mrone, oh, cont);
	else if (daos_oclass_is_ec(&mrone->mo_oca) &&
		 is_ec_parity_shard_by_layout_ver(mrone->mo_oid.id_layout_ver,
						  mrone->mo_dkey_hash, &mrone->mo_oca,
						  mrone->mo_oid.id_shard))
		rc = migrate_fetch_update_parity(mrone, oh, cont);
	else if (data_size < MAX_BUF_SIZE || data_size == (daos_size_t)(-1))
		rc = migrate_fetch_update_inline(mrone, oh, cont);
	else
		rc = migrate_fetch_update_bulk(mrone, oh, cont);

	tls->mpt_rec_count += mrone->mo_rec_num;
	tls->mpt_size += mrone->mo_size;
obj_close:
	dsc_obj_close(oh);
cont_put:
	if (cont != NULL)
		ds_cont_child_put(cont);
	return rc;
}

static void
migrate_one_destroy(struct migrate_one *mrone)
{
	int i;

	D_ASSERT(d_list_empty(&mrone->mo_list));
	daos_iov_free(&mrone->mo_dkey);

	if (mrone->mo_iods_update_ephs) {
		for (i = 0; i < mrone->mo_iod_alloc_num; i++) {
			if (mrone->mo_iods_update_ephs[i])
				D_FREE(mrone->mo_iods_update_ephs[i]);
		}
		D_FREE(mrone->mo_iods_update_ephs);
	}

	if (mrone->mo_iods_update_ephs_from_parity) {
		for (i = 0; i < mrone->mo_iod_alloc_num; i++) {
			if (mrone->mo_iods_update_ephs_from_parity[i])
				D_FREE(mrone->mo_iods_update_ephs_from_parity[i]);
		}
		D_FREE(mrone->mo_iods_update_ephs_from_parity);
	}

	if (mrone->mo_iods)
		daos_iods_free(mrone->mo_iods, mrone->mo_iod_alloc_num, true);

	if (mrone->mo_iods_from_parity)
		daos_iods_free(mrone->mo_iods_from_parity, mrone->mo_iod_alloc_num, true);

	if (mrone->mo_punch_iods)
		daos_iods_free(mrone->mo_punch_iods, mrone->mo_iod_alloc_num, true);

	if (mrone->mo_akey_punch_ephs)
		D_FREE(mrone->mo_akey_punch_ephs);

	if (mrone->mo_sgls) {
		for (i = 0; i < mrone->mo_iod_alloc_num; i++)
			d_sgl_fini(&mrone->mo_sgls[i], true);
		D_FREE(mrone->mo_sgls);
	}

	if (mrone->mo_iods_csums)
		D_FREE(mrone->mo_iods_csums);

	D_FREE(mrone);
}

enum {
	OBJ_ULT = 1,
	DKEY_ULT = 2,
};

/* Check if there are enough resource for the migration to proceed. */
static int
migrate_system_enter(struct migrate_pool_tls *tls, int tgt_idx, bool *yielded)
{
	uint32_t tgt_cnt = 0;
	int	 rc = 0;

	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);
	D_ASSERTF(tgt_idx < dss_tgt_nr, "tgt idx %d tgt nr %u\n", tgt_idx, dss_tgt_nr);

	tgt_cnt = atomic_load(&tls->mpt_obj_ult_cnts[tgt_idx]) +
		  atomic_load(&tls->mpt_dkey_ult_cnts[tgt_idx]);

	while ((tls->mpt_inflight_max_ult / dss_tgt_nr) <= tgt_cnt) {
		D_DEBUG(DB_REBUILD, DF_RB ": tgt%d:%u max %u\n", DP_RB_MPT(tls), tgt_idx, tgt_cnt,
			tls->mpt_inflight_max_ult / dss_tgt_nr);
		*yielded = true;
		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_wait(tls->mpt_inflight_cond, tls->mpt_inflight_mutex);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
		if (tls->mpt_fini)
			D_GOTO(out, rc = -DER_SHUTDOWN);

		tgt_cnt = atomic_load(&tls->mpt_obj_ult_cnts[tgt_idx]) +
			  atomic_load(&tls->mpt_dkey_ult_cnts[tgt_idx]);
	}

	atomic_fetch_add(&tls->mpt_obj_ult_cnts[tgt_idx], 1);
out:
	return rc;
}

static int
migrate_tgt_enter(struct migrate_pool_tls *tls)
{
	uint32_t dkey_cnt = 0;
	int	 rc = 0;

	D_ASSERT(dss_get_module_info()->dmi_xs_id != 0);

	dkey_cnt = atomic_load(tls->mpt_tgt_dkey_ult_cnt);
	while (tls->mpt_inflight_max_ult / 2 <= dkey_cnt) {
		D_DEBUG(DB_REBUILD, DF_RB ": tgt %u max %u\n", DP_RB_MPT(tls), dkey_cnt,
			tls->mpt_inflight_max_ult);

		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_wait(tls->mpt_inflight_cond, tls->mpt_inflight_mutex);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
		if (tls->mpt_fini)
			D_GOTO(out, rc = -DER_SHUTDOWN);

		dkey_cnt = atomic_load(tls->mpt_tgt_dkey_ult_cnt);
	}

	atomic_fetch_add(tls->mpt_tgt_dkey_ult_cnt, 1);
out:
	return rc;
}

static void
migrate_system_try_wakeup(struct migrate_pool_tls *tls)
{
	bool wakeup = false;
	int i;

	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);
	for (i = 0; i < dss_tgt_nr; i++) {
		uint32_t total_cnt;

		total_cnt = atomic_load(&tls->mpt_obj_ult_cnts[i]) +
			    atomic_load(&tls->mpt_dkey_ult_cnts[i]);
		if (tls->mpt_inflight_max_ult / dss_tgt_nr > total_cnt)
			wakeup = true;
	}

	if (wakeup) {
		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_broadcast(tls->mpt_inflight_cond);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
	}
}

static void
migrate_system_exit(struct migrate_pool_tls *tls, unsigned int tgt_idx)
{
	/* NB: this will only be called during errr handling. In normal case
	 * the migrate ULT created by system will be exit on each target XS.
	 */
	D_ASSERT(dss_get_module_info()->dmi_xs_id == 0);
	atomic_fetch_sub(&tls->mpt_obj_ult_cnts[tgt_idx], 1);
	migrate_system_try_wakeup(tls);
}

static void
migrate_tgt_try_wakeup(struct migrate_pool_tls *tls)
{
	uint32_t dkey_cnt = 0;

	D_ASSERT(dss_get_module_info()->dmi_xs_id != 0);
	dkey_cnt = atomic_load(tls->mpt_tgt_dkey_ult_cnt);
	if (tls->mpt_inflight_max_ult / 2 > dkey_cnt) {
		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_broadcast(tls->mpt_inflight_cond);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
	}
}

static void
migrate_tgt_exit(struct migrate_pool_tls *tls, int ult_type)
{
	D_ASSERT(dss_get_module_info()->dmi_xs_id != 0);
	if (ult_type == OBJ_ULT) {
		atomic_fetch_sub(tls->mpt_tgt_obj_ult_cnt, 1);
		return;
	}

	atomic_fetch_sub(tls->mpt_tgt_dkey_ult_cnt, 1);
	migrate_tgt_try_wakeup(tls);
}

static void
migrate_one_ult(void *arg)
{
	struct migrate_one	*mrone = arg;
	struct migrate_pool_tls	*tls;
	daos_size_t		data_size;
	int			rc = 0;

	while (daos_fail_check(DAOS_REBUILD_TGT_REBUILD_HANG))
		dss_sleep(0);

	tls = migrate_pool_tls_lookup(mrone->mo_pool_uuid,
				      mrone->mo_pool_tls_version, mrone->mo_generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(mrone->mo_pool_uuid));
		goto out;
	}

	data_size = daos_iods_len(mrone->mo_iods, mrone->mo_iod_num);
	data_size += daos_iods_len(mrone->mo_iods_from_parity,
				   mrone->mo_iods_num_from_parity);

	D_DEBUG(DB_TRACE, DF_RB ": mrone %p data size is " DF_U64 " %d/%d\n", DP_RB_MPT(tls), mrone,
		data_size, mrone->mo_iod_num, mrone->mo_iods_num_from_parity);

	D_ASSERT(data_size != (daos_size_t)-1);
	D_DEBUG(DB_REBUILD, DF_RB ": mrone %p inflight_size " DF_U64 " max " DF_U64 "\n",
		DP_RB_MPT(tls), mrone, tls->mpt_inflight_size, tls->mpt_inflight_max_size);

	while (tls->mpt_inflight_size + data_size >= tls->mpt_inflight_max_size &&
	       tls->mpt_inflight_max_size != 0 && tls->mpt_inflight_size != 0 &&
	       !tls->mpt_fini) {
		D_DEBUG(DB_REBUILD, DF_RB ": mrone %p wait " DF_U64 "/" DF_U64 "/" DF_U64 "\n",
			DP_RB_MPT(tls), mrone, tls->mpt_inflight_size, tls->mpt_inflight_max_size,
			data_size);
		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_wait(tls->mpt_inflight_cond, tls->mpt_inflight_mutex);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
	}

	if (tls->mpt_fini)
		D_GOTO(out, rc);

	tls->mpt_inflight_size += data_size;
	rc = migrate_dkey(tls, mrone, data_size);
	tls->mpt_inflight_size -= data_size;

	D_DEBUG(DB_REBUILD,
		DF_RB ": " DF_UOID " layout %u migrate dkey " DF_KEY " inflight_size " DF_U64
		      ": " DF_RC "\n",
		DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone->mo_oid.id_layout_ver,
		DP_KEY(&mrone->mo_dkey), tls->mpt_inflight_size, DP_RC(rc));

	/* Ignore nonexistent error because puller could race
	 * with user's container destroy:
	 * - puller got the container+oid from a remote scanner
	 * - user destroyed the container
	 * - puller try to open container or pulling data
	 *   (nonexistent)
	 * This is just a workaround...
	 */
	if (rc != -DER_NONEXIST && rc != -DER_DATA_LOSS && tls->mpt_status == 0)
		tls->mpt_status = rc;
out:
	migrate_one_destroy(mrone);
	if (tls != NULL) {
		migrate_tgt_exit(tls, DKEY_ULT);
		migrate_pool_tls_put(tls);
	}
}

/* If src_iod is NULL, it will try to merge the recxs inside dst_iod */
static int
migrate_merge_iod_recx(daos_iod_t *dst_iod, uint64_t boundary, daos_epoch_t **p_dst_ephs,
		       daos_recx_t *new_recxs, daos_epoch_t *new_ephs, int new_recxs_nr)
{
	struct obj_auxi_list_recx	*recx;
	struct obj_auxi_list_recx	*tmp;
	daos_epoch_t	*dst_ephs;
	daos_recx_t	*recxs;
	d_list_t	merge_list;
	int		nr_recxs = 0;
	int		i;
	int		rc = 0;

	dst_ephs = p_dst_ephs ? *p_dst_ephs : NULL;
	D_INIT_LIST_HEAD(&merge_list);
	for (i = 0; i < new_recxs_nr; i++) {
		D_DEBUG(DB_REBUILD, "src merge "DF_U64"/"DF_U64" eph "DF_X64"\n",
			new_recxs[i].rx_idx, new_recxs[i].rx_nr, new_ephs ? new_ephs[i] : 0);
		rc = merge_recx(&merge_list, new_recxs[i].rx_idx,
				new_recxs[i].rx_nr, new_ephs ? new_ephs[i] : 0, boundary);
		if (rc)
			D_GOTO(out, rc);
	}

	D_ASSERT(dst_iod != NULL);
	recxs = dst_iod->iod_recxs;
	for (i = 0; i < dst_iod->iod_nr; i++) {
		D_DEBUG(DB_REBUILD, "dst merge "DF_U64"/"DF_U64" %p eph "DF_X64"\n",
			recxs[i].rx_idx, recxs[i].rx_nr, dst_ephs, dst_ephs ? dst_ephs[i] : 0);
		rc = merge_recx(&merge_list, recxs[i].rx_idx, recxs[i].rx_nr,
				dst_ephs ? dst_ephs[i] : 0, boundary);
		if (rc)
			D_GOTO(out, rc);
	}

	d_list_for_each_entry(recx, &merge_list, recx_list)
		nr_recxs++;

	if (nr_recxs > dst_iod->iod_nr) {
		D_ALLOC_ARRAY(recxs, nr_recxs);
		if (recxs == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		if (p_dst_ephs != NULL) {
			D_ALLOC_ARRAY(dst_ephs, nr_recxs);
			if (dst_ephs == NULL) {
				D_FREE(recxs);
				D_GOTO(out, rc = -DER_NOMEM);
			}
		}
	} else {
		recxs = dst_iod->iod_recxs;
	}

	i = 0;
	d_list_for_each_entry(recx, &merge_list, recx_list) {
		recxs[i] = recx->recx;
		if (dst_ephs)
			dst_ephs[i] = recx->recx_eph;
		i++;
		D_DEBUG(DB_REBUILD, "%d merge recx "DF_U64"/"DF_U64" %p "DF_X64"\n",
			i - 1, recx->recx.rx_idx, recx->recx.rx_nr, dst_ephs,
			recx->recx_eph);
	}

	if (dst_iod->iod_recxs != recxs)
		D_FREE(dst_iod->iod_recxs);

	if (p_dst_ephs && dst_ephs != *p_dst_ephs) {
		D_FREE(*p_dst_ephs);
		*p_dst_ephs = dst_ephs;
	}

	dst_iod->iod_recxs = recxs;
	dst_iod->iod_nr = i;
out:
	d_list_for_each_entry_safe(recx, tmp, &merge_list, recx_list) {
		d_list_del(&recx->recx_list);
		D_FREE(recx);
	}
	return rc;
}

/* Merge new_iod/new_recx/new_ephs into iods which assume @iods has enough space. */
static int
migrate_insert_recxs_sgl(daos_iod_t *iods, daos_epoch_t **iods_ephs, uint32_t *iods_num,
			 daos_iod_t *new_iod, daos_recx_t *new_recxs,
			 daos_epoch_t *new_ephs, int new_recxs_nr, d_sg_list_t *sgls,
			 d_sg_list_t *new_sgl, uint64_t boundary)
{
	int	   rc = 0;
	int	   i;

	for (i = 0; i < *iods_num; i++) {
		if (daos_iov_cmp(&iods[i].iod_name, &new_iod->iod_name))
			break;
	}

	/* This IOD already exists, let's check if iod_size and type matched */
	if (iods[i].iod_type != DAOS_IOD_NONE &&
	    (iods[i].iod_size != new_iod->iod_size ||
	     iods[i].iod_type != new_iod->iod_type ||
	     iods[i].iod_type == DAOS_IOD_SINGLE)) {
		D_ERROR(DF_KEY" dst_iod size "DF_U64" != "DF_U64
			" dst_iod type %d != %d\n",
			DP_KEY(&new_iod->iod_name), iods[i].iod_size,
			new_iod->iod_size, iods[i].iod_type,
			new_iod->iod_type);
		D_GOTO(out, rc = -DER_INVAL);
	}

	/* Insert new IOD */
	if (iods[i].iod_type == DAOS_IOD_NONE) {
		D_ASSERT(i == *iods_num);
		rc = daos_iov_copy(&iods[i].iod_name, &new_iod->iod_name);
		if (rc)
			D_GOTO(out, rc);
		iods[i].iod_type = new_iod->iod_type;
		iods[i].iod_size = new_iod->iod_size;

		if (new_sgl) {
			rc = daos_sgl_alloc_copy_data(&sgls[i], new_sgl);
			if (rc)
				D_GOTO(out, rc);
		}
		(*iods_num)++;
	}

	if (new_iod->iod_type == DAOS_IOD_SINGLE) {
		iods[i].iod_recxs = NULL;
		if (iods_ephs != NULL) {
			D_ALLOC_ARRAY(iods_ephs[i], 1);
			if (iods_ephs[i] == NULL)
				D_GOTO(out, rc = -DER_NOMEM);
			iods_ephs[i][0] = new_ephs[0];
			iods[i].iod_nr = new_recxs_nr;
		}
	} else {
		rc = migrate_merge_iod_recx(&iods[i], boundary, iods_ephs ? &iods_ephs[i] : NULL,
					    new_recxs, new_ephs, new_recxs_nr);
	}

out:
	D_DEBUG(DB_REBUILD, "Merge akey "DF_KEY" at %d: %d\n",
		DP_KEY(&new_iod->iod_name), i, rc);

	return rc;
}

static int
rw_iod_pack(struct migrate_one *mrone, struct dc_object *obj, daos_iod_t *iod,
	    daos_epoch_t *ephs, d_sg_list_t *sgl)
{
	uint64_t total_size = 0;
	int	 rec_cnt = 0;
	int	 i;
	int	 rc = 0;

	D_ASSERT(iod->iod_size > 0);

	if (sgl && mrone->mo_sgls == NULL) {
		D_ASSERT(mrone->mo_iod_alloc_num > 0);
		D_ALLOC_ARRAY(mrone->mo_sgls, mrone->mo_iod_alloc_num);
		if (mrone->mo_sgls == NULL)
			return -DER_NOMEM;
	}

	if (iod->iod_type == DAOS_IOD_SINGLE) {
		rec_cnt = 1;
		total_size = iod->iod_size;
		D_DEBUG(DB_REBUILD, DF_RB ": single recx " DF_U64 "\n", DP_RB_MRO(mrone),
			total_size);
		rc = migrate_insert_recxs_sgl(mrone->mo_iods, mrone->mo_iods_update_ephs,
					      &mrone->mo_iod_num, iod, &iod->iod_recxs[0],
					      &ephs[0], 1, mrone->mo_sgls, sgl, 0);
		if (rc != 0)
			D_GOTO(out, rc);
	} else {
		uint64_t	boundary = 0;
		int		parity_nr = 0;
		int		nr = 0;
		int		start = 0;

		if (obj_is_ec(obj))
			boundary = obj_ec_stripe_rec_nr(&obj->cob_oca);

		/* For EC object, let's separate the parity rebuild and replicate rebuild to
		 * make sure both extents are being rebuilt individually.
		 */
		for (i = 0; i < iod->iod_nr; i++) {
			rec_cnt += iod->iod_recxs[i].rx_nr;
			total_size += iod->iod_recxs[i].rx_nr * iod->iod_size;
			if (iod->iod_recxs[i].rx_idx & PARITY_INDICATOR) {
				if (nr > 0) {
					/* Once there are parity extents, let's add previous
					 * accumulated replicate extents.
					 **/
					rc = migrate_insert_recxs_sgl(mrone->mo_iods,
								      mrone->mo_iods_update_ephs,
								      &mrone->mo_iod_num, iod,
								      &iod->iod_recxs[start],
								      &ephs[start], nr,
								      mrone->mo_sgls, sgl,
								      boundary);
					if (rc)
						D_GOTO(out, rc);
					start = i;
					nr = 0;
				}
				parity_nr++;
				D_DEBUG(DB_REBUILD,
					DF_RB ": parity recx " DF_X64 "/" DF_X64 " %d/%d\n",
					DP_RB_MRO(mrone), iod->iod_recxs[i].rx_idx,
					iod->iod_recxs[i].rx_nr, parity_nr, nr);
				iod->iod_recxs[i].rx_idx = iod->iod_recxs[i].rx_idx &
							    ~PARITY_INDICATOR;
			} else {
				if (parity_nr > 0) {
					/* Once there are replicate extents, let's add previous
					 * accumulated parity extents
					 **/
					rc = migrate_insert_recxs_sgl(
							mrone->mo_iods_from_parity,
							mrone->mo_iods_update_ephs_from_parity,
							&mrone->mo_iods_num_from_parity,
							iod, &iod->iod_recxs[start],
							&ephs[start], parity_nr,
							mrone->mo_sgls, sgl, boundary);
					if (rc)
						D_GOTO(out, rc);
					start = i;
					parity_nr = 0;
				}
				nr++;
				D_DEBUG(DB_REBUILD,
					DF_RB ": replicate recx " DF_X64 "/" DF_X64 " %d/%d\n",
					DP_RB_MRO(mrone), iod->iod_recxs[i].rx_idx,
					iod->iod_recxs[i].rx_nr, parity_nr, nr);
			}
		}

		if (parity_nr > 0) {
			rc = migrate_insert_recxs_sgl(mrone->mo_iods_from_parity,
						      mrone->mo_iods_update_ephs_from_parity,
						      &mrone->mo_iods_num_from_parity, iod,
						      &iod->iod_recxs[start],
						      &ephs[start], parity_nr,
						      mrone->mo_sgls, sgl, boundary);
			if (rc)
				D_GOTO(out, rc);
		}

		if (nr > 0) {
			rc = migrate_insert_recxs_sgl(mrone->mo_iods, mrone->mo_iods_update_ephs,
						      &mrone->mo_iod_num, iod,
						      &iod->iod_recxs[start], &ephs[start], nr,
						      mrone->mo_sgls, sgl, boundary);
			if (rc)
				D_GOTO(out, rc);
		}
	}

	mrone->mo_rec_num += rec_cnt;
	mrone->mo_size += total_size;
out:
	D_DEBUG(DB_REBUILD,
		DF_RB ": idx %d akey " DF_KEY " nr %d size " DF_U64 " type %d rec %d total " DF_U64
		      "\n",
		DP_RB_MRO(mrone), mrone->mo_iod_num - 1, DP_KEY(&iod->iod_name), iod->iod_nr,
		iod->iod_size, iod->iod_type, rec_cnt, total_size);

	return rc;
}

static int
punch_iod_pack(struct migrate_one *mrone, struct dc_object *obj, daos_iod_t *iod, daos_epoch_t eph)
{
	uint64_t	boundary = 0;
	int		idx = mrone->mo_punch_iod_num;
	int		rc;

	D_ASSERT(iod->iod_size == 0);

	if (mrone->mo_punch_iods == NULL) {
		D_ALLOC_ARRAY(mrone->mo_punch_iods, mrone->mo_iod_alloc_num);
		if (mrone->mo_punch_iods == NULL)
			return -DER_NOMEM;
	}

	if (obj_is_ec(obj))
		boundary = obj_ec_stripe_rec_nr(&obj->cob_oca);

	rc = migrate_insert_recxs_sgl(mrone->mo_punch_iods, NULL, &mrone->mo_punch_iod_num,
				      iod, iod->iod_recxs, NULL, iod->iod_nr, NULL, NULL, boundary);
	if (rc != 0)
		D_GOTO(out, rc);

	D_DEBUG(DB_TRACE,
		"idx %d akey "DF_KEY" nr %d size "DF_U64" type %d\n",
		idx, DP_KEY(&iod->iod_name), mrone->mo_punch_iods->iod_nr, iod->iod_size,
		iod->iod_type);

	if (mrone->mo_rec_punch_eph < eph)
		mrone->mo_rec_punch_eph = eph;
out:
	return rc;
}

static int
migrate_one_insert_recx(struct migrate_one *mrone, struct dc_object *obj, daos_iod_t *iod,
			daos_epoch_t *recx_ephs, daos_epoch_t punch_eph, d_sg_list_t *sgl)
{
	int i;

	if (iod->iod_size == 0)
		return punch_iod_pack(mrone, obj, iod, punch_eph);

	/* update the minimum epoch for this migrate one */
	for (i = 0; i < iod->iod_nr; i++) {
		if (recx_ephs[i] != 0)
			mrone->mo_min_epoch = min(mrone->mo_min_epoch, recx_ephs[i]);
	}

	return rw_iod_pack(mrone, obj, iod, recx_ephs, sgl);
}

/*
 * Try to merge recx from unpack IO into existing migrate IODs.
 *
 * return 0 means all recxs of the IOD are merged.
 * return 1 means not all recxs of the IOD are merged.
 */
static int
migrate_try_merge_recx(struct migrate_one *mo, struct dc_object *obj,
		       struct dc_obj_enum_unpack_io *io)
{
	bool	all_merged = true;
	int	i;
	int	rc = 0;

	for (i = 0; i <= io->ui_iods_top; i++) {
		int j;

		if (io->ui_iods[i].iod_nr == 0)
			continue;

		for (j = 0; j < mo->mo_iod_num; j++) {
			if (mo->mo_iods[j].iod_type == DAOS_IOD_SINGLE)
				continue;

			if (!daos_iov_cmp(&mo->mo_iods[j].iod_name,
					  &io->ui_iods[i].iod_name))
				continue;

			rc = migrate_one_insert_recx(mo, obj, &io->ui_iods[i],
						     io->ui_recx_ephs[i],
						     io->ui_rec_punch_ephs[i], NULL);
			if (rc)
				D_GOTO(out, rc);

			/* If recxs can be merged to other iods, then
			 * it do not need to be processed anymore
			 */
			io->ui_iods[i].iod_nr = 0;
			break;
		}
		if (j == mo->mo_iod_num)
			all_merged = false;
	}

	if (!all_merged)
		rc = 1;
out:
	return rc;
}

struct enum_unpack_arg {
	struct iter_obj_arg	*arg;
	daos_handle_t		oh;
	struct daos_oclass_attr	oc_attr;
	daos_epoch_range_t	epr;
	d_list_t		merge_list;
	uint32_t		version;
	uint32_t		new_layout_ver;	/* New layout version for upgrade */
};

static int
migrate_one_create(struct enum_unpack_arg *arg, struct dc_obj_enum_unpack_io *io)
{
	struct iter_obj_arg	*iter_arg = arg->arg;
	daos_unit_oid_t		oid = io->ui_oid;
	daos_key_t		*dkey = &io->ui_dkey;
	daos_epoch_t		dkey_punch_eph = io->ui_dkey_punch_eph;
	daos_epoch_t		obj_punch_eph = io->ui_obj_punch_eph;
	daos_iod_t		*iods = io->ui_iods;
	daos_epoch_t		*akey_punch_ephs = io->ui_akey_punch_ephs;
	daos_epoch_t		*rec_punch_ephs = io->ui_rec_punch_ephs;
	int			iod_eph_total = io->ui_iods_top + 1;
	d_sg_list_t		*sgls = io->ui_sgls;
	uint32_t		version = io->ui_version;
	struct dc_object	*obj = NULL;
	struct migrate_pool_tls *tls;
	struct migrate_one	*mrone = NULL;
	bool			inline_copy = true;
	int			i;
	int			rc = 0;

	tls = migrate_pool_tls_lookup(iter_arg->pool_uuid, iter_arg->version, iter_arg->generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "dkey " DF_KEY "iod_nr %d\n",
		       DP_UUID(iter_arg->pool_uuid), DP_KEY(dkey), iod_eph_total);
		D_GOTO(put, rc = 0);
	}
	D_DEBUG(DB_REBUILD, DF_RB ": migrate dkey " DF_KEY " iod nr %d\n", DP_RB_MPT(tls),
		DP_KEY(dkey), iod_eph_total);
	if (iod_eph_total == 0 || tls->mpt_fini) {
		D_DEBUG(DB_REBUILD, DF_RB ": no need eph_total %d version %u fini %d\n",
			DP_RB_MPT(tls), iod_eph_total, version, tls->mpt_fini);
		D_GOTO(put, rc = 0);
	}

	D_ALLOC_PTR(mrone);
	if (mrone == NULL)
		D_GOTO(put, rc = -DER_NOMEM);

	D_INIT_LIST_HEAD(&mrone->mo_list);
	D_ALLOC_ARRAY(mrone->mo_iods, iod_eph_total);
	if (mrone->mo_iods == NULL)
		D_GOTO(free, rc = -DER_NOMEM);

	D_ALLOC_ARRAY(mrone->mo_iods_update_ephs, iod_eph_total);
	if (mrone->mo_iods_update_ephs == NULL)
		D_GOTO(free, rc = -DER_NOMEM);

	if (daos_oclass_is_ec(&arg->oc_attr)) {
		D_ALLOC_ARRAY(mrone->mo_iods_from_parity, iod_eph_total);
		if (mrone->mo_iods_from_parity == NULL)
			D_GOTO(free, rc = -DER_NOMEM);

		D_ALLOC_ARRAY(mrone->mo_iods_update_ephs_from_parity, iod_eph_total);
		if (mrone->mo_iods_update_ephs_from_parity == NULL)
			D_GOTO(free, rc = -DER_NOMEM);
	}

	mrone->mo_epoch = arg->epr.epr_hi;
	mrone->mo_obj_punch_eph = obj_punch_eph;
	mrone->mo_dkey_punch_eph = dkey_punch_eph;
	D_ALLOC_ARRAY(mrone->mo_akey_punch_ephs, iod_eph_total);
	if (mrone->mo_akey_punch_ephs == NULL)
		D_GOTO(free, rc = -DER_NOMEM);

	rc = daos_iov_copy(&mrone->mo_dkey, dkey);
	if (rc != 0)
		D_GOTO(free, rc);

	obj = obj_hdl2ptr(arg->oh);
	mrone->mo_oid = oid;
	if (tls->mpt_opc == RB_OP_UPGRADE)
		mrone->mo_oid.id_layout_ver = tls->mpt_new_layout_ver;
	else
		mrone->mo_oid.id_layout_ver = obj->cob_layout_version;

	mrone->mo_oid.id_shard = iter_arg->shard;
	uuid_copy(mrone->mo_cont_uuid, iter_arg->cont_uuid);
	uuid_copy(mrone->mo_pool_uuid, tls->mpt_pool_uuid);
	mrone->mo_pool_tls_version = tls->mpt_version;
	mrone->mo_iod_alloc_num = iod_eph_total;
	mrone->mo_min_epoch = DAOS_EPOCH_MAX;
	mrone->mo_version = version;
	mrone->mo_generation = tls->mpt_generation;
	mrone->mo_dkey_hash = io->ui_dkey_hash;
	mrone->mo_layout_version = obj->cob_layout_version;
	mrone->mo_opc              = tls->mpt_opc;
	/* only do the copy below when each with inline recx data */
	for (i = 0; i < iod_eph_total; i++) {
		int j;

		if (sgls[i].sg_nr == 0 || sgls[i].sg_iovs == NULL) {
			inline_copy = false;
			break;
		}

		for (j = 0; j < sgls[i].sg_nr; j++) {
			if (sgls[i].sg_iovs[j].iov_len == 0 ||
			    sgls[i].sg_iovs[j].iov_buf == NULL) {
				inline_copy = false;
				break;
			}
		}

		if (!inline_copy)
			break;
	}

	for (i = 0; i < iod_eph_total; i++) {
		if (akey_punch_ephs[i] != 0) {
			mrone->mo_akey_punch_ephs[i] = akey_punch_ephs[i];
			D_DEBUG(DB_REBUILD, DF_RB ": punched %d akey " DF_KEY " " DF_U64 "\n",
				DP_RB_MPT(tls), i, DP_KEY(&iods[i].iod_name), akey_punch_ephs[i]);
		}

		if (iods[i].iod_nr == 0)
			continue;

		rc = migrate_one_insert_recx(mrone, obj, &io->ui_iods[i], io->ui_recx_ephs[i],
					     rec_punch_ephs[i], inline_copy ? &sgls[i] : NULL);
		if (rc) {
			obj_decref(obj);
			D_GOTO(free, rc);
		}
	}
	obj_decref(obj);

	if (inline_copy) {
		rc = daos_iov_copy(&mrone->mo_csum_iov, &io->ui_csum_iov);
		if (rc != 0)
			D_GOTO(free, rc);
	}

	D_DEBUG(DB_REBUILD,
		DF_RB ": " DF_UOID " %p dkey " DF_KEY " migrate on idx %d iod_num %d "
		      "min eph " DF_U64 " ver %u\n",
		DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone, DP_KEY(dkey), iter_arg->tgt_idx,
		mrone->mo_iod_num, mrone->mo_min_epoch, version);

	d_list_add(&mrone->mo_list, &arg->merge_list);

free:
	if (rc != 0 && mrone != NULL) {
		d_list_del_init(&mrone->mo_list);
		migrate_one_destroy(mrone);
	}
put:
	if (tls)
		migrate_pool_tls_put(tls);
	return rc;
}

static int
migrate_enum_unpack_cb(struct dc_obj_enum_unpack_io *io, void *data)
{
	struct enum_unpack_arg	*arg = data;
	uint32_t		shard = arg->arg->shard;
	struct migrate_one	*mo;
	uint32_t		unpack_tgt_off;
	uint32_t		migrate_tgt_off;
	bool			merged = false;
	bool			create_migrate_one = false;
	int			rc = 0;
	struct migrate_pool_tls *tls;
	struct dc_object	*obj = NULL;
	uint32_t		parity_shard = -1;
	uint32_t		layout_ver;
	int			i;

	if (!daos_oclass_is_ec(&arg->oc_attr))
		return migrate_one_create(arg, io);

	if (!daos_oclass_is_valid(daos_obj_id2class(io->ui_oid.id_pub))) {
		D_WARN("Skip invalid "DF_UOID".\n", DP_UOID(io->ui_oid));
		return 0;
	}

	/**
	 * If parity shard alive for this dkey, then ignore the data shard enumeration
	 * from data shard.
	 */
	rc = obj_ec_parity_alive(arg->oh, io->ui_dkey_hash, &parity_shard);
	if (rc < 0)
		return rc;

	tls = migrate_pool_tls_lookup(arg->arg->pool_uuid, arg->arg->version,
				      arg->arg->generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(arg->arg->pool_uuid));
		D_GOTO(put, rc = 0);
	}

	obj = obj_hdl2ptr(arg->oh);
	if (tls->mpt_opc == RB_OP_UPGRADE)
		layout_ver = arg->new_layout_ver;
	else
		layout_ver = obj->cob_layout_version;

	migrate_tgt_off = obj_ec_shard_off_by_layout_ver(layout_ver, io->ui_dkey_hash,
							 &arg->oc_attr, shard);
	unpack_tgt_off = obj_ec_shard_off(obj, io->ui_dkey_hash, io->ui_oid.id_shard);
	if (rc == 1 &&
	     (is_ec_data_shard_by_tgt_off(unpack_tgt_off, &arg->oc_attr) ||
	     (io->ui_oid.id_layout_ver > 0 && io->ui_oid.id_shard != parity_shard))) {
		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " ignore shard " DF_KEY "/%u/%d/%u/%d.\n",
			DP_RB_MPT(tls), DP_UOID(io->ui_oid), DP_KEY(&io->ui_dkey), shard,
			(int)obj_ec_shard_off(obj, io->ui_dkey_hash, 0), parity_shard, rc);
		D_GOTO(put, rc = 0);
	}
	rc = 0;

	/* Convert EC object offset to DAOS offset. */
	for (i = 0; i <= io->ui_iods_top && io->ui_dkey_punch_eph == 0 &&
	     io->ui_obj_punch_eph == 0; i++) {
		daos_iod_t	*iod = &io->ui_iods[i];
		daos_epoch_t	**ephs = &io->ui_recx_ephs[i];

		if (iod->iod_type == DAOS_IOD_SINGLE || io->ui_akey_punch_ephs[i] != 0) {
			create_migrate_one = true;
			continue;
		}

		D_DEBUG(DB_REBUILD,
			DF_RB ": " DF_UOID " unpack " DF_KEY " for shard "
			      "%u/%u/%u/" DF_X64 "/%u\n",
			DP_RB_MPT(tls), DP_UOID(io->ui_oid), DP_KEY(&io->ui_dkey), shard,
			unpack_tgt_off, migrate_tgt_off, io->ui_dkey_hash, parity_shard);

		/**
		 * Since we do not need split the rebuild into parity rebuild
		 * (by mo_iods_from_parity) and partial update(by mo_iods),
		 * so it does not need keep the PARITY BIT in recx, see rw_iod_pack().
		 */
		rc = obj_recx_ec2_daos(&arg->oc_attr, unpack_tgt_off,
				       &iod->iod_recxs, ephs, &iod->iod_nr, false);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": " DF_UOID " ec 2 daos %u failed", DP_RB_MPT(tls),
				 DP_UOID(io->ui_oid), shard);
			D_GOTO(put, rc);
		}

		/* Filter the DAOS recxs to the rebuild data shard */
		if (is_ec_data_shard_by_layout_ver(layout_ver, io->ui_dkey_hash,
						   &arg->oc_attr, shard)) {
			D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " convert shard %u tgt %d\n",
				DP_RB_MPT(tls), DP_UOID(io->ui_oid), shard,
				obj_ec_data_tgt_nr(&arg->oc_attr));

			rc = obj_recx_ec_daos2shard(&arg->oc_attr, migrate_tgt_off,
						    &iod->iod_recxs, ephs, &iod->iod_nr);
			if (rc) {
				DL_ERROR(rc, DF_RB ": " DF_UOID " daos to shard %u failed",
					 DP_RB_MPT(tls), DP_UOID(io->ui_oid), shard);
				D_GOTO(put, rc);
			}
		}

		if (iod->iod_nr > 0)
			create_migrate_one = true;
	}

	if (!create_migrate_one) {
		struct ds_cont_child *cont = NULL;

		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID "/" DF_KEY " does not need rebuild.\n",
			DP_RB_MPT(tls), DP_UOID(io->ui_oid), DP_KEY(&io->ui_dkey));

		/* Create the vos container when no record need to be rebuilt for this shard,
		 * for the case of reintegrate the container was discarded ahead.
		 */
		rc = migrate_get_cont_child(tls, arg->arg->cont_uuid, &cont, true);
		if (cont != NULL)
			ds_cont_child_put(cont);

		D_GOTO(put, rc = 0);
	}

	/* Check if some IODs from this unpack can be merged to the exist mrone, mostly for EC
	 * parity rebuilt, since it might enumerate from different data shards, whose recxs might
	 * be able to be merged here.
	 */
	d_list_for_each_entry(mo, &arg->merge_list, mo_list) {
		if (daos_oid_cmp(mo->mo_oid.id_pub,
				 io->ui_oid.id_pub) == 0 &&
		    mo->mo_version == io->ui_version &&
		    daos_key_match(&mo->mo_dkey, &io->ui_dkey)) {
			rc = migrate_try_merge_recx(mo, obj, io);
			if (rc < 0)
				D_GOTO(put, rc);

			if (rc == 0)
				merged = true; /* merged all recxs already */
			else
				rc = 0; /* Not merge all recxs */

			break;
		}
	}

	if (!merged)
		rc = migrate_one_create(arg, io);
put:
	if (obj)
		obj_decref(obj);
	if (tls != NULL)
		migrate_pool_tls_put(tls);
	return rc;
}

static int
migrate_obj_punch_one(void *data)
{
	struct migrate_pool_tls *tls;
	struct iter_obj_arg	*arg = data;
	struct ds_cont_child	*cont;
	int			rc;

	tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(arg->pool_uuid));
		D_GOTO(put, rc = 0);
	}

	D_DEBUG(DB_REBUILD, DF_RB ": tls %p version %d punch " DF_U64 " " DF_UOID "\n",
		DP_RB_MPT(tls), tls, arg->version, arg->punched_epoch, DP_UOID(arg->oid));

	rc = migrate_get_cont_child(tls, arg->cont_uuid, &cont, true);
	if (rc != 0 || cont == NULL)
		D_GOTO(put, rc);

	D_ASSERT(arg->punched_epoch != 0);
	rc = vos_obj_punch(cont->sc_hdl, arg->oid, arg->punched_epoch,
			   tls->mpt_version, VOS_OF_REPLAY_PC,
			   NULL, 0, NULL, NULL);
	ds_cont_child_put(cont);
put:
	if (rc)
		DL_ERROR(rc, DF_RB ": " DF_UOID " migrate punch failed", DP_RB_MPT(tls),
			 DP_UOID(arg->oid));
	if (tls) {
		if (tls->mpt_status == 0 && rc != 0)
			tls->mpt_status = rc;
		migrate_pool_tls_put(tls);
	}

	return rc;
}

static int
migrate_start_ult(struct enum_unpack_arg *unpack_arg)
{
	struct migrate_pool_tls *tls;
	struct iter_obj_arg	*arg = unpack_arg->arg;
	struct migrate_one	*mrone;
	struct migrate_one	*tmp;
	int			rc = 0;

	tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(arg->pool_uuid));
		D_GOTO(put, rc = 0);
	}
	d_list_for_each_entry_safe(mrone, tmp, &unpack_arg->merge_list,
				   mo_list) {
		D_DEBUG(DB_REBUILD,
			DF_RB ": " DF_UOID " %p dkey " DF_KEY " migrate on idx %d"
			      " iod_num %d\n",
			DP_RB_MPT(tls), DP_UOID(mrone->mo_oid), mrone, DP_KEY(&mrone->mo_dkey),
			arg->tgt_idx, mrone->mo_iod_num);

		rc = migrate_tgt_enter(tls);
		if (rc)
			break;
		d_list_del_init(&mrone->mo_list);
		rc = dss_ult_create(migrate_one_ult, mrone, DSS_XS_VOS,
				    arg->tgt_idx, MIGRATE_STACK_SIZE, NULL);
		if (rc) {
			migrate_tgt_exit(tls, DKEY_ULT);
			migrate_one_destroy(mrone);
			break;
		}
	}

put:
	if (tls)
		migrate_pool_tls_put(tls);
	return rc;
}

#define KDS_NUM		96
#define ITER_BUF_SIZE	2048

/**
 * Iterate akeys/dkeys of the object
 */
static int
migrate_one_epoch_object(daos_epoch_range_t *epr, struct migrate_pool_tls *tls,
			 struct iter_obj_arg *arg)
{
	daos_anchor_t		 anchor;
	daos_anchor_t		 dkey_anchor;
	daos_anchor_t		 akey_anchor;
	char			 stack_buf[ITER_BUF_SIZE] = {0};
	char			*buf = NULL;
	daos_size_t		 buf_len;
	daos_key_desc_t		 kds[KDS_NUM] = {0};
	d_iov_t			 csum = {0};
	d_iov_t			 *p_csum;
	uint8_t			 stack_csum_buf[CSUM_BUF_SIZE] = {0};
	struct cont_props	 props;
	struct enum_unpack_arg	 unpack_arg = { 0 };
	d_iov_t			 iov = { 0 };
	d_sg_list_t		 sgl = { 0 };
	daos_handle_t		 coh = DAOS_HDL_INVAL;
	daos_handle_t		 oh  = DAOS_HDL_INVAL;
	uint32_t		 minimum_nr;
	uint32_t		 enum_flags;
	uint32_t		 num;
	int			 rc = 0;

	D_DEBUG(DB_REBUILD, DF_RB ": migrate obj " DF_UOID " shard %u eph " DF_X64 "-" DF_X64 "\n",
		DP_RB_MPT(tls), DP_UOID(arg->oid), arg->shard, epr->epr_lo, epr->epr_hi);

	if (tls->mpt_fini) {
		D_DEBUG(DB_REBUILD, DF_RB ": migration is aborted.\n", DP_RB_MPT(tls));
		return 0;
	}

	D_ASSERT(dss_get_module_info()->dmi_xs_id != 0);

	rc = dsc_pool_open(tls->mpt_pool_uuid, tls->mpt_poh_uuid, 0,
			   NULL, tls->mpt_pool->spc_pool->sp_map,
			   &tls->mpt_svc_list, &tls->mpt_pool_hdl);
	if (rc) {
		DL_ERROR(rc, DF_RB ": dsc_pool_open failed", DP_RB_MPT(tls));
		D_GOTO(out, rc);
	}

	rc = migrate_cont_open(tls, arg->cont_uuid, 0, &coh);
	if (rc) {
		DL_ERROR(rc, DF_RB ": migrate_cont_open failed", DP_RB_MPT(tls));
		D_GOTO(out, rc);
	}

	/* Only open with RW flag, reintegrating flag will be set, which is needed
	 * during unpack_cb to check if parity shard alive.
	 */
	rc = dsc_obj_open(coh, arg->oid.id_pub, DAOS_OO_RO, &oh);
	if (rc) {
		DL_ERROR(rc, DF_RB ": dsc_obj_open failed", DP_RB_MPT(tls));
		D_GOTO(out, rc);
	}

	unpack_arg.arg = arg;
	unpack_arg.epr = *epr;
	unpack_arg.oh = oh;
	unpack_arg.version = tls->mpt_version;
	D_INIT_LIST_HEAD(&unpack_arg.merge_list);
	buf = stack_buf;
	buf_len = ITER_BUF_SIZE;

	dsc_cont_get_props(coh, &props);
	rc = dsc_obj_id2oc_attr(arg->oid.id_pub, &props, &unpack_arg.oc_attr);
	if (rc) {
		DL_ERROR(rc, DF_RB ": unknown object class: %u", DP_RB_MPT(tls),
			 daos_obj_id2class(arg->oid.id_pub));
		D_GOTO(out_obj, rc);
	}

	memset(&anchor, 0, sizeof(anchor));
	memset(&akey_anchor, 0, sizeof(akey_anchor));
	memset(&dkey_anchor, 0, sizeof(dkey_anchor));
	if (tls->mpt_opc == RB_OP_UPGRADE)
		unpack_arg.new_layout_ver = tls->mpt_new_layout_ver;

	enum_flags = DIOF_TO_LEADER | DIOF_WITH_SPEC_EPOCH |
		     DIOF_FOR_MIGRATION;

	/* Upgrade needs to iterate all of the group, since dkey might be
	 * in different group after upgrade, otherwise only iterate the
	 * specific group.
	 */
	if (tls->mpt_opc != RB_OP_UPGRADE) {
		dc_obj_shard2anchor(&dkey_anchor, arg->shard);
		enum_flags |= DIOF_TO_SPEC_GROUP;
	}

	if (daos_oclass_is_ec(&unpack_arg.oc_attr)) {
		p_csum = NULL;
		/* EC rotate needs to fetch from all shards */
		if (obj_ec_parity_rotate_enabled_by_version(arg->oid.id_layout_ver))
			minimum_nr = obj_ec_tgt_nr(&unpack_arg.oc_attr);
		else
			minimum_nr = 2;
		enum_flags |= DIOF_RECX_REVERSE;
	} else {
		minimum_nr = 2;
		p_csum = &csum;
		d_iov_set(&csum, stack_csum_buf, CSUM_BUF_SIZE);
	}

	while (!tls->mpt_fini) {
		memset(buf, 0, buf_len);
		memset(kds, 0, KDS_NUM * sizeof(*kds));
		iov.iov_len = 0;
		iov.iov_buf = buf;
		iov.iov_buf_len = buf_len;

		sgl.sg_nr = 1;
		sgl.sg_nr_out = 1;
		sgl.sg_iovs = &iov;

		if (p_csum != NULL)
			p_csum->iov_len = 0;

		daos_anchor_set_flags(&dkey_anchor, enum_flags);
		num = KDS_NUM;
		rc = dsc_obj_list_obj(oh, epr, NULL, NULL, NULL,
				     &num, kds, &sgl, &anchor,
				     &dkey_anchor, &akey_anchor, p_csum);

		if (rc == -DER_KEY2BIG) {
			D_DEBUG(DB_REBUILD,
				DF_RB ": migrate obj " DF_UOID " got -DER_KEY2BIG, "
				      "key_len " DF_U64 "\n",
				DP_RB_MPT(tls), DP_UOID(arg->oid), kds[0].kd_key_len);
			/* For EC parity migration, it will enumerate from all data
			 * shards, so buffer needs to time grp_size to make sure
			 * retry buffer will be large enough.
			 */
			if (daos_oclass_is_ec(&unpack_arg.oc_attr))
				buf_len = roundup(kds[0].kd_key_len * 2 *
						  daos_oclass_grp_size(&unpack_arg.oc_attr), 8);
			else
				buf_len = roundup(kds[0].kd_key_len * 2, 8);

			if (buf != stack_buf)
				D_FREE(buf);
			D_ALLOC(buf, buf_len);
			if (buf == NULL) {
				rc = -DER_NOMEM;
				break;
			}
			continue;
		} else if (rc == -DER_TRUNC && p_csum != NULL &&
			   p_csum->iov_len > p_csum->iov_buf_len) {
			D_DEBUG(DB_REBUILD,
				DF_RB ": migrate obj csum buf not large enough. "
				      "Increase and try again\n",
				DP_RB_MPT(tls));
			if (p_csum->iov_buf != stack_csum_buf)
				D_FREE(p_csum->iov_buf);

			p_csum->iov_buf_len = p_csum->iov_len;
			p_csum->iov_len = 0;
			D_ALLOC(p_csum->iov_buf, p_csum->iov_buf_len);
			if (p_csum->iov_buf == NULL) {
				rc = -DER_NOMEM;
				break;
			}
			continue;
		} else if (rc && rc != -DER_SHUTDOWN &&
			   daos_anchor_get_flags(&dkey_anchor) & DIOF_TO_LEADER) {
			if (rc != -DER_INPROGRESS) {
				enum_flags &= ~DIOF_TO_LEADER;
				D_DEBUG(DB_REBUILD,
					DF_RB ": retry to non leader " DF_UOID ": " DF_RC "\n",
					DP_RB_MPT(tls), DP_UOID(arg->oid), DP_RC(rc));
			} else {
				/* Keep retry on leader if it is inprogress or shutdown,
				 * since the new dtx leader might still resync the
				 * uncommitted records, or it will choose a new leader
				 * once the pool map is updated.
				 */
				D_DEBUG(DB_REBUILD, DF_RB ": retry leader " DF_UOID "\n",
					DP_RB_MPT(tls), DP_UOID(arg->oid));
			}
			continue;
		} else if (rc == -DER_UPDATE_AGAIN) {
			/* -DER_UPDATE_AGAIN means the remote target does not parse EC
			 * aggregation yet, so let's retry.
			 */
			D_DEBUG(DB_REBUILD, DF_RB ": " DF_UOID " retry with %d \n", DP_RB_MPT(tls),
				DP_UOID(arg->oid), rc);
			rc = 0;
			continue;
		} else if (rc) {
			/* To avoid reclaim and retry rebuild, let's retry until the pool map
			 * being changed due to further failure.
			 */
			if (rc == -DER_TIMEDOUT &&
			    tls->mpt_version + 1 >= tls->mpt_pool->spc_map_version) {
				D_WARN(DF_RB ": retry " DF_UOID " " DF_RC "\n", DP_RB_MPT(tls),
				       DP_UOID(arg->oid), DP_RC(rc));
				rc = 0;
				continue;
			}

			/* container might have been destroyed. Or there is
			 * no spare target left for this object see
			 * obj_grp_valid_shard_get()
			 */
			/* DER_DATA_LOSS means it can not find any replicas
			 * to rebuild the data, see obj_list_common.
			 */
			/* If the container is being destroyed, it may return
			 * -DER_NONEXIST, see obj_ioc_init().
			 */
			if (rc == -DER_DATA_LOSS || rc == -DER_NONEXIST) {
				D_WARN(DF_RB ": mo replicas for " DF_UOID " %d\n", DP_RB_MPT(tls),
				       DP_UOID(arg->oid), rc);
				num = 0;
				rc = 0;
			}
			D_DEBUG(DB_REBUILD, DF_RB ": cannot rebuild " DF_UOID " " DF_RC " spc %u\n",
				DP_RB_MPT(tls), DP_UOID(arg->oid), DP_RC(rc),
				tls->mpt_pool->spc_map_version);
			break;
		}

		/* Each object enumeration RPC will at least one OID */
		if (num <= minimum_nr && (enum_flags & DIOF_TO_SPEC_GROUP)) {
			D_DEBUG(DB_REBUILD, DF_RB ": enumeration buffer %u empty" DF_UOID "\n",
				DP_RB_MPT(tls), num, DP_UOID(arg->oid));
			break;
		}

		D_ASSERTF(sgl.sg_iovs[0].iov_len <= buf_len, DF_U64"/"DF_U64" > "DF_U64"\n",
			  sgl.sg_iovs[0].iov_buf_len, sgl.sg_iovs[0].iov_len, buf_len);
		rc = dc_obj_enum_unpack(arg->oid, kds, num, &sgl, p_csum,
					migrate_enum_unpack_cb, &unpack_arg);
		if (rc) {
			DL_ERROR(rc, DF_RB ": migrate " DF_UOID " failed", DP_RB_MPT(tls),
				 DP_UOID(arg->oid));
			break;
		}

		rc = migrate_start_ult(&unpack_arg);
		if (rc) {
			DL_ERROR(rc, DF_RB ": start migrate " DF_UOID " failed", DP_RB_MPT(tls),
				 DP_UOID(arg->oid));
			break;
		}

		if (daos_anchor_is_eof(&dkey_anchor))
			break;

		/* Restore leader flag to always try the leader first */
		enum_flags |= DIOF_TO_LEADER;
	}

	if (buf != NULL && buf != stack_buf)
		D_FREE(buf);

	if (csum.iov_buf != NULL && csum.iov_buf != stack_csum_buf)
		D_FREE(csum.iov_buf);
out_obj:
	dsc_obj_close(oh);
out:
	D_DEBUG(DB_REBUILD,
		DF_RB ": obj " DF_UOID " shard %u eph " DF_U64 "-" DF_U64 ": " DF_RC "\n",
		DP_RB_MPT(tls), DP_UOID(arg->oid), arg->shard, epr->epr_lo, epr->epr_hi, DP_RC(rc));

	return rc;
}

struct migrate_stop_arg {
	uuid_t	pool_uuid;
	unsigned int version;
	unsigned int generation;
};

static int
migrate_fini_one_ult(void *data)
{
	struct migrate_stop_arg *arg = data;
	struct migrate_pool_tls *tls;
	int			 rc;

	tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (tls == NULL)
		return 0;

	tls->mpt_fini = 1;

	ABT_mutex_lock(tls->mpt_inflight_mutex);
	ABT_cond_broadcast(tls->mpt_inflight_cond);
	ABT_mutex_unlock(tls->mpt_inflight_mutex);

	migrate_pool_tls_put(tls); /* lookup */
	rc = ABT_eventual_wait(tls->mpt_done_eventual, NULL);
	if (rc != ABT_SUCCESS) {
		rc = dss_abterr2der(rc);
		D_WARN("failed to migrate fini one ult "DF_UUID": "DF_RC"\n",
		       DP_UUID(arg->pool_uuid), DP_RC(rc));
	} else {
		rc = 0;
	}

	migrate_pool_tls_put(tls); /* destroy */

	D_INFO("migrate fini one ult "DF_UUID"\n", DP_UUID(arg->pool_uuid));
	return rc;
}

/* stop the migration */
void
ds_migrate_stop(struct ds_pool *pool, unsigned int version, unsigned int generation)
{
	struct migrate_pool_tls *tls;
	struct migrate_stop_arg arg;
	int			 rc;

	tls = migrate_pool_tls_lookup(pool->sp_uuid, version, generation);
	if (tls == NULL || tls->mpt_fini) {
		if (tls != NULL)
		       migrate_pool_tls_put(tls);
		D_INFO(DF_UUID" migrate stopped\n", DP_UUID(pool->sp_uuid));
		return;
	}

	tls->mpt_fini = 1;
	uuid_copy(arg.pool_uuid, pool->sp_uuid);
	arg.version = version;
	arg.generation = generation;

	rc = ds_pool_thread_collective(pool->sp_uuid, 0, migrate_fini_one_ult, &arg, 0);
	if (rc)
		D_ERROR(DF_UUID" migrate stop: %d\n", DP_UUID(pool->sp_uuid), rc);

	migrate_pool_tls_put(tls);
	/* Wait for xstream 0 migrate ULT(migrate_ult) stop */
	if (tls->mpt_ult_running) {
		ABT_mutex_lock(tls->mpt_inflight_mutex);
		ABT_cond_broadcast(tls->mpt_inflight_cond);
		ABT_mutex_unlock(tls->mpt_inflight_mutex);
		rc = ABT_eventual_wait(tls->mpt_done_eventual, NULL);
		if (rc != ABT_SUCCESS) {
			rc = dss_abterr2der(rc);
			D_WARN("failed to migrate wait "DF_UUID": "DF_RC"\n",
			       DP_UUID(pool->sp_uuid), DP_RC(rc));
		}
	}

	migrate_pool_tls_put(tls);
	pool->sp_rebuilding--;
	D_INFO(DF_UUID" migrate stopped\n", DP_UUID(pool->sp_uuid));
}

static int
migrate_obj_punch(struct iter_obj_arg *arg)
{
	return dss_ult_execute(migrate_obj_punch_one, arg, NULL, NULL, DSS_XS_VOS,
			       arg->tgt_idx, MIGRATE_STACK_SIZE);
}

/**
 * This ULT manages migration one object ID for one container. It does not do
 * the data migration itself - instead it iterates akeys/dkeys as a client and
 * schedules the actual data migration on their own ULTs
 *
 * If this is reintegration, this ULT will be launched on the target where data
 * is stored so that it can be safely deleted prior to migration. If this is not
 * reintegration, this ULT will be launched on a pseudorandom ULT to increase
 * parallelism by spreading the work among many xstreams.
 *
 * Note that this ULT is guaranteed to only be spawned once per object per
 * container per migration session (using mpt_migrated_root)
 */
static void
migrate_obj_ult(void *data)
{
	struct iter_obj_arg	*arg = data;
	struct migrate_pool_tls	*tls = NULL;
	daos_epoch_range_t	epr;
	daos_epoch_t		stable_epoch = 0;
	int			i;
	int			rc = 0;

	tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (tls == NULL || tls->mpt_fini) {
		D_WARN("someone aborted the rebuild " DF_UUID "\n", DP_UUID(arg->pool_uuid));
		D_GOTO(free_notls, rc);
	}

	/* Only reintegrating targets/pool needs to discard the object,
	 * if sp_need_discard is 0, either the target does not need to
	 * discard, or discard has been done. spc_discard_done means
	 * discarding has been done in the current VOS target.
	 */
	if (tls->mpt_pool->spc_pool->sp_need_discard) {
		while(!tls->mpt_pool->spc_discard_done) {
			D_DEBUG(DB_REBUILD, DF_RB ": wait for discard to finish.\n",
				DP_RB_MPT(tls));
			dss_sleep(2 * 1000);
			if (tls->mpt_fini)
				D_GOTO(free_notls, rc);
		}
		if (tls->mpt_pool->spc_pool->sp_discard_status) {
			rc = tls->mpt_pool->spc_pool->sp_discard_status;
			D_DEBUG(DB_REBUILD, DF_RB ": discard failure: " DF_RC "\n", DP_RB_MPT(tls),
				DP_RC(rc));
			D_GOTO(out, rc);
		}
	}

	if (tls->mpt_reintegrating) {
		struct ds_cont_child *cont_child = NULL;

		/* check again to see if the container is being destroyed. */
		migrate_get_cont_child(tls, arg->cont_uuid, &cont_child, false);
		if (cont_child != NULL && !cont_child->sc_stopping) {
			if (vos_oi_exist(cont_child->sc_hdl, arg->oid)) {
				stable_epoch = vos_cont_get_global_stable_epoch(cont_child->sc_hdl);
			} else {
				/* If the object does not exist on the local target at all,
				 * it is either created after stable epoch or migrated to this
				 * target due to co-location. let's set stable epoch to 0,
				 * migrate the whole object.
				 */
				stable_epoch = 0;
			}
			D_DEBUG(DB_REBUILD, DF_CONT " reint from stable epoch "DF_X64"\n",
				DP_CONT(arg->pool_uuid, arg->cont_uuid), stable_epoch);
		}

		if (cont_child)
			ds_cont_child_put(cont_child);
	}

	for (i = 0; i < arg->snap_cnt; i++) {
		daos_epoch_t lower_epoch = 0;

		if (arg->snaps[i] < stable_epoch) {
			D_DEBUG(DB_REBUILD, DF_CONT " obj " DF_UOID " skip snap "DF_X64
				" < stable " DF_X64 "\n", DP_CONT(arg->pool_uuid, arg->cont_uuid),
				DP_UOID(arg->oid), arg->snaps[i], stable_epoch);
			continue;
		}

		if (i == 0)
			lower_epoch = stable_epoch;
		else
			lower_epoch = max(stable_epoch, arg->snaps[i - 1]);

		epr.epr_lo = lower_epoch;
		epr.epr_hi = arg->snaps[i];
		D_DEBUG(DB_REBUILD, DF_RB ": rebuild_snap %d " DF_X64 "-" DF_X64 "\n",
			DP_RB_MPT(tls), i, epr.epr_lo, epr.epr_hi);
		rc = migrate_one_epoch_object(&epr, tls, arg);
		if (rc)
			D_GOTO(free, rc);
	}

	if (arg->snap_cnt > 0 && arg->punched_epoch != 0 && arg->punched_epoch > stable_epoch) {
		rc = migrate_obj_punch(arg);
		if (rc)
			D_GOTO(free, rc);
	}

	epr.epr_lo = arg->snaps ? arg->snaps[arg->snap_cnt - 1] + 1 : 0;
	epr.epr_lo = max(epr.epr_lo, stable_epoch);
	D_ASSERT(tls->mpt_max_eph != 0);
	epr.epr_hi = tls->mpt_max_eph;
	if (arg->epoch > 0) {
		rc = migrate_one_epoch_object(&epr, tls, arg);
	} else {
		/* The obj has been punched for this range */
		D_DEBUG(DB_REBUILD,
			DF_RB ": punched obj " DF_UOID " epoch " DF_U64 "/" DF_U64 "/" DF_U64 "\n",
			DP_RB_MPT(tls), DP_UOID(arg->oid), arg->epoch, arg->punched_epoch,
			epr.epr_hi);
		arg->epoch = DAOS_EPOCH_MAX;
	}
free:
	if (arg->epoch == DAOS_EPOCH_MAX)
		tls->mpt_obj_count++;

	if (rc == -DER_NONEXIST) {
		struct ds_cont_child *cont_child = NULL;

		/* check again to see if the container is being destroyed. */
		migrate_get_cont_child(tls, arg->cont_uuid, &cont_child, false);
		if (cont_child == NULL || cont_child->sc_stopping)
			rc = 0;

		if (cont_child)
			ds_cont_child_put(cont_child);
	}

	if (DAOS_FAIL_CHECK(DAOS_REBUILD_OBJ_FAIL) &&
	    tls->mpt_obj_count >= daos_fail_value_get())
		rc = -DER_IO;

out:
	if (tls->mpt_status == 0 && rc < 0)
		tls->mpt_status = rc;

	D_DEBUG(
	    DB_REBUILD,
	    DF_RB ":  stop migrate obj " DF_UOID "for shard %u ult %u/%u " DF_U64 " : " DF_RC "\n",
	    DP_RB_MPT(tls), DP_UOID(arg->oid), arg->shard, atomic_load(tls->mpt_tgt_obj_ult_cnt),
	    atomic_load(tls->mpt_tgt_dkey_ult_cnt), tls->mpt_obj_count, DP_RC(rc));
free_notls:
	if (tls != NULL)
		migrate_tgt_exit(tls, OBJ_ULT);

	D_FREE(arg->snaps);
	D_FREE(arg);
	migrate_pool_tls_put(tls);
}

struct migrate_obj_val {
	daos_epoch_t	epoch;
	daos_epoch_t	punched_epoch;
	uint32_t	shard;
	uint32_t	tgt_idx;
};

/* This is still running on the main migration ULT */
static int
migrate_one_object(daos_unit_oid_t oid, daos_epoch_t eph, daos_epoch_t punched_eph,
		   unsigned int shard, unsigned int tgt_idx, void *data)
{
	struct iter_cont_arg	*cont_arg = data;
	struct iter_obj_arg	*obj_arg;
	struct migrate_pool_tls *tls = cont_arg->pool_tls;
	daos_handle_t		 toh = tls->mpt_migrated_root_hdl;
	struct migrate_obj_val	 val;
	d_iov_t			 val_iov;
	int			 rc;

	D_ASSERT(daos_handle_is_valid(toh));

	D_ALLOC_PTR(obj_arg);
	if (obj_arg == NULL)
		return -DER_NOMEM;

	obj_arg->oid = oid;
	obj_arg->epoch = eph;
	obj_arg->shard = shard;
	obj_arg->punched_epoch = punched_eph;
	obj_arg->tgt_idx = tgt_idx;
	uuid_copy(obj_arg->pool_uuid, cont_arg->pool_tls->mpt_pool_uuid);
	uuid_copy(obj_arg->cont_uuid, cont_arg->cont_uuid);
	obj_arg->version = cont_arg->pool_tls->mpt_version;
	obj_arg->generation = cont_arg->pool_tls->mpt_generation;
	if (cont_arg->snaps) {
		D_ALLOC(obj_arg->snaps,
			sizeof(*cont_arg->snaps) * cont_arg->snap_cnt);
		if (obj_arg->snaps == NULL)
			D_GOTO(free, rc = -DER_NOMEM);

		obj_arg->snap_cnt = cont_arg->snap_cnt;
		memcpy(obj_arg->snaps, cont_arg->snaps,
		       sizeof(*obj_arg->snaps) * cont_arg->snap_cnt);
	}

	/* Let's iterate the object on different xstream */
	rc = dss_ult_create(migrate_obj_ult, obj_arg, DSS_XS_VOS,
			    tgt_idx, MIGRATE_STACK_SIZE, NULL);
	if (rc)
		goto free;

	val.epoch = eph;
	val.shard = shard;
	val.tgt_idx = tgt_idx;

	d_iov_set(&val_iov, &val, sizeof(struct migrate_obj_val));
	rc = obj_tree_insert(toh, cont_arg->cont_uuid, -1, oid, &val_iov);
	D_DEBUG(DB_REBUILD, DF_RB ": insert " DF_UUID "/" DF_UOID ": ult %u/%u " DF_RC "\n",
		DP_RB_MPT(tls), DP_UUID(cont_arg->cont_uuid), DP_UOID(oid),
		atomic_load(&tls->mpt_obj_ult_cnts[tgt_idx]),
		atomic_load(&tls->mpt_dkey_ult_cnts[tgt_idx]), DP_RC(rc));

	return 0;
free:
	D_FREE(obj_arg->snaps);
	D_FREE(obj_arg);
	return rc;
}

#define DEFAULT_YIELD_FREQ	16

static int
migrate_obj_iter_cb(daos_handle_t ih, d_iov_t *key_iov, d_iov_t *val_iov, void *data)
{
	struct iter_cont_arg		*arg = data;
	daos_unit_oid_t			*oid = key_iov->iov_buf;
	struct migrate_obj_val		*obj_val = val_iov->iov_buf;
	daos_epoch_t			epoch = obj_val->epoch;
	daos_epoch_t			punched_epoch = obj_val->punched_epoch;
	unsigned int			tgt_idx = obj_val->tgt_idx;
	unsigned int			shard = obj_val->shard;
	d_iov_t				tmp_iov;
	bool				yielded = false;
	int				rc;

	if (arg->pool_tls->mpt_fini)
		return 1;

	D_DEBUG(DB_REBUILD,
		DF_RB ": obj migrate " DF_UUID "/" DF_UOID " %" PRIx64 " eph " DF_U64 " start\n",
		DP_RB_MPT(arg->pool_tls), DP_UUID(arg->cont_uuid), DP_UOID(*oid), ih.cookie, epoch);

	rc = migrate_system_enter(arg->pool_tls, tgt_idx, &yielded);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": " DF_UUID " enter migrate failed.", DP_RB_MPT(arg->pool_tls),
			 DP_UUID(arg->cont_uuid));
		return rc;
	}

	rc = migrate_one_object(*oid, epoch, punched_epoch, shard, tgt_idx, arg);
	if (rc != 0) {
		DL_ERROR(rc, DF_RB ": obj " DF_UOID " migration failed", DP_RB_MPT(arg->pool_tls),
			 DP_UOID(*oid));
		migrate_system_exit(arg->pool_tls, tgt_idx);
		return rc;
	}

	/* migrate_system_enter possibly yielded the ULT, let's re-probe before delete  */
	if (yielded) {
		d_iov_set(&tmp_iov, oid, sizeof(*oid));
		rc = dbtree_iter_probe(ih, BTR_PROBE_EQ, DAOS_INTENT_MIGRATION, &tmp_iov, NULL);
		if (rc) {
			D_ASSERT(rc != -DER_NONEXIST);
			DL_ERROR(rc, DF_RB ": obj " DF_UOID " probe failed",
				 DP_RB_MPT(arg->pool_tls), DP_UOID(*oid));
			return rc;
		}
	}

	rc = dbtree_iter_delete(ih, NULL);
	if (rc) {
		DL_ERROR(rc, DF_RB ": dbtree_iter_delete failed", DP_RB_MPT(arg->pool_tls));
		return rc;
	}

	if (--arg->yield_freq == 0) {
		arg->yield_freq = DEFAULT_YIELD_FREQ;
		dss_sleep(0);
	}

	/* re-probe the dbtree after deletion */
	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, DAOS_INTENT_MIGRATION,
			       NULL, NULL);
	if (rc == -DER_NONEXIST)
		return 1;
	else if (rc != 0)
		DL_ERROR(rc, DF_RB ": dbtree_iter_probe failed", DP_RB_MPT(arg->pool_tls));

	return rc;
}

/* This iterates the migration database "container", which is different than the
 * similarly identified by container UUID as the actual container in VOS.
 * However, this container only contains object IDs that were specified to be
 * migrated
 */
static int
migrate_cont_iter_cb(daos_handle_t ih, d_iov_t *key_iov,
		     d_iov_t *val_iov, void *data)
{
	struct ds_pool		*dp;
	struct iter_cont_arg	 arg = { 0 };
	struct tree_cache_root	*root = val_iov->iov_buf;
	struct migrate_pool_tls	*tls = data;
	uint64_t		*snapshots = NULL;
	uuid_t			 cont_uuid;
	int			 snap_cnt;
	d_iov_t			 tmp_iov;
	int			 rc;

	uuid_copy(cont_uuid, *(uuid_t *)key_iov->iov_buf);
	D_DEBUG(DB_REBUILD, DF_RB ": iter cont " DF_UUID "/%" PRIx64 " %" PRIx64 " start\n",
		DP_RB_MPT(tls), DP_UUID(cont_uuid), ih.cookie, root->tcr_root_hdl.cookie);

	rc = ds_pool_lookup(tls->mpt_pool_uuid, &dp);
	if (rc) {
		DL_ERROR(rc, DF_RB ": ds_pool_lookup failed", DP_RB_MPT(tls));
		rc = 0;
		D_GOTO(out_put, rc);
	}

	rc = ds_cont_fetch_snaps(dp->sp_iv_ns, cont_uuid, &snapshots,
				 &snap_cnt);
	if (rc) {
		DL_ERROR(rc, DF_RB ": ds_cont_fetch_snaps failed", DP_RB_MPT(tls));
		D_GOTO(out_put, rc);
	}

	rc = ds_cont_fetch_ec_agg_boundary(dp->sp_iv_ns, cont_uuid);
	if (rc) {
		/* Sometime it may too early to fetch the EC boundary,
		 * since EC boundary does not start yet, which is forbidden
		 * during rebuild anyway, so let's continue.
		 */
		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UUID " fetch agg_boundary failed: " DF_RC "\n",
			DP_RB_MPT(tls), DP_UUID(cont_uuid), DP_RC(rc));
	}

	arg.yield_freq	= DEFAULT_YIELD_FREQ;
	arg.cont_root	= root;
	arg.snaps	= snapshots;
	arg.snap_cnt	= snap_cnt;
	arg.pool_tls	= tls;
	uuid_copy(arg.cont_uuid, cont_uuid);
	while (!dbtree_is_empty(root->tcr_root_hdl)) {
		if (tls->mpt_fini)
			break;

		rc = dbtree_iterate(root->tcr_root_hdl, DAOS_INTENT_MIGRATION,
				    false, migrate_obj_iter_cb, &arg);
		if (rc || tls->mpt_fini)
			break;
	}

	D_DEBUG(DB_REBUILD, DF_RB ": iter cont " DF_UUID "/%" PRIx64 " finish.\n", DP_RB_MPT(tls),
		DP_UUID(cont_uuid), ih.cookie);

	rc = dbtree_destroy(root->tcr_root_hdl, NULL);
	if (rc) {
		/* Ignore the DRAM migrate object tree for the moment, since
		 * it does not impact the migration on the storage anyway
		 */
		DL_ERROR(rc, DF_RB ": dbtree_destroy failed", DP_RB_MPT(tls));
	}

	/* Snapshot fetch will yield the ULT, let's reprobe before delete  */
	d_iov_set(&tmp_iov, cont_uuid, sizeof(uuid_t));
	rc = dbtree_iter_probe(ih, BTR_PROBE_EQ, DAOS_INTENT_MIGRATION,
			       &tmp_iov, NULL);
	if (rc) {
		D_ASSERT(rc != -DER_NONEXIST);
		D_GOTO(free, rc);
	}

	rc = dbtree_iter_delete(ih, NULL);
	if (rc) {
		DL_ERROR(rc, DF_RB ": dbtree_iter_delete failed", DP_RB_MPT(tls));
		D_GOTO(free, rc);
	}

	/* re-probe the dbtree after delete */
	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, DAOS_INTENT_MIGRATION,
			       NULL, NULL);

	if (rc == -DER_NONEXIST) {
		rc = 1; /* empty after delete */
		D_GOTO(free, rc);
	}
free:
	if (snapshots)
		D_FREE(snapshots);

out_put:
	if (tls->mpt_status == 0 && rc < 0)
		tls->mpt_status = rc;
	if (dp != NULL)
		ds_pool_put(dp);
	return rc;
}

struct migrate_ult_arg {
	uuid_t		pool_uuid;
	uint32_t	version;
};

static void
migrate_ult(void *arg)
{
	struct migrate_pool_tls	*pool_tls = arg;
	int			rc;

	D_ASSERT(pool_tls != NULL);
	while (!dbtree_is_empty(pool_tls->mpt_root_hdl) && !pool_tls->mpt_fini) {
		rc = dbtree_iterate(pool_tls->mpt_root_hdl,
				    DAOS_INTENT_PURGE, false,
				    migrate_cont_iter_cb, pool_tls);
		if (rc < 0) {
			DL_ERROR(rc, DF_RB ": dbtree iterate failed", DP_RB_MPT(pool_tls));
			if (pool_tls->mpt_status == 0)
				pool_tls->mpt_status = rc;
			break;
		}
	}

	pool_tls->mpt_ult_running = 0;
	migrate_pool_tls_put(pool_tls);
}

/**
 * Init migrate objects tree, so the migrating objects are added to
 * to-be-migrated tree(mpt_root/mpt_root_hdl) first, then moved to
 * migrated tree once it is being migrated. (mpt_migrated_root/
 * mpt_migrated_root_hdl). The incoming objects will check both
 * tree to see if the objects being migrated already.
 */
static int
migrate_try_create_object_tree(struct migrate_pool_tls *tls)
{
	struct umem_attr uma;
	int rc;

	if (daos_handle_is_inval(tls->mpt_root_hdl)) {
		/* migrate tree root init */
		memset(&uma, 0, sizeof(uma));
		uma.uma_id = UMEM_CLASS_VMEM;
		rc = dbtree_create_inplace(DBTREE_CLASS_UV, 0, 4, &uma,
					   &tls->mpt_root,
					   &tls->mpt_root_hdl);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": failed to create tree", DP_RB_MPT(tls));
			return rc;
		}
	}

	if (daos_handle_is_inval(tls->mpt_migrated_root_hdl)) {
		/* migrate tree root init */
		memset(&uma, 0, sizeof(uma));
		uma.uma_id = UMEM_CLASS_VMEM;
		rc = dbtree_create_inplace(DBTREE_CLASS_UV, 0, 4, &uma,
					   &tls->mpt_migrated_root,
					   &tls->mpt_migrated_root_hdl);
		if (rc != 0) {
			DL_ERROR(rc, DF_RB ": failed to create migrated tree", DP_RB_MPT(tls));
			return rc;
		}
	}

	return 0;
}

/**
 * Only insert objects if the objects does not exist in both
 * tobe-migrated tree and migrated tree.
 */
static int
migrate_try_obj_insert(struct migrate_pool_tls *tls, uuid_t co_uuid,
		       daos_unit_oid_t oid, daos_epoch_t epoch,
		       daos_epoch_t punched_epoch, unsigned int shard,
		       unsigned int tgt_idx)
{
	struct migrate_obj_val	val;
	daos_handle_t		toh = tls->mpt_root_hdl;
	daos_handle_t		migrated_toh = tls->mpt_migrated_root_hdl;
	d_iov_t			val_iov;
	int			rc;

	D_ASSERT(daos_handle_is_valid(toh));
	D_ASSERT(daos_handle_is_valid(migrated_toh));

	val.epoch = epoch;
	val.punched_epoch = punched_epoch;
	val.shard = shard;
	val.tgt_idx = tgt_idx;
	D_DEBUG(DB_REBUILD,
		DF_RB ": insert migrate " DF_UUID "/" DF_UOID " " DF_U64 "/" DF_U64 "/%d/%d\n",
		DP_RB_MPT(tls), DP_UUID(co_uuid), DP_UOID(oid), epoch, punched_epoch, shard,
		tgt_idx);

	d_iov_set(&val_iov, &val, sizeof(struct migrate_obj_val));
	rc = obj_tree_lookup(toh, co_uuid, oid, &val_iov);
	if (rc != -DER_NONEXIST) {
		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UUID "/" DF_UOID " no insert needed: " DF_RC "\n",
			DP_RB_MPT(tls), DP_UUID(co_uuid), DP_UOID(oid), DP_RC(rc));
		return rc;
	}

	rc = obj_tree_lookup(migrated_toh, co_uuid, oid, &val_iov);
	if (rc != -DER_NONEXIST) {
		D_DEBUG(DB_REBUILD, DF_RB ": " DF_UUID "/" DF_UOID " no insert needed: " DF_RC "\n",
			DP_RB_MPT(tls), DP_UUID(co_uuid), DP_UOID(oid), DP_RC(rc));
		return rc;
	}

	rc = obj_tree_insert(toh, co_uuid, -1, oid, &val_iov);

	return rc;
}

int
ds_migrate_object(struct ds_pool *pool, uuid_t po_hdl, uuid_t co_hdl, uuid_t co_uuid,
		  uint32_t version, unsigned int generation, uint64_t max_eph, uint32_t opc,
		  daos_unit_oid_t *oids, daos_epoch_t *epochs, daos_epoch_t *punched_epochs,
		  unsigned int *shards, uint32_t count, unsigned int tgt_idx,
		  uint32_t new_layout_ver)
{
	struct migrate_pool_tls	*tls = NULL;
	int			i;
	int			rc;

	/* Check if the pool tls exists */
	rc = migrate_pool_tls_lookup_create(pool, version, generation, po_hdl, co_hdl, max_eph,
					    new_layout_ver, opc, &tls);
	if (rc != 0)
		D_GOTO(out, rc);
	if (tls->mpt_fini)
		D_GOTO(out, rc = -DER_SHUTDOWN);

	/* NB: only create this tree on xstream 0 */
	rc = migrate_try_create_object_tree(tls);
	if (rc)
		D_GOTO(out, rc);

	/* Insert these oids/conts into the local tree */
	for (i = 0; i < count; i++) {
		/* firstly insert/check rebuilt tree */
		rc = migrate_try_obj_insert(tls, co_uuid, oids[i], epochs[i], punched_epochs[i],
					    shards[i], tgt_idx);
		if (rc == -DER_EXIST) {
			D_DEBUG(DB_TRACE, DF_RB ": " DF_UOID "/" DF_UUID "exists.\n",
				DP_RB_MPT(tls), DP_UOID(oids[i]), DP_UUID(co_uuid));
			rc = 0;
			continue;
		} else if (rc < 0) {
			DL_ERROR(rc, DF_RB ": " DF_UOID "/" DF_U64 "/" DF_UUID "/%u insert failed",
				 DP_RB_MPT(tls), DP_UOID(oids[i]), epochs[i], DP_UUID(co_uuid),
				 shards[i]);
			break;
		}
	}
	if (rc < 0)
		D_GOTO(out, rc);

	if (tls->mpt_ult_running)
		D_GOTO(out, rc);

	/* Check and create task to iterate the to-be-rebuilt tree */
	tls->mpt_ult_running = 1;
	migrate_pool_tls_get(tls);
	rc = dss_ult_create(migrate_ult, tls, DSS_XS_SELF, 0, MIGRATE_STACK_SIZE, NULL);
	if (rc) {
		DL_ERROR(rc, DF_RB ": create migrate ULT failed", DP_RB_MPT(tls));
		tls->mpt_ult_running = 0;
		migrate_pool_tls_put(tls);
	}

out:
	if (tls)
		migrate_pool_tls_put(tls);
	return rc;
}

/* Got the object list to migrate objects from remote target to
 * this target.
 */
void
ds_obj_migrate_handler(crt_rpc_t *rpc)
{
	struct obj_migrate_in	*migrate_in;
	struct obj_migrate_out	*migrate_out;
	daos_unit_oid_t		*oids;
	unsigned int		oids_count;
	daos_epoch_t		*ephs;
	daos_epoch_t		*punched_ephs;
	unsigned int		ephs_count;
	uint32_t		*shards;
	unsigned int		shards_count;
	uuid_t			po_uuid;
	uuid_t			po_hdl_uuid;
	uuid_t			co_uuid;
	uuid_t			co_hdl_uuid;
	struct ds_pool		*pool = NULL;
	uint32_t		rebuild_ver;
	int			rc;

	migrate_in = crt_req_get(rpc);
	oids = migrate_in->om_oids.ca_arrays;
	oids_count = migrate_in->om_oids.ca_count;
	ephs = migrate_in->om_ephs.ca_arrays;
	punched_ephs = migrate_in->om_punched_ephs.ca_arrays;
	ephs_count = migrate_in->om_ephs.ca_count;
	shards = migrate_in->om_shards.ca_arrays;
	shards_count = migrate_in->om_shards.ca_count;

	if (oids_count == 0 || shards_count == 0 || ephs_count == 0 ||
	    oids_count != shards_count || oids_count != ephs_count) {
		D_ERROR(DF_RB ": oids %u shards %u ephs %d\n", DP_RB_OMI(migrate_in), oids_count,
			shards_count, ephs_count);
		D_GOTO(out, rc = -DER_INVAL);
	}

	if (migrate_in->om_tgt_idx >= dss_tgt_nr) {
		D_ERROR(DF_RB " wrong tgt idx %d\n", DP_RB_OMI(migrate_in), migrate_in->om_tgt_idx);
		D_GOTO(out, rc = -DER_INVAL);
	}

	uuid_copy(co_uuid, migrate_in->om_cont_uuid);
	uuid_copy(co_hdl_uuid, migrate_in->om_coh_uuid);
	uuid_copy(po_uuid, migrate_in->om_pool_uuid);
	uuid_copy(po_hdl_uuid, migrate_in->om_poh_uuid);

	rc = ds_pool_lookup(po_uuid, &pool);
	if (rc != 0) {
		if (rc == -DER_SHUTDOWN) {
			D_DEBUG(DB_REBUILD, DF_RB " pool service is stopping.\n",
				DP_RB_OMI(migrate_in));
			rc = 0;
		} else {
			D_DEBUG(DB_REBUILD, DF_RB " pool service is not started yet. " DF_RC "\n",
				DP_RB_OMI(migrate_in), DP_RC(rc));
			rc = -DER_AGAIN;
		}
		D_GOTO(out, rc);
	}

	ds_rebuild_running_query(migrate_in->om_pool_uuid, -1, &rebuild_ver, NULL, NULL);
	if (rebuild_ver == 0 || rebuild_ver != migrate_in->om_version) {
		rc = -DER_SHUTDOWN;
		DL_ERROR(rc, DF_RB " rebuild ver %u", DP_RB_OMI(migrate_in), rebuild_ver);
		D_GOTO(out, rc);
	}

	rc = ds_migrate_object(pool, po_hdl_uuid, co_hdl_uuid, co_uuid, migrate_in->om_version,
			       migrate_in->om_generation, migrate_in->om_max_eph,
			       migrate_in->om_opc, oids, ephs, punched_ephs, shards, oids_count,
			       migrate_in->om_tgt_idx, migrate_in->om_new_layout_ver);
out:
	if (pool)
		ds_pool_put(pool);

	migrate_out = crt_reply_get(rpc);
	migrate_out->om_status = rc;
	dss_rpc_reply(rpc, DAOS_REBUILD_DROP_OBJ);
}

static int
obj_tree_lookup_cont(daos_handle_t toh, uuid_t co_uuid, daos_handle_t *cont_toh)
{
	struct tree_cache_root	*cont_root = NULL;
	d_iov_t			 key_iov;
	d_iov_t			 tmp_iov;
	daos_handle_t		 migrated_toh;
	struct umem_attr	 uma;
	int			 rc;

	D_ASSERT(daos_handle_is_valid(toh));

	d_iov_set(&key_iov, co_uuid, sizeof(uuid_t));
	d_iov_set(&tmp_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &tmp_iov);
	if (rc < 0) {
		if (rc != -DER_NONEXIST)
			D_ERROR("lookup cont "DF_UUID" failed, "DF_RC"\n",
				DP_UUID(co_uuid), DP_RC(rc));
		else
			D_DEBUG(DB_TRACE, "Container "DF_UUID" not exist\n",
				DP_UUID(co_uuid));
		return rc;
	}

	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;
	cont_root = tmp_iov.iov_buf;
	D_ASSERT(daos_handle_is_valid(cont_root->tcr_root_hdl));
	rc = dbtree_open_inplace(&cont_root->tcr_btr_root, &uma, &migrated_toh);
	if (rc == 0)
		*cont_toh = migrated_toh;
	else
		DL_ERROR(rc, DF_UUID" failed to open cont migrated tree", DP_UUID(co_uuid));

	return rc;
}

static int
obj_tree_lookup_uoid(daos_handle_t cont_toh, daos_unit_oid_t uoid)
{
	struct migrate_obj_val	val;
	d_iov_t			key_iov;
	d_iov_t			val_iov;

	d_iov_set(&key_iov, &uoid, sizeof(uoid));
	d_iov_set(&val_iov, &val, sizeof(struct migrate_obj_val));
	return dbtree_lookup(cont_toh, &key_iov, &val_iov);
}

#define REINT_ITER_YIELD_CNT	(256)
struct reint_post_iter_arg {
	daos_handle_t			 ria_migrated_tree_hdl;
	struct migrate_pool_tls		*ria_tls;
	daos_handle_t			 ria_cont_toh;
	uuid_t				 ria_co_uuid;
	int				 ria_yield_cnt;
};

static int
reint_post_obj_iter_cb(daos_handle_t ch, vos_iter_entry_t *entry,
		       vos_iter_type_t type, vos_iter_param_t *iter_param,
		       void *data, unsigned *acts)
{
	struct reint_post_iter_arg	*arg = data;
	struct migrate_pool_tls		*tls = arg->ria_tls;
	daos_unit_oid_t			 uoid = entry->ie_oid;
	int				 rc = 0;

	/* for non-replica obj don't try to delete it */
	if (daos_obj_id2ord(uoid.id_pub) == OR_RP_1)
		goto out;

	rc = obj_tree_lookup_uoid(arg->ria_cont_toh, uoid);
	if (rc) {
		if (rc == -DER_NONEXIST) {
			D_DEBUG(DB_TRACE, DF_RB": cont "DF_UUID", uoid "DF_UOID
				" non-exist in migrate tree, discard it.\n",
				DP_RB_MPT(tls), DP_UUID(arg->ria_co_uuid), DP_UOID(uoid));
			rc = vos_obj_delete(iter_param->ip_hdl, uoid);
			if (rc) {
				DL_ERROR(rc, DF_RB " vos_obj_delete, cont "DF_UUID", obj "DF_UOID
					 " failed", DP_RB_MPT(tls), DP_UUID(arg->ria_co_uuid),
					 DP_UOID(uoid));
				goto out;
			}
			*acts |= VOS_ITER_CB_DELETE;
			goto out;
		}

		DL_ERROR(rc, DF_RB " obj_tree_lookup_uoid "DF_UOID" failed",
			 DP_RB_MPT(tls), DP_UOID(uoid));
		goto out;
	}

out:
	if (--arg->ria_yield_cnt <= 0) {
		D_DEBUG(DB_REBUILD, DF_RB " rebuild yield: %d\n", DP_RB_MPT(tls), rc);
		arg->ria_yield_cnt = REINT_ITER_YIELD_CNT;
		if (rc == 0)
			dss_sleep(0);
		*acts |= VOS_ITER_CB_YIELD;
	}
	return rc;
}

static int
reint_post_cont_iter_cb(daos_handle_t ih, vos_iter_entry_t *entry,
			vos_iter_type_t type, vos_iter_param_t *iter_param,
			void *data, unsigned *acts)
{
	struct reint_post_iter_arg	*arg = data;
	struct migrate_pool_tls		*tls = arg->ria_tls;
	vos_iter_param_t		 param = { 0 };
	struct vos_iter_anchors		 anchor = { 0 };
	daos_handle_t			 toh = arg->ria_migrated_tree_hdl;
	daos_handle_t			 cont_toh = { 0 };
	struct ds_cont_child		*cont_child = NULL;
	int				 rc;

	rc = obj_tree_lookup_cont(toh, entry->ie_couuid, &cont_toh);
	if (rc) {
		if (rc == -DER_NONEXIST) {
			D_DEBUG(DB_TRACE, DF_RB": cont "DF_UUID" non-exist in migrate tree, "
				"discard it.\n", DP_RB_MPT(tls), DP_UUID(entry->ie_couuid));
			rc = ds_cont_child_destroy(tls->mpt_pool_uuid, entry->ie_couuid);
			if (rc) {
				DL_ERROR(rc, DF_RB " destroy container "DF_UUID" failed",
					 DP_RB_MPT(tls), DP_UUID(entry->ie_couuid));
				goto out;
			}
			*acts |= VOS_ITER_CB_DELETE;
			goto out;
		}

		DL_ERROR(rc, DF_RB " obj_tree_lookup_cont "DF_UUID" failed",
			 DP_RB_MPT(tls), DP_UUID(entry->ie_couuid));
		goto out;
	}

	D_ASSERT(daos_handle_is_valid(cont_toh));

	rc = ds_cont_child_lookup(tls->mpt_pool_uuid, entry->ie_couuid, &cont_child);
	if (rc == -DER_NONEXIST || rc == -DER_SHUTDOWN) {
		D_DEBUG(DB_REBUILD, DF_RB" co_uuid "DF_UUID" already destroyed or destroying, "
			DF_RC"\n", DP_RB_MPT(tls), DP_UUID(entry->ie_couuid), DP_RC(rc));
		rc = 0;
		goto out;
	}
	if (rc != 0) {
		DL_ERROR(rc, DF_RB " Container " DF_UUID ", ds_cont_child_lookup failed",
			 DP_RB_MPT(tls), DP_UUID(entry->ie_couuid));
		D_GOTO(out, rc);
	}

	param.ip_hdl = cont_child->sc_hdl;
	param.ip_epr.epr_lo = 0;
	param.ip_epr.epr_hi = DAOS_EPOCH_MAX;
	param.ip_flags = VOS_IT_FOR_MIGRATION;
	uuid_copy(arg->ria_co_uuid, entry->ie_couuid);
	arg->ria_cont_toh = cont_toh;
	rc = vos_iterate(&param, VOS_ITER_OBJ, false, &anchor,
			 reint_post_obj_iter_cb, NULL, arg, NULL);
	if (rc)
		DL_ERROR(rc, DF_RB " iterate container " DF_UUID " failed", DP_RB_MPT(tls),
			 DP_UUID(entry->ie_couuid));
	ds_cont_child_put(cont_child);

out:
	if (daos_handle_is_valid(cont_toh))
		dbtree_close(cont_toh);
	if (--arg->ria_yield_cnt <= 0) {
		D_DEBUG(DB_REBUILD, DF_RB " rebuild yield: %d\n", DP_RB_MPT(tls), rc);
		arg->ria_yield_cnt = REINT_ITER_YIELD_CNT;
		if (rc == 0)
			dss_sleep(0);
		*acts |= VOS_ITER_CB_YIELD;
	}

	return rc;
}

struct reint_post_ult_arg {
	struct migrate_pool_tls		*rpa_tls;
	struct btr_root			*rpa_migrated_root;
};

static void
reint_post_process_ult(void *data)
{
	struct reint_post_ult_arg	*arg = data;
	struct migrate_pool_tls		*tls;
	struct ds_pool_child		*pool_child = NULL;
	struct reint_post_iter_arg	 iter_arg = { 0 };
	vos_iter_param_t		 param = { 0 };
	struct vos_iter_anchors		 anchor = { 0 };
	daos_handle_t			 toh = { 0 };
	struct umem_attr		 uma;
	int				 rc = 0;

	tls = arg->rpa_tls;
	pool_child = ds_pool_child_lookup(tls->mpt_pool_uuid);
	if (pool_child == NULL) {
		rc = -DER_NONEXIST;
		DL_ERROR(rc, DF_RB": pool child lookup failed", DP_RB_MPT(tls));
		goto out;
	}

	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;
	rc = dbtree_open_inplace(arg->rpa_migrated_root, &uma, &toh);
	if (rc) {
		DL_ERROR(rc, DF_RB" migrated tree open failed", DP_RB_MPT(tls));
		goto out;
	}

	param.ip_hdl = pool_child->spc_hdl;
	param.ip_flags = VOS_IT_FOR_MIGRATION;
	iter_arg.ria_tls = tls;
	iter_arg.ria_yield_cnt = REINT_ITER_YIELD_CNT;
	iter_arg.ria_migrated_tree_hdl = toh;
	rc = vos_iterate(&param, VOS_ITER_COUUID, false, &anchor,
			 reint_post_cont_iter_cb, NULL, &iter_arg, NULL);
	if (rc)
		DL_ERROR(rc, DF_RB" vos_iterate failed.", DP_RB_MPT(tls));

out:
	if (daos_handle_is_valid(toh))
		dbtree_close(toh);
	if (pool_child != NULL)
		ds_pool_child_put(pool_child);
	if (tls->mpt_status == 0)
		tls->mpt_status = rc;
	tls->mpt_reintegrating = 0;
	D_FREE(arg);
	migrate_pool_tls_put(tls);
}

struct migrate_query_arg {
	uuid_t				pool_uuid;
	ABT_mutex			status_lock;
	struct btr_root			*mpt_migrated_root;
	struct ds_migrate_status	dms;
	uint32_t			version;
	uint32_t			total_ult_cnt;
	uint32_t			generation;
	daos_rebuild_opc_t		rebuild_op;
	uint32_t			mpt_reintegrating:1,
					reint_post_start:1,
					reint_post_processing:1;
};

static int
migrate_check_one(void *data)
{
	struct migrate_query_arg	*arg = data;
	struct reint_post_ult_arg	*ult_arg;
	struct migrate_pool_tls		*tls;
	uint32_t			 ult_cnt;
	bool				 reint_post_start = false;
	int				 rc = 0;

	tls = migrate_pool_tls_lookup(arg->pool_uuid, arg->version, arg->generation);
	if (tls == NULL)
		return 0;

	ult_cnt = atomic_load(tls->mpt_tgt_obj_ult_cnt) + atomic_load(tls->mpt_tgt_dkey_ult_cnt);

	ABT_mutex_lock(arg->status_lock);
	arg->dms.dm_rec_count += tls->mpt_rec_count;
	arg->dms.dm_obj_count += tls->mpt_obj_count;
	arg->dms.dm_total_size += tls->mpt_size;
	if (arg->dms.dm_status == 0)
		arg->dms.dm_status = tls->mpt_status;
	arg->total_ult_cnt += ult_cnt;
	if (tls->mpt_reintegrating) {
		arg->mpt_reintegrating = 1;
		if (arg->reint_post_start) {
			D_ASSERTF(arg->total_ult_cnt == 0, "total_ult_cnt %d, ult_cnt %d\n",
				  arg->total_ult_cnt, ult_cnt);
			if (tls->mpt_status == 0) {
				arg->reint_post_processing = 1;
				reint_post_start = true;
			} else {
				D_ERROR(DF_RB ": mpt_status %d, will not initiate reint post "
					"processing.\n", DP_RB_MPT(tls), tls->mpt_status);
				tls->mpt_reintegrating = 0;
			}
		} else if (tls->mpt_post_process_started) {
			arg->reint_post_processing = 1;
		}
	}
	ABT_mutex_unlock(arg->status_lock);
	D_DEBUG(DB_REBUILD,
		DF_RB " status %d/%d/ ult %u/%u rec/obj/size " DF_U64 "/" DF_U64 "/" DF_U64 "\n",
		DP_RB_MQA(arg), tls->mpt_status, arg->dms.dm_status,
		atomic_load(tls->mpt_tgt_obj_ult_cnt), atomic_load(tls->mpt_tgt_dkey_ult_cnt),
		tls->mpt_rec_count, tls->mpt_obj_count, tls->mpt_size);

	if (reint_post_start && !tls->mpt_post_process_started) {
		migrate_pool_tls_get(tls);
		tls->mpt_post_process_started = 1;
		D_ALLOC_PTR(ult_arg);
		if (ult_arg == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		ult_arg->rpa_tls = tls;
		ult_arg->rpa_migrated_root = arg->mpt_migrated_root;
		rc = dss_ult_create(reint_post_process_ult, ult_arg, DSS_XS_SELF, 0,
				    MIGRATE_STACK_SIZE, NULL);
		if (rc) {
			DL_ERROR(rc, DF_RB ": create reint post process ULT failed",
				 DP_RB_MPT(tls));
			tls->mpt_reintegrating = 0;
			migrate_pool_tls_put(tls);
			D_FREE(ult_arg);
			goto out;
		}
	}

out:
	if (tls->mpt_status == 0)
		tls->mpt_status = rc;
	migrate_pool_tls_put(tls);
	return 0;
}

int
ds_migrate_query_status(uuid_t pool_uuid, uint32_t ver, unsigned int generation, int op,
			bool gl_scan_done, struct ds_migrate_status *dms)
{
	struct migrate_query_arg	arg = { 0 };
	struct migrate_pool_tls		*tls;
	int				rc;

	tls = migrate_pool_tls_lookup(pool_uuid, ver, generation);
	if (tls == NULL)
		return 0;

	uuid_copy(arg.pool_uuid, pool_uuid);
	arg.version = ver;
	arg.generation = generation;
	arg.rebuild_op = op;
	arg.mpt_migrated_root = &tls->mpt_migrated_root;
	rc = ABT_mutex_create(&arg.status_lock);
	if (rc != ABT_SUCCESS)
		D_GOTO(out, rc);

	rc = ds_pool_thread_collective(pool_uuid, PO_COMP_ST_NEW | PO_COMP_ST_DOWN |
				       PO_COMP_ST_DOWNOUT, migrate_check_one, &arg, 0);
	if (rc)
		D_GOTO(out, rc);

	/* when globally scan done, and locally pull done, for reintegration need to do some post
	 * processing, cannot report riv_pull_done before the post processing complete.
	 */
	if (gl_scan_done && arg.total_ult_cnt == 0 && !tls->mpt_ult_running &&
	    arg.mpt_reintegrating && !arg.reint_post_processing) {
		arg.reint_post_start = 1;
		rc = ds_pool_thread_collective(pool_uuid, PO_COMP_ST_NEW | PO_COMP_ST_DOWN |
					       PO_COMP_ST_DOWNOUT, migrate_check_one, &arg, 0);
		if (rc)
			D_GOTO(out, rc);
	}

	if (!gl_scan_done || arg.total_ult_cnt > 0 || tls->mpt_ult_running ||
	    arg.reint_post_processing)
		arg.dms.dm_migrating = 1;
	else
		arg.dms.dm_migrating = 0;

	if (dms != NULL)
		*dms = arg.dms;

	migrate_system_try_wakeup(tls);
	D_DEBUG(DB_REBUILD,
		DF_RB " migrating=%s, obj_count=" DF_U64 ", rec_count=" DF_U64 ", size=" DF_U64
		      " ult_cnt %u, mpt_ult_running %d, reint_post_processing %d, status %d\n",
		DP_RB_MQA(&arg), arg.dms.dm_migrating ? "yes" : "no", arg.dms.dm_obj_count,
		arg.dms.dm_rec_count, arg.dms.dm_total_size, arg.total_ult_cnt,
		tls->mpt_ult_running, arg.reint_post_processing, arg.dms.dm_status);

out:
	ABT_mutex_free(&arg.status_lock);
	migrate_pool_tls_put(tls);
	return rc;
}

/**
 * send the object to the new target to Migrate there (@tgt_id).
 *
 * param pool [in]		ds_pool of the pool.
 * param pool_hdl_uuid [in]	pool_handle uuid.
 * param cont_uuid [in]		container uuid.
 * param cont_hdl_uuid [in]	container handle uuid.
 * param tgt_id [in]		target id where the data to be migrated.
 * param version [in]		rebuild version.
 * param generation [in]	rebuild generation.
 * param max_eph [in]		maxim epoch of the migration.
 * param max_eph [in]		maxim epoch of the migration.
 * param max_eph [in]		maxim epoch of the migration.
 * param oids [in]		array of the objects to be migrated.
 * param ephs [in]		epoch of the objects.
 * param punched_ephs [in]	punched_epoch of objects.
 * param shards [in]		it can be NULL, otherwise it indicates
 *				the source shard of the migration, so it
 *				is only used for replicate objects.
 * param cnt [in]		count of objects.
 * param new_layout_ver [in]	new layout version during upgrade.
 * param migrate_opc [in]	operation which cause the migration.
 *
 * return			0 if it succeeds, otherwise errno.
 */
int
ds_object_migrate_send(struct ds_pool *pool, uuid_t pool_hdl_uuid, uuid_t cont_hdl_uuid,
		       uuid_t cont_uuid, int tgt_id, uint32_t version, unsigned int generation,
		       uint64_t max_eph, daos_unit_oid_t *oids, daos_epoch_t *ephs,
		       daos_epoch_t *punched_ephs, unsigned int *shards, int cnt,
		       uint32_t new_layout_ver, uint32_t migrate_opc,
		       uint64_t *enqueue_id, uint32_t *max_delay)
{
	struct obj_migrate_in	*migrate_in = NULL;
	struct obj_migrate_out	*migrate_out = NULL;
	struct pool_target	*target;
	crt_endpoint_t		tgt_ep = {0};
	crt_opcode_t		opcode;
	unsigned int		index;
	crt_rpc_t		*rpc;
	int			rc;
	uint32_t		rpc_timeout = 0;

	ABT_rwlock_rdlock(pool->sp_lock);
	rc = pool_map_find_target(pool->sp_map, tgt_id, &target);
	if (rc != 1 || (target->ta_comp.co_status != PO_COMP_ST_UPIN
			&& target->ta_comp.co_status != PO_COMP_ST_UP
			&& target->ta_comp.co_status != PO_COMP_ST_NEW)) {
		/* Remote target has failed, no need retry, but not
		 * report failure as well and next rebuild will handle
		 * it anyway.
		 */
		ABT_rwlock_unlock(pool->sp_lock);
		D_DEBUG(DB_TRACE, "Can not find tgt %d or target is down %d\n",
			tgt_id, target->ta_comp.co_status);
		return -DER_NONEXIST;
	}

	/* NB: let's send object list to 0 xstream to simplify the migrate
	 * object handling process for now, for example avoid lock to insert
	 * objects in the object tree.
	 */
	tgt_ep.ep_rank = target->ta_comp.co_rank;
	index = target->ta_comp.co_index;
	ABT_rwlock_unlock(pool->sp_lock);
	tgt_ep.ep_tag = 0;
	opcode = DAOS_RPC_OPCODE(DAOS_OBJ_RPC_MIGRATE, DAOS_OBJ_MODULE,
				 DAOS_OBJ_VERSION);
	rc = crt_req_create(dss_get_module_info()->dmi_ctx, &tgt_ep, opcode,
			    &rpc);
	if (rc) {
		D_ERROR("crt_req_create failed: "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	migrate_in = crt_req_get(rpc);
	uuid_copy(migrate_in->om_pool_uuid, pool->sp_uuid);
	uuid_copy(migrate_in->om_poh_uuid, pool_hdl_uuid);
	uuid_copy(migrate_in->om_cont_uuid, cont_uuid);
	uuid_copy(migrate_in->om_coh_uuid, cont_hdl_uuid);
	migrate_in->om_version = version;
	migrate_in->om_generation = generation;
	migrate_in->om_max_eph = max_eph,
	migrate_in->om_tgt_idx = index;
	migrate_in->om_new_layout_ver = new_layout_ver;
	migrate_in->om_opc = migrate_opc;

	migrate_in->om_oids.ca_arrays = oids;
	migrate_in->om_oids.ca_count = cnt;
	migrate_in->om_ephs.ca_arrays = ephs;
	migrate_in->om_ephs.ca_count = cnt;
	migrate_in->om_punched_ephs.ca_arrays = punched_ephs;
	migrate_in->om_punched_ephs.ca_count = cnt;
	migrate_in->om_comm_in.req_in_enqueue_id = *enqueue_id;
	crt_req_get_timeout(rpc, &rpc_timeout);

	if (shards) {
		migrate_in->om_shards.ca_arrays = shards;
		migrate_in->om_shards.ca_count = cnt;
	}
	rc = dss_rpc_send(rpc);
	if (rc) {
		D_ERROR("dss_rpc_send failed: "DF_RC"\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	migrate_out = crt_reply_get(rpc);
	rc = migrate_out->om_status;
	if (rc == -DER_OVERLOAD_RETRY) {
		*enqueue_id = migrate_out->om_comm_out.req_out_enqueue_id;
		*max_delay = rpc_timeout;
	}
out:
	D_DEBUG(DB_REBUILD, DF_RB ": rc=%d\n", DP_RB_OMI(migrate_in), rc);
	if (rpc)
		crt_req_decref(rpc);

	return rc;
}
