/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 20/07/2020
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTMS0

#include "lib/trace.h"
#include "lib/memory.h"
#include "lib/finject.h"
#include "lib/assert.h"
#include "lib/arith.h"               /* min_check, M0_3WAY */
#include "lib/misc.h"                /* M0_IN */
#include "lib/errno.h"               /* ENOMEM, EPROTO */
#include "lib/ext.h"
#include "fop/fom_long_lock.h"
#include "fop/fom_generic.h"
#include "fop/fom_interpose.h"
#include "reqh/reqh_service.h"
#include "reqh/reqh.h"               /* m0_reqh */
#include "rpc/rpc_opcodes.h"
#include "rpc/rpc_machine.h"         /* m0_rpc_machine */
#include "conf/schema.h"             /* M0_CST_DTMS0 */
#include "module/instance.h"
#include "addb2/addb2.h"

/* #include "dtms0/dtms0_addb2.h" */
#include "dtms0/dtms0.h"
#include "dtms0/dtms0_xc.h"
#include "motr/setup.h"              /* m0_reqh_context */

struct dtms0_service {
	struct m0_reqh_service  ds_service;
};

struct dtms0_fom {
	struct m0_fom      df_fom;
	struct m0_dtms0_op df_op;

	struct m0_long_lock_link  df_lock;
	bool               df_op_checked;
	struct m0_chan     df_chan;
	/** Mutex protecting df_chan. */
	struct m0_mutex    df_chan_guard;
};

enum dtms0_fom_phase {
	DTMS0_CHECK = M0_FOPH_TYPE_SPECIFIC,
	DTMS0_PREPARE,
	DTMS0_EXEC,
	DTMS0_DONE,
	DTMS0_NR
};

static void   dtms0_prep(struct dtms0_fom *fom);
static int    dtms0_service_start(struct m0_reqh_service *service);
static void   dtms0_service_stop(struct m0_reqh_service *service);
static void   dtms0_service_fini(struct m0_reqh_service *service);
static int    dtms0_service_type_allocate(struct m0_reqh_service **service,
					const struct m0_reqh_service_type *st);
static bool   dtms0_service_started(struct m0_fop *fop, struct m0_reqh *reqh);
static int    dtms0_fom_create(struct m0_fop *fop, struct m0_fom **out,
			       struct m0_reqh *reqh);
static int    dtms0_op_exec(struct dtms0_fom *fom,
			    const struct m0_dtms0_op *op,
			    enum m0_dtms0_opcode opc,
			    struct m0_dtms0_rep *rep,
			    enum dtms0_fom_phase next_phase);
static void   dtms0_fom_cleanup(struct dtms0_fom *fom);
static void   dtms0_fom_failure(struct dtms0_fom *fom, int rc);
static void   dtms0_fom_success(struct dtms0_fom *fom,
				enum m0_dtms0_opcode opc);
static int    dtms0_op_check(struct m0_dtms0_op *op, struct dtms0_fom *fom,
			     bool is_index_drop);
static bool   dtms0_is_valid(struct dtms0_fom *fom, enum m0_dtms0_opcode opc);

static struct m0_dtms0_op     *dtms0_op(const struct m0_fom *fom);
static enum   m0_dtms0_opcode  dtms0_opcode(const struct m0_fop *fop);

M0_INTERNAL   int	       dtms0_op_rc(struct m0_dtms0_op *dtms0_op);

static int  dtms0_done(struct dtms0_fom *fom, struct m0_dtms0_op *op,
		       struct m0_dtms0_rep *rep, enum m0_dtms0_opcode opc);

static bool dtms0_fom_invariant(const struct dtms0_fom *fom);

static const struct m0_reqh_service_ops      dtms0_service_ops;
static const struct m0_reqh_service_type_ops dtms0_service_type_ops;
static const struct m0_fom_ops               dtms0_fom_ops;
static const struct m0_fom_type_ops          dtms0_fom_type_ops;
static       struct m0_sm_conf               dtms0_sm_conf;
static       struct m0_sm_state_descr        dtms0_fom_phases[];
static	     struct m0_sm_trans_descr	     dtms0_fom_trans[];

