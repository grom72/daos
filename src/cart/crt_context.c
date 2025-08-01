/*
 * (C) Copyright 2016-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 * (C) Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of CaRT. It implements the CaRT context related APIs.
 */
#define D_LOGFAC	DD_FAC(rpc)

#include "crt_internal.h"

static void
crt_epi_destroy(struct crt_ep_inflight *epi);
static void
context_quotas_init(struct crt_context *ctx);
static void
context_quotas_finalize(struct crt_context *ctx);

static struct crt_ep_inflight *
epi_link2ptr(d_list_t *rlink)
{
	D_ASSERT(rlink != NULL);
	return container_of(rlink, struct crt_ep_inflight, epi_link);
}

static uint32_t
epi_op_key_hash(struct d_hash_table *hhtab, const void *key,
		unsigned int ksize)
{
	D_ASSERT(ksize == sizeof(d_rank_t));

	return (uint32_t)(*(const uint32_t *)key
			 & ((1U << CRT_EPI_TABLE_BITS) - 1));
}

static bool
epi_op_key_cmp(struct d_hash_table *hhtab, d_list_t *rlink,
	  const void *key, unsigned int ksize)
{
	struct crt_ep_inflight *epi = epi_link2ptr(rlink);

	D_ASSERT(ksize == sizeof(d_rank_t));
	/* TODO: use global rank */

	return epi->epi_ep.ep_rank == *(d_rank_t *)key;
}

static uint32_t
epi_op_rec_hash(struct d_hash_table *htable, d_list_t *link)
{
	struct crt_ep_inflight *epi = epi_link2ptr(link);

	return (uint32_t)epi->epi_ep.ep_rank
			& ((1U << CRT_EPI_TABLE_BITS) - 1);
}

static void
epi_op_rec_addref(struct d_hash_table *hhtab, d_list_t *rlink)
{
	epi_link2ptr(rlink)->epi_ref++;
}

static bool
epi_op_rec_decref(struct d_hash_table *hhtab, d_list_t *rlink)
{
	struct crt_ep_inflight *epi = epi_link2ptr(rlink);

	epi->epi_ref--;
	return epi->epi_ref == 0;
}

static void
epi_op_rec_free(struct d_hash_table *hhtab, d_list_t *rlink)
{
	crt_epi_destroy(epi_link2ptr(rlink));
}

static d_hash_table_ops_t epi_table_ops = {
	.hop_key_hash		= epi_op_key_hash,
	.hop_key_cmp		= epi_op_key_cmp,
	.hop_rec_hash		= epi_op_rec_hash,
	.hop_rec_addref		= epi_op_rec_addref,
	.hop_rec_decref		= epi_op_rec_decref,
	.hop_rec_free		= epi_op_rec_free,
};

static void
crt_epi_destroy(struct crt_ep_inflight *epi)
{
	D_ASSERT(epi != NULL);

	D_ASSERT(epi->epi_ref == 0);
	D_ASSERT(epi->epi_initialized == 1);

	D_ASSERT(d_list_empty(&epi->epi_req_waitq));
	D_ASSERT(epi->epi_req_wait_num == 0);

	D_ASSERT(d_list_empty(&epi->epi_req_q));
	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);

	/* crt_list_del_init(&epi->epi_link); */
	D_MUTEX_DESTROY(&epi->epi_mutex);

	D_FREE(epi);
}

static int
crt_ep_empty(d_list_t *rlink, void *arg)
{
	struct crt_ep_inflight	*epi;

	epi = epi_link2ptr(rlink);

	return (d_list_empty(&epi->epi_req_waitq) &&
		epi->epi_req_wait_num == 0 &&
		d_list_empty(&epi->epi_req_q) &&
		epi->epi_req_num >= epi->epi_reply_num) ? 0 : 1;
}

bool
crt_context_ep_empty(crt_context_t crt_ctx)
{
	struct crt_context	*ctx;
	int			 rc;

	ctx = crt_ctx;
	D_MUTEX_LOCK(&ctx->cc_mutex);
	rc = d_hash_table_traverse(&ctx->cc_epi_table, crt_ep_empty, NULL);
	D_MUTEX_UNLOCK(&ctx->cc_mutex);

	return rc == 0;
}

static int
crt_context_init(struct crt_context *ctx)
{
	uint32_t bh_node_cnt;
	int      rc;

	D_ASSERT(ctx != NULL);

	rc = D_MUTEX_INIT(&ctx->cc_mutex, NULL);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = D_MUTEX_INIT(&ctx->cc_quotas.mutex, NULL);
	if (rc != 0) {
		D_MUTEX_DESTROY(&ctx->cc_mutex);
		D_GOTO(out, rc);
	}

	D_INIT_LIST_HEAD(&ctx->cc_quotas.rpc_waitq);
	D_INIT_LIST_HEAD(&ctx->cc_link);

	/* create timeout binheap */
	bh_node_cnt = CRT_DEFAULT_CREDITS_PER_EP_CTX * 64;
	rc          = d_binheap_create_inplace(DBH_FT_NOLOCK, bh_node_cnt, NULL /* priv */,
					       &crt_timeout_bh_ops, &ctx->cc_bh_timeout);
	if (rc != 0) {
		D_ERROR("d_binheap_create() failed, " DF_RC "\n", DP_RC(rc));
		D_GOTO(out_mutex_destroy, rc);
	}

	/* create epi table, use external lock */
	rc = d_hash_table_create_inplace(D_HASH_FT_NOLOCK, CRT_EPI_TABLE_BITS, NULL, &epi_table_ops,
					 &ctx->cc_epi_table);
	if (rc != 0) {
		D_ERROR("d_hash_table_create() failed, " DF_RC "\n", DP_RC(rc));
		D_GOTO(out_binheap_destroy, rc);
	}

	context_quotas_init(ctx);

	return 0;

out_binheap_destroy:
	d_binheap_destroy_inplace(&ctx->cc_bh_timeout);
out_mutex_destroy:
	D_MUTEX_DESTROY(&ctx->cc_quotas.mutex);
	D_MUTEX_DESTROY(&ctx->cc_mutex);
out:
	return rc;
}

int
crt_context_uri_get(crt_context_t crt_ctx, char **uri)
{
	struct crt_context	*ctx = NULL;

	if (crt_ctx == NULL || uri == NULL) {
		D_ERROR("Invalid null parameters (%p) (%p)\n", crt_ctx, uri);
		return -DER_INVAL;
	}

	ctx = crt_ctx;
	D_STRNDUP(*uri, ctx->cc_self_uri, CRT_ADDR_STR_MAX_LEN);
	if (*uri == NULL)
		return -DER_NOMEM;

	return DER_SUCCESS;
}

