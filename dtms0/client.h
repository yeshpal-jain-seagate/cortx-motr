/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 26/07/2020
 */

#pragma once

#include "linux/types.h"
#ifndef __MOTR_DTMS0_CLIENT_H__
#define __MOTR_DTMS0_CLIENT_H__

#include "lib/time.h" /* m0_time_t */
#include "lib/chan.h" /* m0_chan */
#include "fid/fid.h"  /* m0_fid */
#include "fop/fop.h"  /* m0_fop */
#include "dtms0/dtms0.h"

/* Imports */
struct m0_rpc_session;

/**
 * @defgroup dtms0-client
 *
 * @{
 *
 * DTMS0 client provides an interface to send dtm operations to DTMS0 service.
 * Available:
 */

/** Possible DTMS0 request states. */
enum m0_dtms0_req_state {
	DTMS0REQ_INVALID,
	DTMS0REQ_INIT,
	DTMS0REQ_OPEN,
	DTMS0REQ_CLOSE,
	DTMS0REQ_SENT,
	DTMS0REQ_FINAL,
	DTMS0REQ_FAILURE,
	DTMS0REQ_NR
};

/**
 * Representation of a request to remote DTMS0 service.
 */
struct m0_dtms0_req {
	/** DTMS0 request state machine. */
	struct m0_sm            dr_sm;

	/* Private fields. */
	struct m0_dtms0_op     *dr_op;
	struct m0_dtms0_rep     dr_reply;
	/** FOP carrying op request. */
	struct m0_fop          *dr_fop;
	struct m0_fop_type     *dr_ftype;
	/** RPC session with remote DTMS0 service. */
	struct m0_rpc_session  *dr_sess;
	/** AST to post RPC-related events. */
	struct m0_sm_ast        dr_replied_ast;
	/** AST to move sm to failure state. */
	struct m0_sm_ast        dr_failure_ast;
};

/**
 * Initialises DTMS0 client request.
 *
 * Initialisation should be done always before sending specific request.
 * RPC session 'sess' to DTMS0 service should be already established.
 *
 * @pre M0_IS0(req)
 */
M0_INTERNAL void m0_dtms0_req_init(struct m0_dtms0_req   *req,
				   struct m0_rpc_session *sess,
				   struct m0_sm_group    *grp);

/**
 * Finalises DTMS0 client request.
 */
M0_INTERNAL void m0_dtms0_req_fini(struct m0_dtms0_req *req);

M0_INTERNAL void m0_dtms0_req_fini_lock(struct m0_dtms0_req *req);
/**
 * Locks state machine group that is used by DTMS0 request.
 */
M0_INTERNAL void m0_dtms0_req_lock(struct m0_dtms0_req *req);

/**
 * Unlocks state machine group that is used by dtms0 request.
 */
M0_INTERNAL void m0_dtms0_req_unlock(struct m0_dtms0_req *req);

/**
 * Checks whether DTMS0 request state machine group is locked.
 */
M0_INTERNAL bool m0_dtms0_req_is_locked(const struct m0_dtms0_req *req);

M0_INTERNAL int m0_dtms0_req_wait(struct m0_dtms0_req *req, uint64_t states,
				  m0_time_t to);

M0_INTERNAL int m0_dtms0_dtx(struct m0_dtms0_req *req);

M0_INTERNAL int  m0_dtms0_sm_conf_init(void);
M0_INTERNAL void m0_dtms0_sm_conf_fini(void);

/** @} end of dtms0-client group */
#endif /* __MOTR_DTMS0_CLIENT_H__ */

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