M0_INTERNAL void m0_dtms0_svc_init(void)
{
	m0_sm_conf_extend(m0_generic_conf.scf_state, dtms0_fom_phases,
			  m0_generic_conf.scf_nr_states);
	m0_sm_conf_trans_extend(&m0_generic_conf, &dtms0_sm_conf);

	m0_sm_conf_init(&dtms0_sm_conf);
	m0_reqh_service_type_register(&m0_dtms0_service_type);
}

M0_INTERNAL void m0_dtms0_svc_fini(void)
{
	m0_reqh_service_type_unregister(&m0_dtms0_service_type);
	m0_sm_conf_fini(&dtms0_sm_conf);
}

M0_INTERNAL void m0_dtms0_svc_fop_args(struct m0_sm_conf            **sm_conf,
				       const struct m0_fom_type_ops **fom_ops,
				       struct m0_reqh_service_type  **svctype)
{
	*sm_conf = &dtms0_sm_conf;
	*fom_ops = &dtms0_fom_type_ops;
	*svctype = &m0_dtms0_service_type;
}

static int dtms0_service_start(struct m0_reqh_service *svc)
{
	int                    rc = 0;
	struct dtms0_service  *service = M0_AMB(service, svc, ds_service);

	M0_PRE(m0_reqh_service_state_get(svc) == M0_RST_STARTING);

	return rc;
}

static void dtms0_service_prepare_to_stop(struct m0_reqh_service *svc)
{
	/* Wait if required until we can stop */
}

static void dtms0_service_stop(struct m0_reqh_service *svc)
{
	M0_PRE(m0_reqh_service_state_get(svc) == M0_RST_STOPPED);
	/* perform some stop related actions. */
}

static void dtms0_service_fini(struct m0_reqh_service *svc)
{
	struct dtms0_service *service = M0_AMB(service, svc, ds_service);

	M0_PRE(M0_IN(m0_reqh_service_state_get(svc),
		     (M0_RST_STOPPED, M0_RST_FAILED)));
	m0_free(service);
}

static int dtms0_service_type_allocate(struct m0_reqh_service **svc,
				     const struct m0_reqh_service_type *stype)
{
	struct dtms0_service *service;

	M0_ALLOC_PTR(service);
	if (service != NULL) {
		*svc = &service->ds_service;
		(*svc)->rs_type = stype;
		(*svc)->rs_ops  = &dtms0_service_ops;
		return M0_RC(0);
	} else
		return M0_ERR(-ENOMEM);
}

static bool dtms0_service_started(struct m0_fop  *fop,
				struct m0_reqh *reqh)
{
	struct m0_reqh_service            *svc;
	const struct m0_reqh_service_type *stype;

	stype = fop->f_type->ft_fom_type.ft_rstype;
	M0_ASSERT(stype != NULL);

	svc = m0_reqh_service_find(stype, reqh);
	M0_ASSERT(svc != NULL);

	return m0_reqh_service_state_get(svc) == M0_RST_STARTED;
}

static int dtms0_fom_create(struct m0_fop *fop,
			  struct m0_fom **out, struct m0_reqh *reqh)
{
	struct dtms0_fom    *fom;
	struct m0_fom     *fom0;
	struct m0_fop     *repfop;
	struct m0_dtms0_rep *repdata;

	if (!dtms0_service_started(fop, reqh))
		return M0_ERR(-EAGAIN);

	M0_ALLOC_PTR(fom);
	/**
	 * Check validity of fop here??
	 */

	repfop = m0_fop_reply_alloc(fop, &dtms0_dtx_rep_fopt);

	if (fom != NULL && repfop != NULL) {
		*out = fom0 = &fom->df_fom;
		repdata = m0_fop_data(repfop);
		repdata->dr_rc = 0;
		m0_fom_init(fom0, &fop->f_type->ft_fom_type,
			    &dtms0_fom_ops, fop, repfop, reqh);
		m0_long_lock_link_init(&fom->df_lock, fom0, NULL);
		return M0_RC(0);
	} else {
		m0_free(repfop);
		m0_free(fom);
		return M0_ERR(-ENOMEM);
	}
}

