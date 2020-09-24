/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 26/07/2020
 */

#include "rpc/rpc_opcodes.h"
#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTMS0

#include "lib/trace.h"
#include "lib/vec.h"
#include "lib/misc.h"    /* M0_IN */
#include "lib/memory.h"
#include "sm/sm.h"
#include "rpc/item.h"
#include "rpc/rpc.h"     /* m0_rpc_post */
#include "rpc/session.h" /* m0_rpc_session */
#include "rpc/conn.h"    /* m0_rpc_conn */
#include "fop/fop.h"
#include "fop/fom_generic.h"
#include "dtms0/dtms0.h"
#include "dtms0/dtms0_xc.h"
#include "dtms0/client.h"

/**
 * @addtogroup dtms0-client
 * @{
 */

#define DTMS0REQ_FOP_DATA(fop) ((struct m0_dtms0_op *)m0_fop_data(fop))

static void dtms0_req_replied_cb(struct m0_rpc_item *item);

static const struct m0_rpc_item_ops dtms0_item_ops = {
	.rio_sent    = NULL,
	.rio_replied = dtms0_req_replied_cb
};

static struct m0_sm_state_descr dtms0_req_states[] = {
	[DTMS0REQ_INIT] = {
		.sd_flags     = M0_SDF_INITIAL | M0_SDF_FINAL,
		.sd_name      = "init",
		.sd_allowed   = M0_BITS(DTMS0REQ_SENT)
	},
	[DTMS0REQ_SENT] = {
		.sd_name      = "request-sent",
		.sd_allowed   = M0_BITS(DTMS0REQ_FINAL, DTMS0REQ_FAILURE)
	},
	[DTMS0REQ_FINAL] = {
		.sd_name      = "final",
		.sd_flags     = M0_SDF_TERMINAL,
	},
	[DTMS0REQ_FAILURE] = {
		.sd_name      = "failure",
		.sd_flags     = M0_SDF_TERMINAL | M0_SDF_FAILURE
	}
};

static struct m0_sm_trans_descr dtms0_req_trans[] = {
	{ "send-over-rpc",       DTMS0REQ_INIT,       DTMS0REQ_SENT       },
	{ "rpc-failure",         DTMS0REQ_SENT,       DTMS0REQ_FAILURE    },
	{ "req-processed",       DTMS0REQ_SENT,       DTMS0REQ_FINAL      },
};

struct m0_sm_conf dtms0_req_sm_conf = {
	.scf_name      = "dtms0_req",
	.scf_nr_states = ARRAY_SIZE(dtms0_req_states),
	.scf_state     = dtms0_req_states,
	.scf_trans_nr  = ARRAY_SIZE(dtms0_req_trans),
	.scf_trans     = dtms0_req_trans
};

static const char * DTMS0_OP_STR[] = {
	 "DTMS0 DTX RECORD",
	 "DTMS0 EXECUTE RECORD",
	 "DTMS0 PERSISTENT RECORD",
	 "DTMS0 REDO RECORD",
	 "DTMS0 REPLY RECORD"
};

static int dreq_op_alloc(struct m0_dtms0_op **out,
			 uint32_t	      size,
			 uint32_t	      oc,
			 uint32_t	      of)
{
	struct m0_dtms0_op *op;
	char *buf;

	M0_ALLOC_PTR(op);
	if (op == NULL)
		return M0_ERR(-ENOMEM);
	M0_SET0(op);

	M0_ALLOC_ARR(buf, size);
	if (buf == NULL){
		m0_free(buf);
		return M0_ERR(-ENOMEM);
	}
	M0_SET0(buf);

	op->dt_data = buf;
	op->dt_size = size;
	op->dt_id = 0;

	op->dt_opcode = oc;
	op->dt_opflags = of;

	*out = op;
	return M0_RC(0);
}

static void dreq_op_free(struct m0_dtms0_op *op)
{
	if (op != NULL) {
		m0_free(op->dt_data);
		m0_free(op);
	}
}

M0_INTERNAL void m0_dtms0_req_init(struct m0_dtms0_req   *req,
				   struct m0_rpc_session *sess,
				   struct m0_sm_group    *grp)
{
	M0_ENTRY();
	M0_PRE(sess != NULL);
	M0_PRE(M0_IS0(req));
	req->dr_sess = sess;
	m0_sm_init(&req->dr_sm, &dtms0_req_sm_conf, DTMS0REQ_INIT, grp);
	m0_sm_addb2_counter_init(&req->dr_sm);
	M0_LEAVE();
}

