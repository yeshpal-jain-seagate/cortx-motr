/* -*- C -*- */
/*
 * Copyright (c) 2013-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */



/**
 * @addtogroup dtm
 *
 * @{
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTM
#include "dtm0/dtx.h"
#include "lib/assert.h" /* M0_PRE */
#include "lib/memory.h" /* M0_ALLOC */
#include "lib/errno.h"  /* ENOMEM */
#include "lib/trace.h"  /* M0_ERR */
#include "dtm0/service.h" /* m0_dtm0_service */
#include "reqh/reqh.h" /* reqh2confc */
#include "conf/helpers.h" /* proc2srv */

static struct m0_sm_state_descr dtx_states[] = {
	[M0_DDS_INIT] = {
		.sd_flags     = M0_SDF_INITIAL | M0_SDF_FINAL,
		.sd_name      = "init",
		.sd_allowed   = M0_BITS(M0_DDS_INPROGRESS),
	},
	[M0_DDS_INPROGRESS] = {
		.sd_name      = "dtx-executed",
		.sd_allowed   = M0_BITS(M0_DDS_EXECUTED, M0_DDS_FAILED),
	},
	[M0_DDS_EXECUTED] = {
		.sd_name      = "dtx-persistent",
		.sd_allowed   = M0_BITS(M0_DDS_PERSISTENT),
	},
	[M0_DDS_PERSISTENT] = {
		.sd_name      = "dtx-persistent",
		.sd_allowed   = M0_BITS(M0_DDS_STABLE),
	},
	[M0_DDS_STABLE] = {
		.sd_name      = "dtx-stable",
		.sd_allowed   = M0_BITS(M0_DDS_DONE),
	},
	[M0_DDS_DONE] = {
		.sd_name      = "done",
		.sd_flags     = M0_SDF_TERMINAL,
	},
	[M0_DDS_FAILED] = {
		.sd_name      = "dtx-failed",
		.sd_flags     = M0_SDF_TERMINAL | M0_SDF_FAILURE
	}
};

static struct m0_sm_trans_descr dtx_trans[] = {
	{ "populated",  M0_DDS_INIT,       M0_DDS_INPROGRESS },
	{ "executed",   M0_DDS_INPROGRESS, M0_DDS_EXECUTED   },
	{ "exec-fail",  M0_DDS_INPROGRESS, M0_DDS_FAILED     },
	{ "persistent", M0_DDS_EXECUTED,   M0_DDS_PERSISTENT },
	{ "stable",     M0_DDS_PERSISTENT, M0_DDS_STABLE     },
	{ "prune",      M0_DDS_STABLE,     M0_DDS_DONE       }
};

static struct m0_sm_conf dtx_sm_conf = {
	.scf_name      = "dtm0dtx",
	.scf_nr_states = ARRAY_SIZE(dtx_states),
	.scf_state     = dtx_states,
	.scf_trans_nr  = ARRAY_SIZE(dtx_trans),
	.scf_trans     = dtx_trans,
};

static int m0_dtm0_dtx_init(struct m0_dtm0_service *svc,
			    struct m0_sm_group     *sm_group,
			    struct m0_dtm0_dtx     **out)
{
	struct m0_dtm0_dtx *dtx;

	M0_ALLOC_PTR(dtx);
	if (dtx == NULL)
		return M0_ERR(-ENOMEM);

	m0_sm_conf_init(&dtx_sm_conf);
	m0_sm_init(&dtx->dd_sm, &dtx_sm_conf, M0_DDS_INIT, sm_group);
	dtx->dd_dtms = svc;
	dtx->dd_ancient_dtx.tx_dtx = dtx;
	*out = dtx;
	return 0;
}

static void m0_dtm0_dtx_fini(struct m0_dtm0_dtx *dtx)
{
	m0_sm_fini(&dtx->dd_sm);
	m0_free(dtx);
}

static int m0_dtm0_dtx_prepare(struct m0_dtm0_dtx *dtx)
{
	int               rc;
	struct m0_dtm0_ts now;

	rc = m0_dtm0_clk_src_now(&dtx->dd_dtms->dos_clk_src, &now);
	if (rc != 0)
		return rc;

	dtx->dd_txd.dtd_id = (struct m0_dtm0_tid) {
		.dti_ts  = now,
		.dti_fid = dtx->dd_dtms->dos_generic.rs_service_fid,
	};

	M0_POST(m0_dtm0_tid__invariant(&dtx->dd_txd.dtd_id));
	return rc;
}

static int m0_dtm0_dtx_open(struct m0_dtm0_dtx  *dtx,
			    uint32_t             nr)
{
	return m0_dtm0_tx_desc_init(&dtx->dd_txd, nr);
}