static int dtms0_op_exec(struct dtms0_fom          *fom,
			const struct m0_dtms0_op  *op,
			enum m0_dtms0_opcode       opc,
			struct m0_dtms0_rep *rep,
			enum dtms0_fom_phase       next_phase)
{
	int result = M0_FSO_AGAIN;

	M0_ENTRY("fom %p, op %p, opc %d", fom, op, opc);

	if (!dtms0_is_valid(fom, opc)) {
		rep->dr_rc = EINVAL;
		return M0_FSO_AGAIN;
	}

	if (dtms0_in_ut()) {
		m0_fom_phase_set(&fom->df_fom, next_phase);
		return M0_FSO_AGAIN;
	}
	/* Else do the actual work. */

	switch (opc) {
	case DT_DTX:
		break;
	case DT_EXECUTE:
		break;
	case DT_PERSISTENT:
		break;
	case DT_REDO:
		break;
	case DT_REPLY:
		break;
	default:
		break;
	}

	m0_fom_phase_set(&fom->df_fom, next_phase);
	return result;
}

static void dtms0_fom_cleanup(struct dtms0_fom *fom)
{
	/* struct m0_dtms0_op  *op     = &fom->df_op; */
}

static void dtms0_fom_failure(struct dtms0_fom *fom, int rc)
{
	struct m0_dtms0_rep *repdata;

	M0_PRE(rc < 0);

	repdata = m0_fop_data(fom->df_fom.fo_rep_fop);
	memset(&repdata->dr_rep, 0, sizeof(repdata->dr_rep));
	repdata->dr_rc = rc;

	dtms0_fom_cleanup(fom);
	m0_fom_phase_move(&fom->df_fom, rc, M0_FOPH_FAILURE);
}

static void dtms0_fom_success(struct dtms0_fom *fom, enum m0_dtms0_opcode opc)
{
	dtms0_fom_cleanup(fom);
	m0_fom_phase_set(&fom->df_fom, M0_FOPH_SUCCESS);
}

static bool dtms0_fom_invariant(const struct dtms0_fom *fom)
{
	const struct m0_fom *fom0    = &fom->df_fom;
	int                  phase   = m0_fom_phase(fom0);
	/* struct m0_dtms0_op    *op      = dtms0_op(fom0); */
	struct dtms0_service  *service = M0_AMB(service,
					      fom0->fo_service, ds_service);

	return _0C(ergo(phase > M0_FOPH_INIT && phase != M0_FOPH_FAILURE, phase <= DTMS0_NR));
		/* _0C(phase <= DTMS0_NR); */
}

static int dtms0_fom_tick(struct m0_fom *fom0)
{
	/* uint64_t	      i; */
	int		      rc;
	int		      result  = M0_FSO_AGAIN;
	int		      phase   = m0_fom_phase(fom0);
	struct m0_dtms0_op   *op      = dtms0_op(fom0);
	struct m0_dtms0_rep  *rep     = m0_fop_data(fom0->fo_rep_fop);
	enum m0_dtms0_opcode  opc     = dtms0_opcode(fom0->fo_fop);
	struct dtms0_fom     *fom     = M0_AMB(fom, fom0, df_fom);
	struct dtms0_service *service = M0_AMB(service,
					       fom0->fo_service, ds_service);

	M0_ENTRY("fom %p phase %d", fom0, phase);
	M0_PRE(dtms0_fom_invariant(fom));
	switch (phase) {
	case M0_FOPH_INIT ... M0_FOPH_NR - 1:

		if (phase == M0_FOPH_INIT && !fom->df_op_checked) {
			/* do some init* */
			m0_fom_phase_set(fom0, DTMS0_CHECK);
			break;
		}
		if (phase == M0_FOPH_FAILURE) {
			/*
			 * Do any necessary cleanup here
			 */
			dtms0_fom_cleanup(fom);
		}
		result = m0_fom_tick_generic(fom0);
		m0_fom_phase_set(fom0, DTMS0_PREPARE);
		break;
	case DTMS0_CHECK:
		rc = dtms0_op_check(op, fom, false);
		if (rc == 0) {
			fom->df_op_checked = true;
			m0_fom_phase_set(fom0, M0_FOPH_INIT);
		} else
			m0_fom_phase_move(fom0, M0_ERR(rc), M0_FOPH_FAILURE);
		break;
	case DTMS0_PREPARE:
		dtms0_prep(fom);
		m0_fom_phase_set(fom0, DTMS0_EXEC);
		break;
	case DTMS0_EXEC:
		result = dtms0_op_exec(fom, op, opc, rep, DTMS0_DONE);
		break;
	case DTMS0_DONE:
		rc = dtms0_done(fom, op, rep, opc);
		if (rc == 0)
			dtms0_fom_success(fom, opc);
		else
			dtms0_fom_failure(fom, rc);
		break;
	default:
		M0_IMPOSSIBLE("Invalid phase");
	}

	M0_POST(dtms0_fom_invariant(fom));
	return M0_RC(result);
}