static struct m0_rpc_conn *dreq_rpc_conn(const struct m0_dtms0_req *req)
{
	return req->dr_sess->s_conn;
}

static struct m0_rpc_machine *dreq_rpc_mach(const struct m0_dtms0_req *req)
{
	return dreq_rpc_conn(req)->c_rpc_machine;
}

static struct m0_sm_group *dtms0_req_smgrp(const struct m0_dtms0_req *req)
{
	return req->dr_sm.sm_grp;
}

M0_INTERNAL void m0_dtms0_req_lock(struct m0_dtms0_req *req)
{
	M0_ENTRY();
	m0_sm_group_lock(dtms0_req_smgrp(req));
}

M0_INTERNAL void m0_dtms0_req_unlock(struct m0_dtms0_req *req)
{
	M0_ENTRY();
	m0_sm_group_unlock(dtms0_req_smgrp(req));
}

M0_INTERNAL bool m0_dtms0_req_is_locked(const struct m0_dtms0_req *req)
{
	return m0_mutex_is_locked(&dtms0_req_smgrp(req)->s_lock);
}

static void dtms0_req_state_set(struct m0_dtms0_req     *req,
			        enum m0_dtms0_req_state  state)
{
	M0_LOG(M0_DEBUG, "DTMS0 req: %p, state change:[%s -> %s]\n",
	       req, m0_sm_state_name(&req->dr_sm, req->dr_sm.sm_state),
	       m0_sm_state_name(&req->dr_sm, state));
	m0_sm_state_set(&req->dr_sm, state);
}

static void dtms0_req_reply_fini(struct m0_dtms0_req *req)
{
}

static void dtms0_req_fini(struct m0_dtms0_req *req)
{
	uint32_t cur_state = req->dr_sm.sm_state;

	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));
	M0_PRE(M0_IN(cur_state, (DTMS0REQ_INIT, DTMS0REQ_FINAL, DTMS0REQ_FAILURE)));
	if (cur_state == DTMS0REQ_FAILURE) {
		if (req->dr_fop != NULL) {
			dreq_op_free(req->dr_fop->f_data.fd_data);
			req->dr_fop->f_data.fd_data = NULL;
			m0_fop_put_lock(req->dr_fop);
		}
	}
	dtms0_req_reply_fini(req);
	m0_sm_fini(&req->dr_sm);
	M0_LEAVE();
}

M0_INTERNAL void m0_dtms0_req_fini(struct m0_dtms0_req *req)
{
	M0_PRE(m0_dtms0_req_is_locked(req));
	dtms0_req_fini(req);
	M0_SET0(req);
}

M0_INTERNAL void m0_dtms0_req_fini_lock(struct m0_dtms0_req *req)
{
	M0_ENTRY();
	m0_dtms0_req_lock(req);
	dtms0_req_fini(req);
	m0_dtms0_req_unlock(req);
	M0_SET0(req);
	M0_LEAVE();
}

static void dreq_fop_destroy(struct m0_dtms0_req *req)
{
	struct m0_fop *fop = req->dr_fop;

	m0_free(fop);
	req->dr_fop = NULL;
}

static void dreq_fop_release(struct m0_ref *ref)
{
	struct m0_fop *fop;

	M0_ENTRY();
	M0_PRE(ref != NULL);
	fop = container_of(ref, struct m0_fop, f_ref);
	if (fop->f_data.fd_data != NULL) {
		dreq_op_free(fop->f_data.fd_data);
		fop->f_data.fd_data = NULL;
	}
	M0_LEAVE();
}

static int dreq_fop_create(struct m0_dtms0_req  *req,
			   struct m0_fop_type   *ftype,
			   struct m0_dtms0_op   *op)
{
	struct m0_fop *fop;

	M0_ALLOC_PTR(fop);
	if (fop == NULL)
		return M0_ERR(-ENOMEM);

	m0_fop_init(fop, ftype, (void *)op, dreq_fop_release);
	fop->f_opaque = req;
	req->dr_fop = fop;
	req->dr_ftype = ftype;

	return 0;
}

static int dreq_fop_create_and_prepare(struct m0_dtms0_req     *req,
				       struct m0_fop_type      *ftype,
				       struct m0_dtms0_op      *op,
				       enum m0_dtms0_req_state *next_state)
{
	int rc;

