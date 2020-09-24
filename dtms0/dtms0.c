/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 20/07/2020
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTMS0

#include "lib/trace.h"
#include "lib/finject.h"     /* M0_FI_ENABLED */
#include "fop/fop.h"
#include "rpc/rpc_opcodes.h"
#include "dtms0/dtms0.h"
#include "dtms0/dtms0_xc.h"
#include "dtms0/client.h"   /* m0_dtms0_sm_conf_init */

struct m0_fom_type_ops;
struct m0_sm_conf;
struct m0_reqh_service_type;

/**
 * @addtogroup dtms0
 *
 * @{
 */

M0_INTERNAL struct m0_fop_type dtms0_dtx_fopt;
M0_INTERNAL struct m0_fop_type dtms0_dtx_execute_fopt;
M0_INTERNAL struct m0_fop_type dtms0_dtx_persistent_fopt;
M0_INTERNAL struct m0_fop_type dtms0_dtx_redo_fopt;
M0_INTERNAL struct m0_fop_type dtms0_dtx_rep_fopt;

static int dtms0_fops_init(const struct m0_sm_conf           *sm_conf,
			   const struct m0_fom_type_ops      *fom_ops,
			   const struct m0_reqh_service_type *svctype)
{
	M0_FOP_TYPE_INIT(&dtms0_dtx_fopt,
			 .name      = "dtms0-dtx",
			 .opcode    = M0_DTMS0_DTX_FOP_OPCODE,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REQUEST,
			 .xt        = m0_dtms0_op_xc,
			 .fom_ops   = fom_ops,
			 .sm        = sm_conf,
			 .svc_type  = svctype);
	M0_FOP_TYPE_INIT(&dtms0_dtx_execute_fopt,
			 .name      = "dtms0-execute",
			 .opcode    = M0_DTMS0_DTX_EXECUTE_FOP_OPCODE,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REQUEST |
				      M0_RPC_ITEM_TYPE_MUTABO,
			 .xt        = m0_dtms0_op_xc,
			 .fom_ops   = fom_ops,
			 .sm        = sm_conf,
			 .svc_type  = svctype);
	M0_FOP_TYPE_INIT(&dtms0_dtx_persistent_fopt,
			 .name      = "dtms0-persistent",
			 .opcode    = M0_DTMS0_DTX_PERSISTENT_FOP_OPCODE,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REQUEST |
				      M0_RPC_ITEM_TYPE_MUTABO,
			 .xt        = m0_dtms0_op_xc,
			 .fom_ops   = fom_ops,
			 .sm        = sm_conf,
			 .svc_type  = svctype);
	M0_FOP_TYPE_INIT(&dtms0_dtx_redo_fopt,
			 .name      = "dtms0-redo",
			 .opcode    = M0_DTMS0_DTX_REDO_FOP_OPCODE,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REQUEST |
				      M0_RPC_ITEM_TYPE_MUTABO,
			 .xt        = m0_dtms0_op_xc,
			 .fom_ops   = fom_ops,
			 .sm        = sm_conf,
			 .svc_type  = svctype);
	M0_FOP_TYPE_INIT(&dtms0_dtx_rep_fopt,
			 .name      = "dtms0-rep",
			 .opcode    = M0_DTMS0_DTX_REP_FOP_OPCODE,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REPLY,
			 .xt        = m0_dtms0_rep_xc,
			 .svc_type  = svctype);
	return  m0_fop_type_addb2_instrument(&dtms0_dtx_fopt) ?:
		m0_fop_type_addb2_instrument(&dtms0_dtx_execute_fopt) ?:
		m0_fop_type_addb2_instrument(&dtms0_dtx_persistent_fopt) ?:
		m0_fop_type_addb2_instrument(&dtms0_dtx_redo_fopt);
}

static void dtms0_fops_fini(void)
{
	m0_fop_type_addb2_deinstrument(&dtms0_dtx_fopt);
	m0_fop_type_addb2_deinstrument(&dtms0_dtx_execute_fopt);
	m0_fop_type_addb2_deinstrument(&dtms0_dtx_persistent_fopt);
	m0_fop_type_addb2_deinstrument(&dtms0_dtx_redo_fopt);
	m0_fop_type_fini(&dtms0_dtx_rep_fopt);
	m0_fop_type_fini(&dtms0_dtx_redo_fopt);
	m0_fop_type_fini(&dtms0_dtx_persistent_fopt);
	m0_fop_type_fini(&dtms0_dtx_execute_fopt);
	m0_fop_type_fini(&dtms0_dtx_fopt);
}

M0_INTERNAL int m0_dtms0_module_init(void)
{
	struct m0_sm_conf            *sm_conf;
	const struct m0_fom_type_ops *fom_ops;
	struct m0_reqh_service_type  *svctype;

	m0_dtms0_svc_init();
	m0_dtms0_svc_fop_args(&sm_conf, &fom_ops, &svctype);
	return dtms0_fops_init(sm_conf, fom_ops, svctype) ?:
	       m0_dtms0_sm_conf_init();
}

M0_INTERNAL void m0_dtms0_module_fini(void)
{
	m0_dtms0_sm_conf_fini();
	dtms0_fops_fini();
	m0_dtms0_svc_fini();
}

M0_INTERNAL bool dtms0_in_ut(void)
{
	return M0_FI_ENABLED("ut");
}

#undef M0_TRACE_SUBSYSTEM

/** @} end of dtms0 group */

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