static int
crt_context_provider_create(crt_context_t *crt_ctx, crt_provider_t provider, bool primary,
			    int iface_idx)
{
	struct crt_context	*ctx = NULL;
	int			rc = 0;
	size_t			uri_len = CRT_ADDR_STR_MAX_LEN;
	int			ctx_idx;
	int			max_ctx_num;
	d_list_t		*ctx_list;

	if (crt_ctx == NULL) {
		D_ERROR("invalid parameter of NULL crt_ctx.\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	D_RWLOCK_WRLOCK(&crt_gdata.cg_rwlock);
	ctx_idx = crt_provider_get_ctx_idx(primary, provider);

	if (ctx_idx < 0) {
		max_ctx_num = crt_provider_get_max_ctx_num(primary, provider);
		D_WARN("Provider: %d; Context limit (%d) reached\n",
		       provider, max_ctx_num);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_GOTO(out, rc = -DER_AGAIN);
	}

	D_ALLOC_PTR(ctx);
	if (ctx == NULL) {
		crt_provider_put_ctx_idx(primary, provider, ctx_idx);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_GOTO(out, rc = -DER_NOMEM);
	}

	rc = crt_context_init(ctx);
	if (rc != 0) {
		D_ERROR("crt_context_init() failed, " DF_RC "\n", DP_RC(rc));
		D_FREE(ctx);
		crt_provider_put_ctx_idx(primary, provider, ctx_idx);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_GOTO(out, rc);
	}

	ctx->cc_primary = primary;
	ctx->cc_idx = ctx_idx;

	rc = crt_hg_ctx_init(&ctx->cc_hg_ctx, provider, ctx_idx, primary, iface_idx);
	if (rc != 0) {
		D_ERROR("crt_hg_ctx_init() failed, " DF_RC "\n", DP_RC(rc));
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		crt_context_destroy(ctx, true);
		D_GOTO(out, rc);
	}

	rc = crt_hg_get_addr(ctx->cc_hg_ctx.chc_hgcla, ctx->cc_self_uri, &uri_len);
	if (rc != 0) {
		D_ERROR("ctx_hg_get_addr() failed; rc: %d.\n", rc);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		crt_context_destroy(ctx, true);
		D_GOTO(out, rc);
	}

	ctx_list = crt_provider_get_ctx_list(primary, provider);
	d_list_add_tail(&ctx->cc_link, ctx_list);

	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	/** initialize sensors for servers */
	if (crt_gdata.cg_use_sensors && crt_is_service()) {
		int	ret;
		char	*prov;

		prov = crt_provider_name_get(ctx->cc_hg_ctx.chc_provider);
		ret = d_tm_add_metric(&ctx->cc_timedout, D_TM_COUNTER,
				      "Total number of timed out RPC requests",
				      "reqs", "net/%s/req_timeout/ctx_%u",
				      prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create timed out req counter");

		ret = d_tm_add_metric(&ctx->cc_timedout_uri, D_TM_COUNTER,
				      "Total number of timed out URI lookup "
				      "requests", "reqs",
				      "net/%s/uri_lookup_timeout/ctx_%u",
				      prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create timed out uri req counter");

		ret = d_tm_add_metric(&ctx->cc_failed_addr, D_TM_COUNTER,
				      "Total number of failed address "
				      "resolution attempts", "reqs",
				      "net/%s/failed_addr/ctx_%u",
				      prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create failed addr counter");

		ret = d_tm_add_metric(&ctx->cc_net_glitches, D_TM_COUNTER,
				      "Total number of network glitch errors", "errors",
				      "net/%s/glitch/ctx_%u", prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create network glitch counter");

		ret = d_tm_add_metric(&ctx->cc_swim_delay, D_TM_STATS_GAUGE,
				      "SWIM delay measurements", "delay",
				      "net/%s/swim_delay/ctx_%u", prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create SWIM delay gauge");

		ret = d_tm_add_metric(&ctx->cc_quotas.rpc_waitq_depth, D_TM_GAUGE,
				      "Current count of enqueued RPCs", "rpcs",
				      "net/%s/waitq_depth/ctx_%u", prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create rpc waitq gauge");

		ret = d_tm_add_metric(&ctx->cc_quotas.rpc_quota_exceeded, D_TM_COUNTER,
				      "Total number of exceeded RPC quota errors", "errors",
				      "net/%s/quota_exceeded/ctx_%u", prov, ctx->cc_idx);
		if (ret)
			DL_WARN(ret, "Failed to create quota exceeded counter");
	}

	if (crt_is_service() && crt_gdata.cg_auto_swim_disable == 0 &&
	    ctx->cc_idx == crt_gdata.cg_swim_ctx_idx) {
		rc = crt_swim_init(crt_gdata.cg_swim_ctx_idx);
		if (rc) {
			D_ERROR("crt_swim_init() failed rc: %d.\n", rc);
			crt_context_destroy(ctx, true);
			D_GOTO(out, rc);
		}

		/* TODO: Address this hack */
		if (provider == CRT_PROV_OFI_SOCKETS || provider == CRT_PROV_OFI_TCP_RXM) {
			struct crt_grp_priv	*grp_priv = crt_gdata.cg_grp->gg_primary_grp;
			struct crt_swim_membs	*csm = &grp_priv->gp_membs_swim;

			D_DEBUG(DB_TRACE, "Slow network provider is detected, "
					  "increase SWIM timeouts by twice.\n");

			swim_suspect_timeout_set(swim_suspect_timeout_get() * 2);
			swim_ping_timeout_set(swim_ping_timeout_get() * 2);
			swim_period_set(swim_period_get() * 2);
			csm->csm_ctx->sc_default_ping_timeout *= 2;
		}
	}

	*crt_ctx = (crt_context_t)ctx;
	D_DEBUG(DB_TRACE, "created context (idx %d)\n", ctx->cc_idx);

out:
	return rc;
}

bool
crt_context_is_primary(crt_context_t crt_ctx)
{
	struct crt_context *ctx = crt_ctx;

	return ctx->cc_primary;
}

int
crt_context_create(crt_context_t *crt_ctx)
{
	return crt_context_provider_create(crt_ctx, crt_gdata.cg_primary_prov, true, 0);
}

uint32_t
crt_num_ifaces_get(void)
{
	return crt_provider_num_ifaces_get(true, crt_gdata.cg_primary_prov);
}

int
crt_context_create_on_iface_idx(uint32_t iface_index, crt_context_t *crt_ctx)
{
	uint32_t num_ifaces;

	if (crt_is_service()) {
		D_ERROR("API not available on servers\n");
		return -DER_NOSYS;
	}

	num_ifaces = crt_num_ifaces_get();
	if (num_ifaces == 0) {
		D_ERROR("No interfaces specified at startup\n");
		return -DER_INVAL;
	}

	if (iface_index >= num_ifaces) {
		D_ERROR("interface index %d outside of range [0-%d]\n",
			iface_index, num_ifaces - 1);
		return -DER_INVAL;
	}

	return crt_context_provider_create(crt_ctx, crt_gdata.cg_primary_prov, true, iface_index);
}

int
crt_iface_name2idx(const char *iface_name, int *idx)
{
	uint32_t	num_ifaces;
	int		i;
	char		*name;

	num_ifaces = crt_provider_num_ifaces_get(true, crt_gdata.cg_primary_prov);

	for (i = 0; i < num_ifaces; i++) {
		name = crt_provider_iface_str_get(true, crt_gdata.cg_primary_prov, i);

		if (!name)
			return -DER_INVAL;

		if (strcmp(name, iface_name) == 0) {
			*idx = i;
			return DER_SUCCESS;
		}
	}

	return -DER_INVAL;
}

int
crt_context_create_on_iface(const char *iface_name, crt_context_t *crt_ctx)
{
	int idx;
	int rc;

	rc = crt_iface_name2idx(iface_name, &idx);
	if (rc != 0)
		D_GOTO(out, rc);

	D_DEBUG(DB_ALL, "%s resolved to index=%d\n", iface_name, idx);

	return crt_context_create_on_iface_idx(idx, crt_ctx);
out:
	return rc;
}


/* TODO: Add crt_context_create_secondary_on_iface_idx() if needed */
int
crt_context_create_secondary(crt_context_t *crt_ctx, int idx)
{
	crt_provider_t sec_prov;

	if (crt_gdata.cg_secondary_provs == NULL) {
		D_ERROR("Secondary provider not initialized\n");
		return -DER_INVAL;
	}

	/* TODO: Use idx later to ref other providers */
	sec_prov = crt_gdata.cg_secondary_provs[0];
	if (sec_prov == CRT_PROV_UNKNOWN) {
		D_ERROR("Unknown secondary provider\n");
		return -DER_INVAL;
	}

	return crt_context_provider_create(crt_ctx, sec_prov, false, 0 /* interface index */);
}

int
crt_context_register_rpc_task(crt_context_t ctx, crt_rpc_task_t process_cb,
			      crt_rpc_task_t iv_resp_cb, void *arg)
{
	struct crt_context *crt_ctx = ctx;

	if (ctx == CRT_CONTEXT_NULL || process_cb == NULL) {
		D_ERROR("Invalid parameter: ctx %p cb %p\n",
			ctx, process_cb);
		return -DER_INVAL;
	}

	crt_ctx->cc_rpc_cb = process_cb;
	crt_ctx->cc_iv_resp_cb = iv_resp_cb;
	crt_ctx->cc_rpc_cb_arg = arg;
	return 0;
}

bool
crt_rpc_completed(struct crt_rpc_priv *rpc_priv)
{
	bool	rc = false;

	D_SPIN_LOCK(&rpc_priv->crp_lock);
	if (rpc_priv->crp_completed) {
		rc = true;
	} else {
		rpc_priv->crp_completed = 1;
		rc = false;
	}
	D_SPIN_UNLOCK(&rpc_priv->crp_lock);

	return rc;
}

void
crt_rpc_complete_and_unlock(struct crt_rpc_priv *rpc_priv, int rc)
{
	D_ASSERT(rpc_priv != NULL);

	if (crt_rpc_completed(rpc_priv)) {
		crt_rpc_unlock(rpc_priv);
		RPC_ERROR(rpc_priv, "already completed, possibly due to duplicated completions.\n");
		return;
	}

	if (rc == -DER_CANCELED)
		rpc_priv->crp_state = RPC_STATE_CANCELED;
	else if (rc == -DER_TIMEDOUT)
		rpc_priv->crp_state = RPC_STATE_TIMEOUT;
	else if (rc == -DER_UNREACH)
		rpc_priv->crp_state = RPC_STATE_FWD_UNREACH;
	else
		rpc_priv->crp_state = RPC_STATE_COMPLETED;

	crt_rpc_unlock(rpc_priv);

	if (rpc_priv->crp_complete_cb != NULL) {
		struct crt_cb_info	cbinfo;

		cbinfo.cci_rpc = &rpc_priv->crp_pub;
		cbinfo.cci_arg = rpc_priv->crp_arg;
		cbinfo.cci_rc = rc;
		if (cbinfo.cci_rc == 0)
			cbinfo.cci_rc = rpc_priv->crp_reply_hdr.cch_rc;

		if (cbinfo.cci_rc != 0)
			RPC_CWARN(crt_quiet_error(cbinfo.cci_rc), DB_NET, rpc_priv,
				  "failed, " DF_RC "\n", DP_RC(cbinfo.cci_rc));

		RPC_TRACE(DB_TRACE, rpc_priv,
			  "Invoking RPC callback (rank %d tag %d) rc: "
			  DF_RC "\n",
			  rpc_priv->crp_pub.cr_ep.ep_rank,
			  rpc_priv->crp_pub.cr_ep.ep_tag,
			  DP_RC(cbinfo.cci_rc));

		rpc_priv->crp_complete_cb(&cbinfo);
	}

	RPC_DECREF(rpc_priv);
}

/* Flag bits definition for crt_ctx_epi_abort */
#define CRT_EPI_ABORT_FORCE	(0x1)
#define CRT_EPI_ABORT_WAIT	(0x2)

/* abort the RPCs in in-flight queue and waitq in the epi. */
static int
crt_ctx_epi_abort(struct crt_ep_inflight *epi, int flags)
{
	struct crt_context	*ctx;
	struct crt_rpc_priv	*rpc_priv, *rpc_next;
	struct d_vec_pointers	 rpcs;
	bool			 msg_logged;
	int			 force, wait;
	uint64_t		 ts_start, ts_now;
	int			 i;
	int			 rc = 0;

	rc = d_vec_pointers_init(&rpcs, 8 /* cap */);
	if (rc != 0)
		D_GOTO(out, rc);

	/*
	 * DAOS-7306: This mutex is needed in order to avoid double
	 * completions that would happen otherwise. safe list processing
	 * is not sufficient to avoid the race
	 */
	D_MUTEX_LOCK(&epi->epi_mutex);

	ctx = epi->epi_ctx;
	D_ASSERT(ctx != NULL);

	/* empty queue, nothing to do */
	if (d_list_empty(&epi->epi_req_waitq) &&
	    d_list_empty(&epi->epi_req_q))
		D_GOTO(out_mutex, rc = 0);

	force = flags & CRT_EPI_ABORT_FORCE;
	wait = flags & CRT_EPI_ABORT_WAIT;
	if (force == 0) {
		D_ERROR("cannot abort endpoint (idx %d, rank %d, req_wait_num "
			DF_U64", req_num "DF_U64", reply_num "DF_U64", "
			"in-flight "DF_U64", with force == 0.\n", ctx->cc_idx,
			epi->epi_ep.ep_rank, epi->epi_req_wait_num,
			epi->epi_req_num, epi->epi_reply_num,
			epi->epi_req_num - epi->epi_reply_num);
		D_GOTO(out_mutex, rc = -DER_BUSY);
	}

	/* take references to RPCs in waitq */
	msg_logged = false;
	d_list_for_each_entry_safe(rpc_priv, rpc_next, &epi->epi_req_waitq, crp_epi_link) {
		D_ASSERT(epi->epi_req_wait_num > 0);
		if (msg_logged == false) {
			D_DEBUG(DB_NET, "destroy context (idx %d, rank %d, "
				"req_wait_num "DF_U64").\n", ctx->cc_idx,
				epi->epi_ep.ep_rank, epi->epi_req_wait_num);
			msg_logged = true;
		}

		RPC_ADDREF(rpc_priv);
		rc = d_vec_pointers_append(&rpcs, rpc_priv);
		if (rc != 0) {
			RPC_DECREF(rpc_priv);
			D_GOTO(out_mutex, rc);
		}
	}

	/* take references to RPCs in in-flight queue */
	msg_logged = false;
	d_list_for_each_entry_safe(rpc_priv, rpc_next, &epi->epi_req_q, crp_epi_link) {
		D_ASSERT(epi->epi_req_num > epi->epi_reply_num);
		if (msg_logged == false) {
			D_DEBUG(DB_NET,
				"destroy context (idx %d, rank %d, "
				"epi_req_num "DF_U64", epi_reply_num "
				""DF_U64", in-flight "DF_U64").\n",
				ctx->cc_idx, epi->epi_ep.ep_rank,
				epi->epi_req_num, epi->epi_reply_num,
				epi->epi_req_num - epi->epi_reply_num);
			msg_logged = true;
		}

		RPC_ADDREF(rpc_priv);
		rc = d_vec_pointers_append(&rpcs, rpc_priv);
		if (rc != 0) {
			RPC_DECREF(rpc_priv);
			D_GOTO(out_mutex, rc);
		}
	}

	D_MUTEX_UNLOCK(&epi->epi_mutex);
	for (i = 0; i < rpcs.p_len; i++) {
		rpc_priv = rpcs.p_buf[i];
		rc = crt_req_abort(&rpc_priv->crp_pub);
		if (rc != 0) {
			D_DEBUG(DB_NET,
				"crt_req_abort(opc: %#x) failed, rc: %d.\n",
				rpc_priv->crp_pub.cr_opc, rc);
			rc = 0;
			continue;
		}
	}
	D_MUTEX_LOCK(&epi->epi_mutex);

	ts_start = d_timeus_secdiff(0);
	while (wait != 0) {
		/* make sure all above aborting finished */
		if (d_list_empty(&epi->epi_req_waitq) &&
		    d_list_empty(&epi->epi_req_q)) {
			wait = 0;
		} else {
			D_MUTEX_UNLOCK(&epi->epi_mutex);
			rc = crt_progress(ctx, 1);
			D_MUTEX_LOCK(&epi->epi_mutex);
			if (rc != 0 && rc != -DER_TIMEDOUT) {
				D_ERROR("crt_progress failed, rc %d.\n", rc);
				break;
			}
			ts_now = d_timeus_secdiff(0);
			if (ts_now - ts_start > 2 * CRT_DEFAULT_TIMEOUT_US) {
				D_ERROR("stop progress due to timed out.\n");
				d_list_for_each_entry(rpc_priv, &epi->epi_req_q, crp_epi_link)
					RPC_ERROR(rpc_priv,
						  "in-flight: still not aborted: state=%d\n",
						  rpc_priv->crp_state);
				d_list_for_each_entry(rpc_priv, &epi->epi_req_waitq, crp_epi_link)
					RPC_ERROR(rpc_priv,
						  "waiting: still not aborted: state=%d\n",
						  rpc_priv->crp_state);
				rc = -DER_TIMEDOUT;
				break;
			}
		}
	}

out_mutex:
	D_MUTEX_UNLOCK(&epi->epi_mutex);
	for (i = 0; i < rpcs.p_len; i++) {
		rpc_priv = rpcs.p_buf[i];
		RPC_DECREF(rpc_priv);
	}
	d_vec_pointers_fini(&rpcs);
out:
	return rc;
}

/* See crt_rank_abort. */
static pthread_rwlock_t crt_context_destroy_lock = PTHREAD_RWLOCK_INITIALIZER;

static int
crt_context_epis_append(d_list_t *rlink, void *arg)
{
	struct d_vec_pointers	*epis = arg;
	struct crt_ep_inflight	*epi = epi_link2ptr(rlink);
	int			 rc;

	d_hash_rec_addref(&epi->epi_ctx->cc_epi_table, rlink);
	rc = d_vec_pointers_append(epis, epi);
	if (rc != 0)
		d_hash_rec_decref(&epi->epi_ctx->cc_epi_table, rlink);
	return rc;
}

static int
crt_context_abort(struct crt_context *ctx, bool force)
{
	struct d_vec_pointers	epis;
	int			flags;
	int			i;
	int			rc;

	rc = d_vec_pointers_init(&epis, 16 /* cap */);
	if (rc != 0)
		D_GOTO(out, rc);

	D_MUTEX_LOCK(&ctx->cc_mutex);
	rc = d_hash_table_traverse(&ctx->cc_epi_table, crt_context_epis_append, &epis);
	D_MUTEX_UNLOCK(&ctx->cc_mutex);
	if (rc != 0)
		D_GOTO(out_epis, rc);

	flags = force ? (CRT_EPI_ABORT_FORCE | CRT_EPI_ABORT_WAIT) : 0;

	for (i = 0; i < epis.p_len; i++) {
		struct crt_ep_inflight *epi = epis.p_buf[i];

		rc = crt_ctx_epi_abort(epi, flags);
		if (rc != 0)
			break;
	}

out_epis:
	D_MUTEX_LOCK(&ctx->cc_mutex);
	for (i = 0; i < epis.p_len; i++) {
		struct crt_ep_inflight *epi = epis.p_buf[i];

		d_hash_rec_decref(&ctx->cc_epi_table, &epi->epi_link);
	}
	D_MUTEX_UNLOCK(&ctx->cc_mutex);
	d_vec_pointers_fini(&epis);
out:
	return rc;
}

int
crt_context_destroy(crt_context_t crt_ctx, int force)
{
	struct crt_context      *ctx = crt_ctx;
	uint32_t                 timeout_sec;
	int                      ctx_idx;
	int                      provider;
	int                      rc    = 0;
	int                      hg_rc = 0;
	int                      i;

	D_RWLOCK_RDLOCK(&crt_context_destroy_lock);

	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("invalid parameter (NULL crt_ctx).\n");
		D_GOTO(out, rc = -DER_INVAL);
	}
	if (!crt_initialized()) {
		D_ERROR("CRT not initialized.\n");
		D_GOTO(out, rc = -DER_UNINIT);
	}

	context_quotas_finalize(ctx);

	rc = crt_context_idx(crt_ctx, &ctx_idx);
	if (rc != 0) {
		D_ERROR("crt_context_idx() failed: " DF_RC "\n", DP_RC(rc));
		D_GOTO(out, rc);
	}

	hg_rc = HG_Context_unpost(ctx->cc_hg_ctx.chc_hgctx);
	if (hg_rc != 0) {
		if (!force)
			D_GOTO(out, rc = -DER_INVAL);
	}

	if (crt_gdata.cg_swim_inited && crt_gdata.cg_swim_ctx_idx == ctx_idx)
		crt_swim_disable_all();

	rc = crt_grp_ctx_invalid(ctx, false /* locked */);
	if (rc) {
		DL_ERROR(rc, "crt_grp_ctx_invalid() failed");
		if (!force)
			D_GOTO(out, rc);
	}

	timeout_sec = crt_swim_rpc_timeout();
	for (i = 0; i < CRT_SWIM_FLUSH_ATTEMPTS; i++) {
		rc = crt_context_abort(ctx, force);
		if (rc == 0)
			break; /* ready to destroy */

		D_DEBUG(DB_TRACE, "destroy context (idx %d, force %d), "
			"crt_context_abort failed rc: %d.\n",
			ctx->cc_idx, force, rc);

		if (i > 5)
			D_ERROR("destroy context (idx %d, force %d) "
				"takes too long time. This is attempt %d of %d.\n",
				ctx->cc_idx, force, i, CRT_SWIM_FLUSH_ATTEMPTS);
		/* Flush SWIM RPC already sent */
		rc = crt_context_flush(crt_ctx, timeout_sec);
		if (rc)
			/* give a chance to other threads to complete */
			usleep(1000); /* 1ms */
	}

	if (!force && rc && i == CRT_SWIM_FLUSH_ATTEMPTS)
		D_GOTO(out, rc);

	if (crt_gdata.cg_swim_inited && crt_gdata.cg_swim_ctx_idx == ctx_idx)
		crt_swim_fini();

	D_MUTEX_LOCK(&ctx->cc_mutex);

	rc = d_hash_table_destroy_inplace(&ctx->cc_epi_table, true /* force */);
	if (rc) {
		D_ERROR("destroy context (idx %d, force %d), "
			"d_hash_table_destroy_inplace failed, rc: %d.\n",
			ctx->cc_idx, force, rc);
		if (!force) {
			D_MUTEX_UNLOCK(&ctx->cc_mutex);
			D_GOTO(out, rc);
		}
	}

	d_binheap_destroy_inplace(&ctx->cc_bh_timeout);

	D_MUTEX_UNLOCK(&ctx->cc_mutex);

	provider = ctx->cc_hg_ctx.chc_provider;

	rc = crt_hg_ctx_fini(&ctx->cc_hg_ctx);
	if (rc) {
		D_ERROR("crt_hg_ctx_fini failed() rc: " DF_RC "\n", DP_RC(rc));

		if (!force)
			D_GOTO(out, rc);
	}

	D_RWLOCK_WRLOCK(&crt_gdata.cg_rwlock);

	crt_provider_put_ctx_idx(ctx->cc_primary, provider, ctx->cc_idx);
	d_list_del(&ctx->cc_link);

	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	D_MUTEX_DESTROY(&ctx->cc_mutex);
	D_DEBUG(DB_TRACE, "destroyed context (idx %d, force %d)\n", ctx->cc_idx, force);
	D_FREE(ctx);

out:
	D_RWLOCK_UNLOCK(&crt_context_destroy_lock);
	return rc;
}

int
crt_context_flush(crt_context_t crt_ctx, uint64_t timeout)
{
	uint64_t	ts_now = 0;
	uint64_t	ts_deadline = 0;
	int		rc = 0;

	if (timeout > 0)
		ts_deadline = d_timeus_secdiff(timeout);

	do {
		rc = crt_progress(crt_ctx, 1);
		if (rc != DER_SUCCESS && rc != -DER_TIMEDOUT) {
			D_ERROR("crt_progress() failed, rc: %d\n", rc);
			break;
		}
		if (crt_context_ep_empty(crt_ctx)) {
			rc = DER_SUCCESS;
			break;
		}
		if (timeout == 0)
			continue;
		ts_now = d_timeus_secdiff(0);
	} while (ts_now <= ts_deadline);

	if (timeout > 0 && ts_now >= ts_deadline)
		rc = -DER_TIMEDOUT;

	return rc;
}

/** May return -DER_BUSY if there is a concurrent crt_context destroy. */
int
crt_rank_abort(d_rank_t rank)
{
	struct crt_context	*ctx = NULL;
	struct crt_ep_inflight	*epi;
	d_list_t		*rlink;
	int			 flags;
	int			 rc = 0;
	d_list_t		*ctx_list;
	int			 ctx_num;
	struct d_vec_pointers	 ctxs;
	struct d_vec_pointers	 epis;
	int			 i;

	/*
	 * To avoid acquiring cg_rwlock, cc_mutex, and epi_mutex in wrong
	 * orders, we hold at most one of them at a time.
	 */

	/*
	 * Acquiring crt_context_destroy_lock ensures that the contexts in ctxs
	 * will not be destroyed. We don't block if the lock is busy, because a
	 * crt_context_destroy call may take the lock for quite some time.
	 */
	rc = D_RWLOCK_TRYWRLOCK(&crt_context_destroy_lock);
	if (rc != 0)
		D_GOTO(out, rc);

	D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);
	/* TODO: Do we need to handle secondary providers? */
	crt_provider_get_ctx_list_and_num(true, crt_gdata.cg_primary_prov, &ctx_list, &ctx_num);
	rc = d_vec_pointers_init(&ctxs, ctx_num);
	if (rc != 0) {
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_GOTO(out_crt_context_destroy_lock, rc);
	}
	d_list_for_each_entry(ctx, ctx_list, cc_link) {
		rc = d_vec_pointers_append(&ctxs, ctx);
		if (rc != 0) {
			D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
			D_GOTO(out_ctxs, rc);
		}
	}
	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	rc = d_vec_pointers_init(&epis, 16 /* cap */);
	if (rc != 0)
		D_GOTO(out_ctxs, rc);
	for (i = 0; i < ctxs.p_len; i++) {
		ctx = ctxs.p_buf[i];
		D_MUTEX_LOCK(&ctx->cc_mutex);
		rlink = d_hash_rec_find(&ctx->cc_epi_table, &rank, sizeof(rank));
		if (rlink != NULL) {
			rc = d_vec_pointers_append(&epis, epi_link2ptr(rlink));
			if (rc != 0) {
				d_hash_rec_decref(&ctx->cc_epi_table, rlink);
				D_MUTEX_UNLOCK(&ctx->cc_mutex);
				D_GOTO(out_epis, rc);
			}
		}
		D_MUTEX_UNLOCK(&ctx->cc_mutex);
	}

	for (i = 0; i < epis.p_len; i++) {
		epi = epis.p_buf[i];
		flags = CRT_EPI_ABORT_FORCE;
		rc = crt_ctx_epi_abort(epi, flags);
		if (rc != 0) {
			D_ERROR("context (idx %d), ep_abort (rank %d), failed rc: %d.\n",
				epi->epi_ctx->cc_idx, rank, rc);
			D_GOTO(out_epis, rc);
		}
	}

out_epis:
	for (i = 0; i < epis.p_len; i++) {
		epi = epis.p_buf[i];
		ctx = epi->epi_ctx;
		D_MUTEX_LOCK(&ctx->cc_mutex);
		d_hash_rec_decref(&ctx->cc_epi_table, &epi->epi_link);
		D_MUTEX_UNLOCK(&ctx->cc_mutex);
	}
	d_vec_pointers_fini(&epis);
out_ctxs:
	d_vec_pointers_fini(&ctxs);
out_crt_context_destroy_lock:
	D_RWLOCK_UNLOCK(&crt_context_destroy_lock);
out:
	return rc;
}

int
crt_ep_abort(crt_endpoint_t *ep) {
	return crt_rank_abort(ep->ep_rank);
}

int
crt_rank_abort_all(crt_group_t *grp)
{
	struct crt_grp_priv	*grp_priv;
	d_rank_list_t		*grp_membs;
	int			i;
	int			rc, rc2;

	grp_priv = crt_grp_pub2priv(grp);
	grp_membs = grp_priv_get_membs(grp_priv);
	rc2 = 0;

	if (grp_membs == NULL) {
		D_ERROR("No members in the group\n");
		D_GOTO(out, rc2 = -DER_INVAL);
	}

	D_RWLOCK_RDLOCK(&grp_priv->gp_rwlock);
	for (i = 0; i < grp_membs->rl_nr; i++) {
		D_DEBUG(DB_ALL, "Aborting RPCs to rank=%d\n",
			grp_membs->rl_ranks[i]);

		rc = crt_rank_abort(grp_membs->rl_ranks[i]);
		if (rc != DER_SUCCESS) {
			D_WARN("Abort to rank=%d failed with rc=%d\n",
			       grp_membs->rl_ranks[i], rc);
			rc2 = rc;
		}
	}
	D_RWLOCK_UNLOCK(&grp_priv->gp_rwlock);

out:
	return rc2;
}

/* caller should already hold crt_ctx->cc_mutex */
int
crt_req_timeout_track(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context *crt_ctx = rpc_priv->crp_pub.cr_ctx;
	int rc;

	D_ASSERT(crt_ctx != NULL);

	if (rpc_priv->crp_in_binheap == 1)
		D_GOTO(out, rc = 0);

	/* add to binheap for timeout tracking */
	RPC_ADDREF(rpc_priv); /* decref in crt_req_timeout_untrack */
	rc = d_binheap_insert(&crt_ctx->cc_bh_timeout,
			      &rpc_priv->crp_timeout_bp_node);
	if (rc == 0) {
		rpc_priv->crp_in_binheap = 1;
	} else {
		RPC_ERROR(rpc_priv, "d_binheap_insert failed, rc: %d\n", rc);
		RPC_DECREF(rpc_priv);
	}

out:
	return rc;
}

/* caller should already hold crt_ctx->cc_mutex */
void
crt_req_timeout_untrack(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context *crt_ctx = rpc_priv->crp_pub.cr_ctx;

	D_ASSERT(crt_ctx != NULL);

	/* remove from timeout binheap */
	if (rpc_priv->crp_in_binheap == 1) {
		rpc_priv->crp_in_binheap = 0;
		d_binheap_remove(&crt_ctx->cc_bh_timeout,
				 &rpc_priv->crp_timeout_bp_node);
		RPC_DECREF(rpc_priv); /* addref in crt_req_timeout_track */
	}
}

static bool
crt_req_timeout_reset(struct crt_rpc_priv *rpc_priv)
{
	struct crt_opc_info	*opc_info;
	struct crt_context	*crt_ctx;
	crt_endpoint_t		*tgt_ep;
	int			 rc;

	crt_ctx = rpc_priv->crp_pub.cr_ctx;
	opc_info = rpc_priv->crp_opc_info;
	D_ASSERT(opc_info != NULL);

	if (opc_info->coi_reset_timer == 0) {
		RPC_TRACE(DB_NET, rpc_priv, "reset_timer not enabled.\n");
		return false;
	}
	if (rpc_priv->crp_state == RPC_STATE_CANCELED ||
	    rpc_priv->crp_state == RPC_STATE_COMPLETED) {
		RPC_TRACE(DB_NET, rpc_priv, "state %#x, not resetting timer.\n",
			  rpc_priv->crp_state);
		return false;
	}

	tgt_ep = &rpc_priv->crp_pub.cr_ep;
	if (!CRT_RANK_PRESENT(tgt_ep->ep_grp, tgt_ep->ep_rank)) {
		RPC_TRACE(DB_NET, rpc_priv,
			"grp %p, rank %d already evicted.\n",
			tgt_ep->ep_grp, tgt_ep->ep_rank);
		return false;
	}

	tgt_ep = &rpc_priv->crp_pub.cr_ep;

	RPC_TRACE(DB_NET, rpc_priv, "reset_timer enabled.\n");

	crt_set_timeout(rpc_priv);
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	rc = crt_req_timeout_track(rpc_priv);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
	if (rc != 0) {
		RPC_ERROR(rpc_priv,
			"crt_req_timeout_track(opc: %#x) failed, rc: %d.\n",
			rpc_priv->crp_pub.cr_opc, rc);
		return false;
	}

	return true;
}

static void
crt_req_timeout_hdlr(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context		*crt_ctx;
	struct crt_grp_priv		*grp_priv;
	crt_endpoint_t			*tgt_ep;
	crt_rpc_t			*ul_req;
	struct crt_uri_lookup_in	*ul_in;
	int				 rc;

	crt_rpc_lock(rpc_priv);

	if (crt_req_timeout_reset(rpc_priv)) {
		crt_rpc_unlock(rpc_priv);
		RPC_TRACE(DB_NET, rpc_priv, "reached timeout. Renewed for another cycle.\n");
		return;
	};

	tgt_ep = &rpc_priv->crp_pub.cr_ep;
	grp_priv = crt_grp_pub2priv(tgt_ep->ep_grp);
	crt_ctx = rpc_priv->crp_pub.cr_ctx;

	if (crt_gdata.cg_use_sensors)
		d_tm_inc_counter(crt_ctx->cc_timedout, 1);

	switch (rpc_priv->crp_state) {
	case RPC_STATE_INITED:
	case RPC_STATE_QUEUED:
		RPC_INFO(rpc_priv, "aborting %s rpc to group %s, tgt %d:%d, tgt_uri %s\n",
			 rpc_priv->crp_state == RPC_STATE_QUEUED ? "queued" : "inited",
			 grp_priv->gp_pub.cg_grpid, tgt_ep->ep_rank, tgt_ep->ep_tag,
			 rpc_priv->crp_tgt_uri);
		crt_context_req_untrack(rpc_priv);
		crt_rpc_complete_and_unlock(rpc_priv, -DER_TIMEDOUT);
		break;
	case RPC_STATE_URI_LOOKUP:
		ul_req = rpc_priv->crp_ul_req;
		D_ASSERT(ul_req != NULL);
		ul_in = crt_req_get(ul_req);
		RPC_INFO(rpc_priv,
			 "failed due to URI_LOOKUP(rpc_priv %p) to group %s,"
			 "rank %d through PSR %d timedout\n",
			 container_of(ul_req, struct crt_rpc_priv, crp_pub), ul_in->ul_grp_id,
			 ul_in->ul_rank, ul_req->cr_ep.ep_rank);

		if (crt_gdata.cg_use_sensors)
			d_tm_inc_counter(crt_ctx->cc_timedout_uri, 1);
		crt_req_abort(ul_req);
		/*
		 * don't crt_rpc_complete_and_unlock rpc_priv here, because crt_req_abort
		 * above will lead to ul_req's completion callback --
		 * crt_req_uri_lookup_by_rpc_cb() be called inside there will
		 * complete this rpc_priv.
		 */
		/* crt_rpc_complete_and_unlock(rpc_priv, -DER_PROTO); */
		crt_rpc_unlock(rpc_priv);
		break;
	case RPC_STATE_FWD_UNREACH:
		RPC_INFO(rpc_priv,
			 "failed due to group %s, rank %d, tgt_uri %s can't reach the target\n",
			 grp_priv->gp_pub.cg_grpid, tgt_ep->ep_rank, rpc_priv->crp_tgt_uri);
		crt_context_req_untrack(rpc_priv);
		crt_rpc_complete_and_unlock(rpc_priv, -DER_UNREACH);
		break;
	case RPC_STATE_REQ_SENT:
		/* At this point, RPC should always be completed by
		 * Mercury
		 */
		RPC_INFO(rpc_priv, "aborting in-flight to group %s, rank %d, tgt_uri %s\n",
			 grp_priv->gp_pub.cg_grpid, tgt_ep->ep_rank, rpc_priv->crp_tgt_uri);
		rc = crt_hg_req_cancel(rpc_priv);
		if (rc != 0) {
			RPC_WARN(rpc_priv,
				 "crt_hg_req_cancel failed, rc: %d, "
				 "opc: %#x.\n",
				 rc, rpc_priv->crp_pub.cr_opc);
			crt_context_req_untrack(rpc_priv);
		}
		crt_rpc_unlock(rpc_priv);
		break;
	default:
		RPC_TRACE(DB_NET, rpc_priv, "nothing to do: state=%d\n", rpc_priv->crp_state);
		crt_rpc_unlock(rpc_priv);
		break;
	}
}

static void
crt_context_timeout_check(struct crt_context *crt_ctx)
{
	struct crt_rpc_priv		*rpc_priv;
	struct d_binheap_node		*bh_node;
	d_list_t			 timeout_list;
	uint64_t			 ts_now;
	bool                             print_once = false;
#ifdef HG_HAS_DIAG
	bool should_republish = false;
#endif

	D_ASSERT(crt_ctx != NULL);

	D_INIT_LIST_HEAD(&timeout_list);
	ts_now = d_timeus_secdiff(0);

	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	while (1) {
		bh_node = d_binheap_root(&crt_ctx->cc_bh_timeout);
		if (bh_node == NULL)
			break;
		rpc_priv = container_of(bh_node, struct crt_rpc_priv,
					crp_timeout_bp_node);
		if (rpc_priv->crp_timeout_ts > ts_now)
			break;

		/* +1 to prevent it from being released in timeout_untrack */
		RPC_ADDREF(rpc_priv);
		crt_req_timeout_untrack(rpc_priv);
		rpc_priv->crp_timeout_ts = 0;

		D_ASSERTF(d_list_empty(&rpc_priv->crp_tmp_link_timeout),
			  "already on timeout list\n");
		d_list_add_tail(&rpc_priv->crp_tmp_link_timeout, &timeout_list);
	}

#ifdef HG_HAS_DIAG
	/* piggy-back on the timeout processing so that we don't need to do another gettime() */
	if (ts_now - crt_ctx->cc_hg_ctx.chc_diag_pub_ts > CRT_HG_TM_PUB_INTERVAL_US) {
		should_republish                   = true;
		crt_ctx->cc_hg_ctx.chc_diag_pub_ts = ts_now;
	}
#endif
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	/* handle the timeout RPCs */
	while ((rpc_priv =
		    d_list_pop_entry(&timeout_list, struct crt_rpc_priv, crp_tmp_link_timeout))) {
		/* NB: The reason that the error message is printed at INFO
		 * level is because the user should know how serious the error
		 * is and they will print the RPC error at appropriate level */
		if (!print_once) {
			print_once = true;

			RPC_WARN(rpc_priv,
				 "ctx_id %d, (status: %#x) timed out (%d seconds), "
				 "target (%d:%d)\n",
				 crt_ctx->cc_idx, rpc_priv->crp_state, rpc_priv->crp_timeout_sec,
				 rpc_priv->crp_pub.cr_ep.ep_rank, rpc_priv->crp_pub.cr_ep.ep_tag);
		} else {
			RPC_INFO(rpc_priv,
				 "ctx_id %d, (status: %#x) timed out (%d seconds), "
				 "target (%d:%d)\n",
				 crt_ctx->cc_idx, rpc_priv->crp_state, rpc_priv->crp_timeout_sec,
				 rpc_priv->crp_pub.cr_ep.ep_rank, rpc_priv->crp_pub.cr_ep.ep_tag);
		}

		crt_req_timeout_hdlr(rpc_priv);
		RPC_DECREF(rpc_priv);
	}

#ifdef HG_HAS_DIAG
	/* periodically republish Mercury-level counters as DAOS metrics */
	if (should_republish)
		crt_hg_republish_diags(&crt_ctx->cc_hg_ctx);
#endif
}

/*
 * Track the rpc request per context
 * return CRT_REQ_TRACK_IN_INFLIGHQ - tacked in crt_ep_inflight::epi_req_q
 *        CRT_REQ_TRACK_IN_WAITQ    - queued in crt_ep_inflight::epi_req_waitq
 *        negative value            - other error case such as -DER_NOMEM
 */
int
crt_context_req_track(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx = rpc_priv->crp_pub.cr_ctx;
	struct crt_ep_inflight	*epi = NULL;
	d_list_t		*rlink;
	d_rank_t		 ep_rank;
	int			 rc = 0;
	int 			quota_rc = 0;
	struct crt_grp_priv	*grp_priv;

	D_ASSERT(crt_ctx != NULL);

	if (rpc_priv->crp_pub.cr_opc == CRT_OPC_URI_LOOKUP) {
		RPC_TRACE(DB_NET, rpc_priv,
			  "bypass tracking for URI_LOOKUP.\n");
		D_GOTO(out, rc = CRT_REQ_TRACK_IN_INFLIGHQ);
	}

	/* check inflight quota. if exceeded, queue this rpc */
	quota_rc = get_quota_resource(rpc_priv->crp_pub.cr_ctx, CRT_QUOTA_RPCS);

	grp_priv = crt_grp_pub2priv(rpc_priv->crp_pub.cr_ep.ep_grp);
	ep_rank = crt_grp_priv_get_primary_rank(grp_priv,
				rpc_priv->crp_pub.cr_ep.ep_rank);

	/* lookup the crt_ep_inflight (create one if not found) */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	rlink = d_hash_rec_find(&crt_ctx->cc_epi_table, (void *)&ep_rank,
				sizeof(ep_rank));
	if (rlink == NULL) {
		D_ALLOC_PTR(epi);
		if (epi == NULL)
			D_GOTO(out_unlock, rc = -DER_NOMEM);

		/* init the epi fields */
		D_INIT_LIST_HEAD(&epi->epi_link);
		epi->epi_ep.ep_rank = ep_rank;
		epi->epi_ctx = crt_ctx;
		D_INIT_LIST_HEAD(&epi->epi_req_q);
		epi->epi_req_num = 0;
		epi->epi_reply_num = 0;
		D_INIT_LIST_HEAD(&epi->epi_req_waitq);
		epi->epi_req_wait_num = 0;
		/* epi_ref init as 1 to avoid other thread delete it but here
		 * still need to access it, decref before exit this routine. */
		epi->epi_ref = 1;
		epi->epi_initialized = 1;
		rc = D_MUTEX_INIT(&epi->epi_mutex, NULL);
		if (rc != 0)
			D_GOTO(out_unlock, rc);

		rc = d_hash_rec_insert(&crt_ctx->cc_epi_table, &ep_rank,
				       sizeof(ep_rank), &epi->epi_link,
				       true /* exclusive */);
		if (rc != 0) {
			D_ERROR("d_hash_rec_insert failed, rc: %d.\n", rc);
			D_MUTEX_DESTROY(&epi->epi_mutex);
			D_GOTO(out_unlock, rc);
		}
	} else {
		epi = epi_link2ptr(rlink);
		D_ASSERT(epi->epi_ctx == crt_ctx);
	}
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	/* add the RPC req to crt_ep_inflight */
	D_MUTEX_LOCK(&epi->epi_mutex);
	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);
	crt_set_timeout(rpc_priv);
	rpc_priv->crp_epi = epi;
	RPC_ADDREF(rpc_priv);

	if (quota_rc == -DER_QUOTA_LIMIT) {
		epi->epi_req_num++;
		rpc_priv->crp_state = RPC_STATE_QUEUED;
		rc = CRT_REQ_TRACK_IN_WAITQ;
	} else if (crt_gdata.cg_credit_ep_ctx != 0 &&
	    (epi->epi_req_num - epi->epi_reply_num) >= crt_gdata.cg_credit_ep_ctx) {
		if (rpc_priv->crp_opc_info->coi_queue_front)
			d_list_add(&rpc_priv->crp_epi_link, &epi->epi_req_waitq);
		else
			d_list_add_tail(&rpc_priv->crp_epi_link, &epi->epi_req_waitq);

		epi->epi_req_wait_num++;
		rpc_priv->crp_state = RPC_STATE_QUEUED;
		rc = CRT_REQ_TRACK_IN_WAITQ;
	} else {
		D_MUTEX_LOCK(&crt_ctx->cc_mutex);
		rc = crt_req_timeout_track(rpc_priv);
		D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
		if (rc == 0) {
			d_list_add_tail(&rpc_priv->crp_epi_link, &epi->epi_req_q);
			epi->epi_req_num++;
			rc = CRT_REQ_TRACK_IN_INFLIGHQ;
		} else {
			RPC_ERROR(rpc_priv, "crt_req_timeout_track failed, rc: %d.\n", rc);
			/* roll back the addref above */
			RPC_DECREF(rpc_priv);
		}
	}

	rpc_priv->crp_ctx_tracked = 1;
	D_MUTEX_UNLOCK(&epi->epi_mutex);

	/* reference taken by d_hash_rec_find or "epi->epi_ref = 1" above */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	d_hash_rec_decref(&crt_ctx->cc_epi_table, &epi->epi_link);

	if (quota_rc == -DER_QUOTA_LIMIT) {
		d_list_add_tail(&rpc_priv->crp_waitq_link, &crt_ctx->cc_quotas.rpc_waitq);
		d_tm_inc_gauge(crt_ctx->cc_quotas.rpc_waitq_depth, 1);
	}

	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

out:
	return rc;

out_unlock:
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
	D_FREE(epi);
	return rc;
}

static inline int64_t
credits_available(struct crt_ep_inflight *epi)
{
	int64_t inflight = epi->epi_req_num - epi->epi_reply_num;

	/* TODO: inflight right now includes items queued in quota waitq, and can exceed credit limit */
	if (inflight > crt_gdata.cg_credit_ep_ctx)
		return 0;

	return crt_gdata.cg_credit_ep_ctx - inflight;
}

/* Not to be called on URI_LOOKUP RPCs. */
static void
crt_context_req_untrack_internal(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx = rpc_priv->crp_pub.cr_ctx;
	struct crt_ep_inflight	*epi = rpc_priv->crp_epi;

	D_ASSERT(crt_ctx != NULL);
	D_ASSERT(epi != NULL);
	D_ASSERT(rpc_priv->crp_state == RPC_STATE_INITED ||
		 rpc_priv->crp_state == RPC_STATE_QUEUED ||
		 rpc_priv->crp_state == RPC_STATE_COMPLETED ||
		 rpc_priv->crp_state == RPC_STATE_TIMEOUT ||
		 rpc_priv->crp_state == RPC_STATE_URI_LOOKUP ||
		 rpc_priv->crp_state == RPC_STATE_CANCELED ||
		 rpc_priv->crp_state == RPC_STATE_FWD_UNREACH);

	D_MUTEX_LOCK(&epi->epi_mutex);

	/* Prevent simultaneous untrack from progress thread and
	 * main rpc execution thread.
	 */
	if (rpc_priv->crp_ctx_tracked == 0) {
		RPC_TRACE(DB_NET, rpc_priv,
			"rpc is not tracked already.\n");
		D_MUTEX_UNLOCK(&epi->epi_mutex);
		return;
	}

	/* remove from wait or in-flight queue */
	d_list_del_init(&rpc_priv->crp_epi_link);
	if (rpc_priv->crp_state == RPC_STATE_COMPLETED) {
		epi->epi_reply_num++;
	} else if (rpc_priv->crp_state == RPC_STATE_QUEUED) {
		epi->epi_req_wait_num--;
	} else {/* RPC_CANCELED or RPC_INITED or RPC_TIMEOUT */
		epi->epi_req_num--;
	}

	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);

	D_MUTEX_UNLOCK(&epi->epi_mutex);

	if (!crt_req_timedout(rpc_priv)) {
		D_MUTEX_LOCK(&crt_ctx->cc_mutex);
		crt_req_timeout_untrack(rpc_priv);
		D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
	}

	rpc_priv->crp_ctx_tracked = 0;

	/* decref corresponding to addref in crt_context_req_track */
	RPC_DECREF(rpc_priv);
}

static void
add_rpc_to_list(struct crt_rpc_priv *rpc_priv, d_list_t *submit_list)
{
	struct crt_context     *crt_ctx = rpc_priv->crp_pub.cr_ctx;
	struct crt_ep_inflight *epi     = rpc_priv->crp_epi;

	D_ASSERT(epi != NULL);

	RPC_ADDREF(rpc_priv);

	crt_rpc_lock(rpc_priv);
	D_MUTEX_LOCK(&epi->epi_mutex);
	if (rpc_priv->crp_state == RPC_STATE_QUEUED) {
		bool submit_rpc = true;
		int  rc;

		rpc_priv->crp_state = RPC_STATE_INITED;
		/* RPC got cancelled or timed out before it got here */
		if (rpc_priv->crp_timeout_ts == 0) {
			submit_rpc = false;
		} else {
			crt_set_timeout(rpc_priv);

			D_MUTEX_LOCK(&crt_ctx->cc_mutex);
			rc = crt_req_timeout_track(rpc_priv);
			D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
			if (rc != 0)
				RPC_ERROR(rpc_priv, "crt_req_timeout_track failed, rc: %d.\n", rc);
		}

		d_list_move_tail(&rpc_priv->crp_epi_link, &epi->epi_req_q);
		/* add to submit list if not cancelled or timed out already  */
		if (submit_rpc) {
			/* prevent rpc from being released before it is dispatched below */
			RPC_ADDREF(rpc_priv);

			D_ASSERTF(d_list_empty(&rpc_priv->crp_tmp_link_submit),
				  "already on submit list\n");
			d_list_add_tail(&rpc_priv->crp_tmp_link_submit, submit_list);
		}
	}
	D_MUTEX_UNLOCK(&epi->epi_mutex);
	crt_rpc_unlock(rpc_priv);
	RPC_DECREF(rpc_priv);
}

static void
dispatch_rpc(struct crt_rpc_priv *rpc) {
	int rc;

	D_ASSERTF(rpc != NULL, "rpc is NULL\n");

	crt_rpc_lock(rpc);

	/* RPC got cancelled or timed out before it got here, it got already completed*/
	if (rpc->crp_timeout_ts == 0) {
		crt_rpc_unlock(rpc);
		return;
	}

	rc = crt_req_send_internal(rpc);
	if (rc == 0) {
		crt_rpc_unlock(rpc);
	} else {
		RPC_ADDREF(rpc);
		RPC_ERROR(rpc, "crt_req_send_internal failed, rc: %d\n", rc);
		rpc->crp_state = RPC_STATE_INITED;
		crt_context_req_untrack_internal(rpc);
		/* for error case here */
		crt_rpc_complete_and_unlock(rpc, rc);
	}
}

void
crt_context_req_untrack(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx = rpc_priv->crp_pub.cr_ctx;
	struct crt_ep_inflight	*epi;
	d_list_t		 submit_list;
	struct crt_rpc_priv     *tmp_rpc;

	D_ASSERT(crt_ctx != NULL);

	if (rpc_priv->crp_pub.cr_opc == CRT_OPC_URI_LOOKUP)
		return;

	epi = rpc_priv->crp_epi;
	D_ASSERT(epi != NULL);

	/* Dispatch one rpc from wait_q if any or return resource back */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	tmp_rpc = d_list_pop_entry(&crt_ctx->cc_quotas.rpc_waitq,
				   struct crt_rpc_priv, crp_waitq_link);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	D_INIT_LIST_HEAD(&submit_list);
	if (tmp_rpc != NULL) {
		add_rpc_to_list(tmp_rpc, &submit_list);
		d_tm_dec_gauge(crt_ctx->cc_quotas.rpc_waitq_depth, 1);
	} else {
		put_quota_resource(rpc_priv->crp_pub.cr_ctx, CRT_QUOTA_RPCS);
	}

	crt_context_req_untrack_internal(rpc_priv);

	/* done if ep credit flow control is disabled */
	if (crt_gdata.cg_credit_ep_ctx == 0)
		goto out;

	/* process waitq */
	D_MUTEX_LOCK(&epi->epi_mutex);
	while (credits_available(epi) > 0 && !d_list_empty(&epi->epi_req_waitq)) {
		tmp_rpc = d_list_pop_entry(&epi->epi_req_waitq, struct crt_rpc_priv, crp_epi_link);
		epi->epi_req_wait_num--;
		D_ASSERTF(epi->epi_req_wait_num >= 0, "wait %jd\n", epi->epi_req_wait_num);
		/* remove from waitq and add to in-flight queue */
		epi->epi_req_num++;
		D_ASSERTF(epi->epi_req_num >= epi->epi_reply_num, "req %jd reply %jd\n",
			  epi->epi_req_num, epi->epi_reply_num);
		D_MUTEX_UNLOCK(&epi->epi_mutex);
		add_rpc_to_list(tmp_rpc, &submit_list);
		D_MUTEX_LOCK(&epi->epi_mutex);
	}

	D_MUTEX_UNLOCK(&epi->epi_mutex);

out:
	/* re-submit the rpc req */
	while (
	    (tmp_rpc = d_list_pop_entry(&submit_list, struct crt_rpc_priv, crp_tmp_link_submit))) {
		dispatch_rpc(tmp_rpc);
		/* addref done above during d_list_add_tail */
		RPC_DECREF(tmp_rpc);
	}
}

/* TODO: Need per-provider call */
crt_context_t
crt_context_lookup_locked(int ctx_idx)
{
	struct crt_context	*ctx;
	d_list_t		*ctx_list;
	int			i;

	ctx_list = crt_provider_get_ctx_list(true, crt_gdata.cg_primary_prov);

	d_list_for_each_entry(ctx, ctx_list, cc_link) {
		if (ctx->cc_idx == ctx_idx)
			return ctx;
	}

	for (i = 0; i < crt_gdata.cg_num_secondary_provs; i++) {
		ctx_list = crt_provider_get_ctx_list(false, crt_gdata.cg_secondary_provs[i]);

		d_list_for_each_entry(ctx, ctx_list, cc_link) {
			if (ctx->cc_idx == ctx_idx) {
				return ctx;
			}
		}
	}
	return NULL;
}

crt_context_t
crt_context_lookup(int ctx_idx)
{
	struct crt_context	*ctx;
	bool			found = false;
	int			i;
	d_list_t		*ctx_list;

	D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);

	ctx_list = crt_provider_get_ctx_list(true, crt_gdata.cg_primary_prov);

	d_list_for_each_entry(ctx, ctx_list, cc_link) {
		if (ctx->cc_idx == ctx_idx) {
			found = true;
			D_GOTO(unlock, 0);
		}
	}

	for (i = 0; i < crt_gdata.cg_num_secondary_provs; i++) {
		ctx_list = crt_provider_get_ctx_list(false, crt_gdata.cg_secondary_provs[i]);

		d_list_for_each_entry(ctx, ctx_list, cc_link) {
			if (ctx->cc_idx == ctx_idx) {
				found = true;
				break;
			}
		}
	}

unlock:
	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);


	return (found == true) ? ctx : NULL;
}

int
crt_context_idx(crt_context_t crt_ctx, int *ctx_idx)
{
	struct crt_context	*ctx;
	int			rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL || ctx_idx == NULL) {
		D_ERROR("invalid parameter, crt_ctx: %p, ctx_idx: %p.\n",
			crt_ctx, ctx_idx);
		D_GOTO(out, rc = -DER_INVAL);
	}

	ctx = crt_ctx;
	*ctx_idx = ctx->cc_idx;

out:
	return rc;
}

int
crt_get_nr_secondary_providers(void)
{
	return crt_gdata.cg_num_secondary_provs;
}

int
crt_self_uri_get_secondary(int secondary_idx, char **uri)
{
	char *addr;

	if (secondary_idx != 0) {
		D_ERROR("Only index=0 supported for now\n");
		return -DER_NONEXIST;
	}

	if ((crt_gdata.cg_prov_gdata_secondary == NULL) ||
	    (secondary_idx >= crt_gdata.cg_num_secondary_provs)) {
		return -DER_NONEXIST;
	}

	addr = crt_gdata.cg_prov_gdata_secondary[secondary_idx].cpg_addr;

	D_STRNDUP(*uri, addr, CRT_ADDR_STR_MAX_LEN - 1);

	if (!*uri)
		return -DER_NOMEM;

	return DER_SUCCESS;
}

int
crt_self_uri_get(int tag, char **uri)
{
	struct crt_context	*tmp_crt_ctx;
	char			*tmp_uri = NULL;
	int			 rc = 0;

	if (uri == NULL) {
		D_ERROR("uri can't be NULL.\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	tmp_crt_ctx = crt_context_lookup(tag);
	if (tmp_crt_ctx == NULL) {
		D_ERROR("crt_context_lookup(%d) failed.\n", tag);
		D_GOTO(out, rc = -DER_NONEXIST);
	}

	D_STRNDUP(tmp_uri, tmp_crt_ctx->cc_self_uri, CRT_ADDR_STR_MAX_LEN - 1);
	*uri = tmp_uri;

out:
	return rc;
}

int
crt_context_num(int *ctx_num)
{
	if (ctx_num == NULL) {
		D_ERROR("invalid parameter of NULL ctx_num.\n");
		return -DER_INVAL;
	}

	*ctx_num = crt_gdata.cg_prov_gdata_primary.cpg_ctx_num;
	return 0;
}

bool
crt_context_empty(crt_provider_t provider, int locked)
{
	bool rc = false;

	if (locked == 0)
		D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);

	rc = d_list_empty(&crt_gdata.cg_prov_gdata_primary.cpg_ctx_list);

	if (locked == 0)
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	return rc;
}

int
crt_progress_cond(crt_context_t crt_ctx, int64_t timeout,
		  crt_progress_cond_cb_t cond_cb, void *arg)
{
	struct crt_context	*ctx;
	int64_t			 hg_timeout;
	uint64_t		 now;
	uint64_t		 end = 0;
	int			 rc = 0;

	/** validate input parameters */
	if (unlikely(crt_ctx == CRT_CONTEXT_NULL || cond_cb == NULL)) {
		D_ERROR("invalid parameter (%p)\n", cond_cb);
		return -DER_INVAL;
	}

	/**
	 * Invoke the callback once first, in case the condition is met before
	 * calling progress
	 */
	rc = cond_cb(arg);
	if (rc > 0)
		/** exit as per the callback request */
		return 0;
	if (unlikely(rc < 0))
		/** something wrong happened during the callback execution */
		return rc;

	ctx = crt_ctx;

	/** Progress with callback and non-null timeout */
	if (timeout > 0) {
		now = d_timeus_secdiff(0);
		end = now + timeout;
	}

	/**
	 * Call progress once before processing timeouts in case
	 * any replies are pending in the queue
	 */
	rc = crt_hg_progress(&ctx->cc_hg_ctx, 0);
	if (unlikely(rc && rc != -DER_TIMEDOUT)) {
		D_ERROR("crt_hg_progress failed with %d\n", rc);
		return rc;
	}

	/** loop until callback returns non-null value */
	while ((rc = cond_cb(arg)) == 0) {
		crt_context_timeout_check(ctx);
		if (ctx->cc_prog_cb != NULL)
			timeout = ctx->cc_prog_cb(ctx, timeout, ctx->cc_prog_cb_arg);

		if (timeout < 0) {
			/**
			 * For infinite timeout, use a mercury timeout of 1 ms to avoid
			 * being blocked indefinitely if another thread has called
			 * crt_hg_progress() behind our back
			 */
			hg_timeout = 1000;
		} else if (timeout == 0) {
			hg_timeout = 0;
		} else { /** timeout > 0 */
			/** similarly, probe more frequently if timeout is large */
			if (timeout > 1000 * 1000)
				hg_timeout = 1000 * 1000;
			else
				hg_timeout = timeout;
		}

		rc = crt_hg_progress(&ctx->cc_hg_ctx, hg_timeout);
		if (unlikely(rc && rc != -DER_TIMEDOUT)) {
			D_ERROR("crt_hg_progress failed with %d\n", rc);
			return rc;
		}

		/** check for timeout */
		if (timeout < 0)
			continue;

		now = d_timeus_secdiff(0);
		if (timeout == 0 || now >= end) {
			/** try callback one last time just in case */
			rc = cond_cb(arg);
			if (unlikely(rc != 0))
				break;
			return -DER_TIMEDOUT;
		}
	}

	if (rc > 0)
		rc = 0;

	return rc;
}

int
crt_progress(crt_context_t crt_ctx, int64_t timeout)
{
	struct crt_context	*ctx;
	int			 rc = 0;

	/** validate input parameters */
	if (unlikely(crt_ctx == CRT_CONTEXT_NULL)) {
		D_ERROR("invalid parameter (NULL crt_ctx).\n");
		return -DER_INVAL;
	}

	ctx = crt_ctx;

	/**
	 * call progress once w/o any timeout before processing timed out
	 * requests in case any replies are pending in the queue
	 */
	rc = crt_hg_progress(&ctx->cc_hg_ctx, 0);
	if (unlikely(rc && rc != -DER_TIMEDOUT))
		D_ERROR("crt_hg_progress failed, rc: %d.\n", rc);

	/**
	 * process timeout and progress callback after this initial call to
	 * progress
	 */
	crt_context_timeout_check(ctx);
	if (ctx->cc_prog_cb != NULL)
		timeout = ctx->cc_prog_cb(ctx, timeout, ctx->cc_prog_cb_arg);

	if (timeout != 0 && (rc == 0 || rc == -DER_TIMEDOUT)) {
		/** call progress once again with the real timeout */
		rc = crt_hg_progress(&ctx->cc_hg_ctx, timeout);
		if (unlikely(rc && rc != -DER_TIMEDOUT))
			D_ERROR("crt_hg_progress failed, rc: %d.\n", rc);
	}

	return rc;
}

/**
 * to use this function, the user has to:
 * 1) define a callback function user_cb
 * 2) call crt_register_progress_cb(user_cb);
 */
int
crt_register_progress_cb(crt_progress_cb func, int ctx_idx, void *args)
{
	struct crt_context *ctx;
	int                 rc;

	if (ctx_idx >= CRT_SRV_CONTEXT_NUM) {
		D_ERROR("ctx_idx %d >= %d\n", ctx_idx, CRT_SRV_CONTEXT_NUM);
		D_GOTO(error, rc = -DER_INVAL);
	}

	ctx = crt_context_lookup(ctx_idx);
	if (ctx == NULL) {
		D_ERROR("crt_context_lookup(%d) failed.\n", ctx_idx);
		D_GOTO(error, rc = -DER_NONEXIST);
	}

	D_MUTEX_LOCK(&ctx->cc_mutex);
	ctx->cc_prog_cb     = func;
	ctx->cc_prog_cb_arg = args;
	D_MUTEX_UNLOCK(&ctx->cc_mutex);

	return 0;

error:
	return rc;
}

int
crt_unregister_progress_cb(crt_progress_cb func, int ctx_idx, void *args)
{
	(void)func;
	(void)args;

	return crt_register_progress_cb(NULL, ctx_idx, NULL);
}

int
crt_context_set_timeout(crt_context_t crt_ctx, uint32_t timeout_sec)
{
	struct crt_context	*ctx;
	int			rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("NULL context passed\n");
		D_GOTO(exit, rc = -DER_INVAL);
	}

	if (timeout_sec == 0) {
		D_ERROR("Invalid value 0 for timeout specified\n");
		D_GOTO(exit, rc = -DER_INVAL);
	}

	ctx = crt_ctx;
	ctx->cc_timeout_sec = timeout_sec;

exit:
	return rc;
}

int
crt_context_get_timeout(crt_context_t crt_ctx, uint32_t *timeout_sec)
{
	struct crt_context	*ctx = crt_ctx;
	int			 rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("NULL context passed\n");
		rc = -DER_INVAL;
	} else if (ctx->cc_timeout_sec != 0) {
		*timeout_sec = ctx->cc_timeout_sec;
	} else {
		*timeout_sec = crt_gdata.cg_timeout;
	}

	return rc;
}