	M0_SET0(req);
	rc = dreq_fop_create(req, ftype, op);
	if (rc == 0) {
		*next_state = DTMS0REQ_SENT;
		/*
		 * Check whether original fop payload does not exceed
		 * max rpc item payload.
		 */
		if (m0_rpc_item_max_payload_exceeded(
			    &req->dr_fop->f_item,
			    req->dr_sess)) {
			*next_state = DTMS0REQ_FAILURE;
			rc = -E2BIG;
		}
		if (rc != 0)
			dreq_fop_destroy(req);
	}
	return M0_RC(rc);
}

static struct m0_dtms0_req *item_to_dtms0_req(struct m0_rpc_item *item)
{
	struct m0_fop *fop = M0_AMB(fop, item, f_item);
	return (struct m0_dtms0_req *)fop->f_opaque;
}

static struct m0_rpc_item *dtms0_req_to_item(const struct m0_dtms0_req *req)
{
	return &req->dr_fop->f_item;
}

static struct m0_dtms0_rep *dtms0_rep(struct m0_rpc_item *reply)
{
	return m0_fop_data(m0_rpc_item_to_fop(reply));
}

M0_INTERNAL int m0_dtms0_req_generic_rc(const struct m0_dtms0_req *req)
{
	struct m0_fop      *req_fop = req->dr_fop;
	struct m0_rpc_item *reply;
	int                 rc;

	M0_PRE(M0_IN(req->dr_sm.sm_state, (DTMS0REQ_FINAL, DTMS0REQ_FAILURE)));

	reply = req_fop != NULL ? dtms0_req_to_item(req)->ri_reply : NULL;

	rc = req->dr_sm.sm_rc;
	if (rc == 0 && reply != NULL)
		rc = m0_rpc_item_generic_reply_rc(reply);

	return M0_RC(rc);
}

static int dtms0_rep__validate(const struct m0_fop_type *ftype,
			     struct m0_dtms0_op         *op,
			     struct m0_dtms0_rep        *rep)
{
	M0_ASSERT(M0_IN(ftype, (&dtms0_dtx_fopt, &dtms0_dtx_execute_fopt,
				&dtms0_dtx_persistent_fopt, &dtms0_dtx_redo_fopt)));
	return M0_RC(0);
}

static int dtms0_rep_validate(const struct m0_dtms0_req *req)
{
	const struct m0_fop *rfop = req->dr_fop;
	struct m0_dtms0_op    *op = m0_fop_data(rfop);
	struct m0_dtms0_rep   *rep = dtms0_rep(dtms0_req_to_item(req)->ri_reply);

	return rep->dr_rc ?: dtms0_rep__validate(rfop->f_type, op, rep);
}

static void dtms0_req_failure(struct m0_dtms0_req *req, int32_t rc)
{
	M0_PRE(rc != 0);
	m0_sm_fail(&req->dr_sm, DTMS0REQ_FAILURE, rc);
}

static void dtms0_req_failure_ast(struct m0_sm_group *grp, struct m0_sm_ast *ast)
{
	struct m0_dtms0_req *req = container_of(ast, struct m0_dtms0_req,
					      dr_failure_ast);
	int32_t rc = (long)ast->sa_datum;

	M0_PRE(rc != 0);
	dtms0_req_failure(req, M0_ERR(rc));
}

static void dtms0_req_failure_ast_post(struct m0_dtms0_req *req, int32_t rc)
{
	M0_ENTRY();
	req->dr_failure_ast.sa_cb = dtms0_req_failure_ast;
	req->dr_failure_ast.sa_datum = (void *)(long)rc;
	m0_sm_ast_post(dtms0_req_smgrp(req), &req->dr_failure_ast);
	M0_LEAVE();
}

static void dreq_item_prepare(const struct m0_dtms0_req      *req,
			      struct m0_rpc_item           *item,
			      const struct m0_rpc_item_ops *ops)
{
	item->ri_rmachine = dreq_rpc_mach(req);
	item->ri_ops      = ops;
	item->ri_session  = req->dr_sess;
	item->ri_prio     = M0_RPC_ITEM_PRIO_MID;
	item->ri_deadline = M0_TIME_IMMEDIATELY;
}

static void dtms0_fop_send(struct m0_dtms0_req *req)
{
	/* struct m0_dtms0_op   *op = m0_fop_data(req->dr_fop); */
	struct m0_rpc_item *item;
	int                 rc;

	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));
	item = dtms0_req_to_item(req);
	dreq_item_prepare(req, item, &dtms0_item_ops);
	rc = m0_rpc_post(item);
	M0_LOG(M0_NOTICE, "RPC post returned %d", rc);
}

#if 0
/**
 * Gets transacation identifier information from returned reply fop
 * of a DTX op.
 */