M0_INTERNAL void (*dtms0__ut_cb_done)(struct m0_fom *fom);
M0_INTERNAL void (*dtms0__ut_cb_fini)(struct m0_fom *fom);

static void dtms0_fom_fini(struct m0_fom *fom0)
{
	struct dtms0_fom *fom = M0_AMB(fom, fom0, df_fom);

	if (dtms0_in_ut() && dtms0__ut_cb_done != NULL)
		dtms0__ut_cb_done(fom0);

	m0_fom_fini(fom0);
	m0_free(fom);
	if (dtms0_in_ut() && dtms0__ut_cb_fini != NULL)
		dtms0__ut_cb_fini(fom0);
}

static struct m0_dtms0_op *dtms0_op(const struct m0_fom *fom)
{
	return m0_fop_data(fom->fo_fop);
}

M0_INTERNAL int dtms0_op_rc(struct m0_dtms0_op *dtms0_op)
{
	M0_PRE(dtms0_op != NULL);

	if (M0_FI_ENABLED("ut-failure"))
		return M0_ERR(-ENOMEM);
	return M0_RC(dtms0_op->dt_rc);
}

static int dtms0_op_check(struct m0_dtms0_op *op,
			struct dtms0_fom   *fom,
			bool              is_index_drop)
{
	struct m0_fom              *fom0	= &fom->df_fom;
	enum m0_dtms0_opcode        opc		= dtms0_opcode(fom0->fo_fop);
	struct m0_dtms0_op         *dt_op	= &fom->df_op;
	int                         rc		= 0;

	rc = dtms0_op_rc(dt_op);

	if (rc == 0)
		rc = (dtms0_in_ut()) ? M0_RC(0) : M0_ERR(-EPROTO);

	if (rc == 0)
		rc = (dtms0_is_valid(fom, opc)) ? M0_RC(0) : M0_ERR(-EPROTO);

	return rc;
}


static enum m0_dtms0_opcode dtms0_opcode(const struct m0_fop *fop0)
{
	struct m0_dtms0_op *fop = m0_fop_data(fop0);
	enum m0_dtms0_opcode opcode;

	opcode = fop->dt_opcode;
	M0_ASSERT(0 <= opcode && opcode < DT_NR);
	return opcode;
}

static bool dtms0_is_valid(struct dtms0_fom *fom, enum m0_dtms0_opcode opc)
{
	bool result;

	switch (opc) {
	case DT_DTX:
	case DT_EXECUTE:
	case DT_PERSISTENT:
	case DT_REDO:
	case DT_REPLY:
		result = true;
		break;
	default:
		result = false;
		M0_IMPOSSIBLE("Wrong opcode.");
	}
	return M0_RC(result);
}

static void dtms0_prep(struct dtms0_fom *fom)
{
	struct m0_fom    *fom0   = &fom->df_fom;
	uint32_t          flags  = dtms0_op(fom0)->dt_opflags;

	M0_ENTRY("dtms0_fom=%p, flags=%d", fom, flags);
	M0_LEAVE();
}