static void m0_dtm0_dtx_assign(struct m0_dtm0_dtx  *dtx,
			       uint32_t             pa_idx,
			       const struct m0_fid *pa_fid)
{
	struct m0_dtm0_tx_pa   *pa;
	struct m0_reqh         *reqh;
	struct m0_conf_cache   *cache;
	struct m0_conf_obj     *obj;
	struct m0_conf_process *proc;
	struct m0_fid           rdtms_fid;
	int rc;

	M0_PRE(dtx);
	M0_PRE(pa_idx < dtx->dd_txd.dtd_pg.dtpg_nr);

	/* TODO: Should we release any conf objects in the end? */

	reqh = dtx->dd_dtms->dos_generic.rs_reqh;
	cache = &m0_reqh2confc(reqh)->cc_cache;

	obj = m0_conf_cache_lookup(cache, pa_fid);
	M0_ASSERT_INFO(obj != NULL, "User service is not in the conf cache?");

	obj = m0_conf_obj_grandparent(obj);
	M0_ASSERT_INFO(obj != NULL, "Process the service belongs to "
		       "is not a part of the conf cache?");

	proc = M0_CONF_CAST(obj, m0_conf_process);
	M0_ASSERT_INFO(proc != NULL, "The grandparent is not a process?");

	rc = m0_conf_process2service_get(&reqh->rh_rconfc.rc_confc,
					 &proc->pc_obj.co_id, M0_CST_DTM0,
					 &rdtms_fid);

	M0_ASSERT_INFO(rc == 0, "Cannot find remote DTM service on the remote "
		       "process that runs this user service?");

	pa = &dtx->dd_txd.dtd_pg.dtpg_pa[pa_idx];
	M0_PRE(M0_IS0(pa));

	pa->pa_fid = rdtms_fid;
	M0_ASSERT(pa->pa_state == M0_DTPS_INIT);
	pa->pa_state = M0_DTPS_INPROGRESS;

	M0_LOG(DEBUG, "pa: " FID_F " (User) => " FID_F " (DTM) ",
	       FID_P(pa_fid), FID_P(&rdtms_fid));
}

static int m0_dtm0_dtx_close(struct m0_dtm0_dtx *dtx)
{
	M0_PRE(dtx);

	m0_sm_state_set(&dtx->dd_sm, M0_DDS_INPROGRESS);

	/* TODO: add a log entry */

	return 0;
}

M0_INTERNAL int m0_dtx_dtm0_init(struct m0_dtm0_service *svc,
				 struct m0_sm_group     *group,
				 struct m0_dtx         **out)
{
	struct m0_dtm0_dtx *dtx;
	int                 rc;

	if (svc == NULL) {
		*out = NULL;
		return 0;
	}

	rc = m0_dtm0_dtx_init(svc, group, &dtx);
	if (rc != 0)
		return rc;

	*out = &dtx->dd_ancient_dtx;
	return 0;
}

M0_INTERNAL void m0_dtx_dtm0_fini(struct m0_dtx **pdtx)
{
	if (*pdtx == NULL) {
		return;
	}

	m0_dtm0_dtx_fini((*pdtx)->tx_dtx);
	*pdtx = NULL;
}

M0_INTERNAL int m0_dtx_dtm0_prepare(struct m0_dtx *dtx)
{
	return dtx == NULL ? 0 : m0_dtm0_dtx_prepare(dtx->tx_dtx);
}

M0_INTERNAL int m0_dtx_dtm0_open(struct m0_dtx  *dtx, uint32_t nr)
{
	return dtx == NULL ? 0 : m0_dtm0_dtx_open(dtx->tx_dtx, nr);
}

M0_INTERNAL void m0_dtx_dtm0_assign(struct m0_dtx       *dtx,
				    uint32_t             pa_idx,
				    const struct m0_fid *pa_fid)
{
	return dtx == NULL ? 0 : m0_dtm0_dtx_assign(dtx->tx_dtx, pa_idx,
						    pa_fid);
}

M0_INTERNAL int m0_dtx_dtm0_close(struct m0_dtx *dtx)
{
	return dtx == NULL ? 0 : m0_dtm0_dtx_close(dtx->tx_dtx);
}

M0_INTERNAL int m0_dtx_dtm0_copy_txd(struct m0_dtx *dtx,
				     struct m0_dtm0_tx_desc *txd)
{
	if (dtx == NULL) {
		M0_SET0(txd);
		return 0;
	}

	return m0_dtm0_tx_desc_copy(&dtx->tx_dtx->dd_txd, txd);
}


#undef M0_TRACE_SUBSYSTEM

/** @} end of dtm group */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