static void dtms0_req_remid_copy(struct m0_dtms0_req *req)
{
	struct m0_dtms0_rep *rep;

	M0_PRE(req != NULL);
	M0_PRE(req->ccr_fop != NULL);

	rep = dtms0_rep(dtms0_req_to_item(req)->ri_reply);
	M0_ASSERT(rep != NULL);

	req->ccr_remid = rep->cgr_mod_rep.fmr_remid;
}
#endif

static int dtms0_req_reply_handle(struct m0_dtms0_req *req)
{
	struct m0_fop      *req_fop = req->dr_fop;
	struct m0_dtms0_rep  *rep = dtms0_rep(m0_fop_to_rpc_item(req_fop));

	int                 rc = 0;

	M0_ASSERT(req_fop->f_type == req->dr_ftype);

	rc = rep->dr_rc;

	m0_fop_put_lock(req_fop);
	req->dr_fop = NULL;

	return rc;
}

static void dtms0_req_replied_ast(struct m0_sm_group *grp, struct m0_sm_ast *ast)
{
	struct m0_dtms0_req  *req = container_of(ast, struct m0_dtms0_req,
					       dr_replied_ast);
	int                 rc;

	rc = dtms0_rep_validate(req);
	if (rc == 0) {
		rc = dtms0_req_reply_handle(req);
		if (rc == 0)
			dtms0_req_state_set(req, DTMS0REQ_FINAL);
	}
	if (rc != 0) {
		dtms0_req_failure(req, M0_ERR(rc));
	}
}

static void dtms0_req_replied_cb(struct m0_rpc_item *item)
{
	struct m0_dtms0_req *req = item_to_dtms0_req(item);

	M0_ENTRY();
	if (item->ri_error == 0) {
		req->dr_replied_ast.sa_cb = dtms0_req_replied_ast;
		m0_sm_ast_post(dtms0_req_smgrp(req), &req->dr_replied_ast);
	} else
		dtms0_req_failure_ast_post(req, item->ri_error);
	M0_LEAVE();
}

static int dtms0_op_prepare(struct m0_dtms0_op **out,
			    uint32_t             opcode,
			    uint32_t             flags)
{
	struct m0_dtms0_op  *op;
	int                  rc;

	M0_ENTRY();

	rc = dreq_op_alloc(&op, 1024, opcode, flags);
	if (rc != 0)
		return M0_ERR(rc);
	snprintf(op->dt_data, 1023, "request contains %s data", DTMS0_OP_STR[opcode]);
	*out = op;
	return M0_RC(rc);
}

static int dtms0_req_prepare(struct m0_dtms0_req  *req,
			     struct m0_dtms0_op  **out,
			     uint32_t              opcode,
			     uint32_t              flags)
{
	int                  rc;
	struct m0_dtms0_op  *op;
	struct m0_dtms0_rep *reply;

	rc = dtms0_op_prepare(&op, opcode, flags);
	if (rc != 0)
		return M0_RC(rc);

	M0_SET0(req);
	reply = &req->dr_reply;
	reply->dr_rc = 0;

	*out = op;
	return M0_RC(rc);

}

M0_INTERNAL int m0_dtms0_req_wait(struct m0_dtms0_req *req, uint64_t states,
				m0_time_t to)
{
	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));
	return M0_RC(m0_sm_timedwait(&req->dr_sm, states, to));
}

M0_INTERNAL int m0_dtms0_dtx(struct m0_dtms0_req *req)
{
	struct m0_dtms0_op      *op;
	enum m0_dtms0_req_state  next_state;
	int                      rc;

	M0_ENTRY();
	M0_PRE(req->dr_sess != NULL);
	M0_PRE(m0_dtms0_req_is_locked(req));
	rc = dtms0_req_prepare(req, &op, DT_DTX, DOF_NONE);
	if (rc != 0)
		return M0_ERR(rc);
	rc = dreq_fop_create_and_prepare(req, &dtms0_dtx_fopt, op, &next_state);
	if (rc == 0) {
		dtms0_fop_send(req);
		dtms0_req_state_set(req, next_state);
	}
	return M0_RC(rc);
}

static void dtms0_rep_copy(const struct m0_dtms0_req *req,
			 struct m0_dtms0_rep	     *reply)
{
	struct m0_dtms0_rep  *rep = dtms0_rep(m0_fop_to_rpc_item(req->dr_fop));
	reply->dr_rc = rep->dr_rc;
}