/* Force complete the rpc. Used for handling of unreachable rpcs */
void
crt_req_force_completion(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx;

	RPC_TRACE(DB_TRACE, rpc_priv, "Force completing rpc\n");

	if (rpc_priv == NULL) {
		D_ERROR("Invalid argument, rpc_priv == NULL\n");
		return;
	}

	if (rpc_priv->crp_pub.cr_opc == CRT_OPC_URI_LOOKUP) {
		RPC_TRACE(DB_TRACE, rpc_priv, "Skipping for opcode: %#x\n",
			  CRT_OPC_URI_LOOKUP);
		return;
	}

	/* Handle unreachable rpcs similarly to timed out rpcs */
	crt_ctx = rpc_priv->crp_pub.cr_ctx;

	/**
	 *  set the RPC's expiration time stamp to the past, move it to the top
	 *  of the heap.
	 */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	crt_req_timeout_untrack(rpc_priv);
	rpc_priv->crp_timeout_ts = 0;
	crt_req_timeout_track(rpc_priv);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
}

static void
context_quotas_init(struct crt_context *ctx)
{
	struct crt_quotas *quotas;

	quotas = &ctx->cc_quotas;

	quotas->limit[CRT_QUOTA_RPCS]   = crt_gdata.cg_rpc_quota;
	quotas->current[CRT_QUOTA_RPCS] = 0;
	quotas->enabled[CRT_QUOTA_RPCS] = crt_gdata.cg_rpc_quota > 0 ? true : false;

	quotas->limit[CRT_QUOTA_BULKS]   = crt_gdata.cg_bulk_quota;
	quotas->current[CRT_QUOTA_BULKS] = 0;
	quotas->enabled[CRT_QUOTA_BULKS] = crt_gdata.cg_bulk_quota > 0 ? true : false;
}