M0_INTERNAL void m0_dtms0_op_fini(struct m0_dtms0_op *dtms0_fop)
{
	M0_ENTRY("dtms0_fop=%p", dtms0_fop);
	M0_PRE(dtms0_fop != NULL);

	M0_SET0(dtms0_fop);
	M0_LEAVE();
}

static int dtms0_done(struct dtms0_fom *fom, struct m0_dtms0_op *op,
		    struct m0_dtms0_rep *rep, enum m0_dtms0_opcode opc)
{
	int op_rc = fom->df_op.dt_rc;
	int rc;

	rc = (op_rc == 0)? rep->dr_rc : op_rc;

	M0_LOG(M0_DEBUG, "fom %p, fop %p, rep %p, opcode %d, rc %d",
			fom, op, rep, opc, rc);

	/* m0_dtms0_op_fini(op); */
	return rc;
}

static const struct m0_fom_ops dtms0_fom_ops = {
	.fo_tick          = &dtms0_fom_tick,
	.fo_fini          = &dtms0_fom_fini,
};

static const struct m0_fom_type_ops dtms0_fom_type_ops = {
	.fto_create = &dtms0_fom_create
};

static struct m0_sm_state_descr dtms0_fom_phases[] = {
	[DTMS0_CHECK] = {
		.sd_name      = "dtms0-check-phase",
		.sd_allowed   = M0_BITS(DTMS0_DONE, M0_FOPH_INIT, M0_FOPH_FAILURE)
	},
	[DTMS0_PREPARE] = {
		.sd_name      = "dtms0-prepare-phase",
		.sd_allowed   = M0_BITS(DTMS0_DONE, M0_FOPH_FAILURE)
	},
	[DTMS0_DONE] = {
		.sd_name      = "dtms0-done-phase",
		.sd_allowed   = M0_BITS(M0_FOPH_SUCCESS)
	},
};

static struct m0_sm_trans_descr dtms0_fom_trans[] = {
	[ARRAY_SIZE(m0_generic_phases_trans)] = {
		"dtms0-check-prepare",
		M0_FOPH_INIT,
		DTMS0_CHECK
	},
	[DTMS0_CHECK] = {
		"dtms0-check-triggered",
		 DTMS0_CHECK,
		 M0_FOPH_INIT
	},
	[DTMS0_PREPARE] = {
		"dtms0-prepare-triggered",
		 DTMS0_PREPARE,
		 DTMS0_DONE
	},
	[DTMS0_DONE] = {
		"dtms0-done-triggered",
		 DTMS0_DONE,
		 M0_FOPH_SUCCESS
	},
};

static struct m0_sm_conf dtms0_sm_conf = {
	.scf_name      = "dtms0-fom",
	.scf_nr_states = ARRAY_SIZE(dtms0_fom_phases),
	.scf_state     = dtms0_fom_phases,
	.scf_trans_nr  = ARRAY_SIZE(dtms0_fom_trans),
	.scf_trans     = dtms0_fom_trans
};

static const struct m0_reqh_service_type_ops dtms0_service_type_ops = {
	.rsto_service_allocate = &dtms0_service_type_allocate
};

static const struct m0_reqh_service_ops dtms0_service_ops = {
	.rso_start_async     = &m0_reqh_service_async_start_simple,
	.rso_start           = &dtms0_service_start,
	.rso_prepare_to_stop = &dtms0_service_prepare_to_stop,
	.rso_stop            = &dtms0_service_stop,
	.rso_fini            = &dtms0_service_fini
};

M0_INTERNAL struct m0_reqh_service_type m0_dtms0_service_type = {
	.rst_name     = "M0_CST_DTMS0",
	.rst_ops      = &dtms0_service_type_ops,
	.rst_level    = M0_RS_LEVEL_NORMAL,
	.rst_typecode = M0_CST_DTMS0
};

#undef M0_TRACE_SUBSYSTEM

/** @} end of cas group */

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