M0_INTERNAL void m0_dtms0_dtx_reply(const struct m0_dtms0_req *req,
					 struct m0_dtms0_rep *rep)
{
	M0_ENTRY();
	M0_PRE(req->dr_ftype == &dtms0_dtx_fopt);
	dtms0_rep_copy(req, rep);
	M0_LEAVE();
}

M0_INTERNAL int m0_dtms0_execute(struct m0_dtms0_req    *req,
			     struct m0_dtx          *dtx,
			     uint32_t                flags)
{
	struct m0_dtms0_op      *op;
	enum m0_dtms0_req_state  next_state;
	int                      rc;

	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));

	(void)dtx;
	rc = dtms0_req_prepare(req, &op, DT_EXECUTE, DOF_NONE);
	if (rc != 0)
		return M0_ERR(rc);
	rc = dreq_fop_create_and_prepare(req, &dtms0_dtx_execute_fopt, op, &next_state);
	if (rc == 0) {
		dtms0_fop_send(req);
		dtms0_req_state_set(req, next_state);
	}
	return M0_RC(rc);
}

M0_INTERNAL void m0_dtms0_execute_reply(struct m0_dtms0_req	*req,
				  struct m0_dtms0_rep *rep)
{
	M0_ENTRY();
	M0_PRE(req->dr_ftype == &dtms0_dtx_execute_fopt);
	dtms0_rep_copy(req, rep);
	M0_LEAVE();
}

M0_INTERNAL int m0_dtms0_persistent(struct m0_dtms0_req *req,
			   struct m0_dtx     *dtx,
			   uint32_t           flags)
{
	struct m0_dtms0_op      *op;
	enum m0_dtms0_req_state  next_state;
	int                    rc;

	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));
	M0_PRE(M0_IN(flags, (0, DOF_NONE, DOF_SYNC_WAIT)));

	(void)dtx;
	rc = dtms0_req_prepare(req, &op, DT_PERSISTENT, DOF_NONE);
	if (rc != 0)
		return M0_ERR(rc);
	rc = dreq_fop_create_and_prepare(req, &dtms0_dtx_persistent_fopt, op, &next_state);
	if (rc == 0) {
		dtms0_fop_send(req);
		dtms0_req_state_set(req, next_state);
	}
	return M0_RC(rc);
}

M0_INTERNAL void m0_dtms0_persistent_reply(struct m0_dtms0_req *req,
				  struct m0_dtms0_rep *rep)
{
	M0_ENTRY();
	M0_PRE(req->dr_ftype == &dtms0_dtx_persistent_fopt);
	dtms0_rep_copy(req, rep);
	M0_LEAVE();
}

M0_INTERNAL int m0_dtms0_redo(struct m0_dtms0_req *req,
			      struct m0_dtx     *dtx,
			      uint32_t           flags)
{
	struct m0_dtms0_op      *op;
	enum m0_dtms0_req_state  next_state;
	int                    rc;

	M0_ENTRY();
	M0_PRE(m0_dtms0_req_is_locked(req));
	M0_PRE(M0_IN(flags, (0, DOF_NONE, DOF_SYNC_WAIT)));

	(void)dtx;
	rc = dtms0_req_prepare(req, &op, DT_REDO, DOF_NONE);
	if (rc != 0)
		return M0_ERR(rc);
	rc = dreq_fop_create_and_prepare(req, &dtms0_dtx_redo_fopt, op, &next_state);
	if (rc == 0) {
		dtms0_fop_send(req);
		dtms0_req_state_set(req, next_state);
	}
	return M0_RC(rc);
}

M0_INTERNAL void m0_dtms0_redo_reply(struct m0_dtms0_req *req,
				  struct m0_dtms0_rep *rep)
{
	M0_ENTRY();
	M0_PRE(req->dr_ftype == &dtms0_dtx_redo_fopt);
	dtms0_rep_copy(req, rep);
	M0_LEAVE();
}

M0_INTERNAL int  m0_dtms0_sm_conf_init(void)
{
	m0_sm_conf_init(&dtms0_req_sm_conf);
	return M0_RC(0);
	/* return m0_sm_addb2_init(&dtms0_req_sm_conf, */
	/* 			M0_AVI_DTMS0_SM_REQ, */
	/* 			M0_AVI_DTMS0_SM_REQ_COUNTER); */
}

M0_INTERNAL void m0_dtms0_sm_conf_fini(void)
{
	/* m0_sm_addb2_fini(&dtms0_req_sm_conf); */
	m0_sm_conf_fini(&dtms0_req_sm_conf);
}

#undef M0_TRACE_SUBSYSTEM

/** @} end of dtms0-client group */

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