static void
context_quotas_finalize(struct crt_context *ctx)
{
	for (int i = 0; i < CRT_QUOTA_COUNT; i++)
		ctx->cc_quotas.enabled[i] = false;
}

int
crt_context_quota_limit_set(crt_context_t crt_ctx, crt_quota_type_t quota, int value)
{
	struct crt_context	*ctx = crt_ctx;
	int			rc = 0;

	if (ctx == NULL) {
		D_ERROR("NULL context\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	if (quota < 0 || quota >= CRT_QUOTA_COUNT) {
		D_ERROR("Invalid quota %d passed\n", quota);
		D_GOTO(out, rc = -DER_INVAL);
	}

	D_MUTEX_LOCK(&ctx->cc_quotas.mutex);
	ctx->cc_quotas.limit[quota] = value;
	D_MUTEX_UNLOCK(&ctx->cc_quotas.mutex);

out:
	return rc;
}

int
crt_context_quota_limit_get(crt_context_t crt_ctx, crt_quota_type_t quota, int *value)
{
	struct crt_context	*ctx = crt_ctx;
	int			rc = 0;

	if (ctx == NULL) {
		D_ERROR("NULL context\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	if (quota < 0 || quota >= CRT_QUOTA_COUNT) {
		D_ERROR("Invalid quota %d passed\n", quota);
		D_GOTO(out, rc = -DER_INVAL);
	}

	if (value == NULL) {
		D_ERROR("NULL value\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	*value = ctx->cc_quotas.limit[quota];

out:
	return rc;
}

/* bump tracked usage of the resource by 1 without checking for limits  */
void
record_quota_resource(crt_context_t crt_ctx, crt_quota_type_t quota)
{
	struct crt_context *ctx = crt_ctx;

	D_ASSERTF(ctx != NULL, "NULL context\n");
	D_ASSERTF(quota >= 0 && quota < CRT_QUOTA_COUNT, "Invalid quota\n");

	/* If quotas not enabled or unlimited quota */
	if (!ctx->cc_quotas.enabled[quota] || ctx->cc_quotas.limit[quota] == 0)
		return;

	atomic_fetch_add(&ctx->cc_quotas.current[quota], 1);
}

/* returns 0 if resource is available or -DER_QUOTA_LIMIT otherwise */
int
get_quota_resource(crt_context_t crt_ctx, crt_quota_type_t quota)
{
	struct crt_context	*ctx = crt_ctx;
	int			rc = 0;

	D_ASSERTF(ctx != NULL, "NULL context\n");
	D_ASSERTF(quota >= 0 && quota < CRT_QUOTA_COUNT, "Invalid quota\n");

	/* If quotas not enabled or unlimited quota */
	if (!ctx->cc_quotas.enabled[quota] || ctx->cc_quotas.limit[quota] == 0)
		return 0;

	/* It's ok if we go slightly above quota in a corner case, but avoid locks */
	if (ctx->cc_quotas.current[quota] < ctx->cc_quotas.limit[quota]) {
		atomic_fetch_add(&ctx->cc_quotas.current[quota], 1);
	} else {
		D_DEBUG(DB_TRACE, "Quota limit (%d) reached for quota_type=%d\n",
			ctx->cc_quotas.limit[quota], quota);
		d_tm_inc_counter(ctx->cc_quotas.rpc_quota_exceeded, 1);
		rc = -DER_QUOTA_LIMIT;
	}

	return rc;
}

/* return resource back */
void
put_quota_resource(crt_context_t crt_ctx, crt_quota_type_t quota)
{
	struct crt_context	*ctx = crt_ctx;

	D_ASSERTF(ctx != NULL, "NULL context\n");
	D_ASSERTF(quota >= 0 && quota < CRT_QUOTA_COUNT, "Invalid quota\n");

	/* If quotas not enabled or unlimited quota */
	if (!ctx->cc_quotas.enabled[quota] || ctx->cc_quotas.limit[quota] == 0)
		return;

	D_ASSERTF(ctx->cc_quotas.current[quota] > 0, "Invalid current limit");
	atomic_fetch_sub(&ctx->cc_quotas.current[quota], 1);

	return;
}
