/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 20/07/2020
 */

#pragma once

#include "rpc/at.h"
#ifndef __MOTR_DTMS0_DTMS0_H__
#define __MOTR_DTMS0_DTMS0_H__

#include "xcode/xcode_attr.h"
#include "lib/types.h"
#include "lib/buf.h"
#include "lib/buf_xc.h"
#include "fop/fom_generic.h"    /* m0_fop_mod_rep */
#include "fop/fom_generic_xc.h" /* m0_fop_mod_rep */
#include "dtm/update.h"

/**
 * @page dtms0-fspec The dtms0 service (DTMS0)
 *
 * The interface of the service for a user is a format of request/reply FOPs.
 * DTMS0 service is a request handler service and provides several callbacks to
 * the request handler, see @ref m0_dtms0_service_type::rst_ops().
 * DTMS0 service registers following FOP types during initialisation:
 * - @ref dtms0_dtx_req_fopt
 * - @ref dtms0_dtx_rep_fopt
 *
 * @section dtms0-fspec-ds Data Structures
 * DTMS0 OPs:
 * - @ref m0_dtms0_req
 * Reply OPs:
 * - @ref m0_dtms0_rep
 *
 * The primary datastructure that describes a user operation and all the
 * associated data for the request is m0_dtms0_op. This structure has a member
 * field that carries an operation code. The operation code specifies what kind
 * of an operation this is. It can be one of:
 *  DT_REQ - for a dtms0 request, or
 *  DT_REPLY - for a dtms0 reply operation
 *
 * Each m0_dtms0_op also carries a m0_dtms_msg type which tells what kind of
 * message it is carrying.
 * A m0_dtms0_op containing a request must specify a message type of either
 * DMT_EXECUTE_DTX,indicating this an original request, or DMT_REDO, indicating
 * that this is a redo request.
 * Similarly, a m0_dtms0_op containing a reply operation should contain either a
 * DMT_EXECUTED or a DMT_PERSISTENT message.
 *
 * struct m0_dtms0_op also contains a buffer to send data like the txr payload.
 * The exact data depends on the rest of the logic that implements the dtms0
 * operation. Additional buffers will be added to pass the user client data
 * and information that is needed to invoke the user action.
 *
 * struct m0_dtms0_op also has field to pass additional flags to the DTMS0
 * operation FOP/FOM handler.
 * m0_dtms0_op_flags
 *
 * Request OPs:
 * - @ref m0_dtms0_req
 * Reply OPs:
 * - @ref m0_dtms0_rep
 *
 * A user request is represented by m0_dtms0_req and user reply by m0_dtms0_rep.
 * They are a part of the client side interface of dtms0.
 * An m0_dtms0_req object contains the m0_dtms0_op, m0_fop, connection and session
 * information and everything else that is necessary to send the op.
 * Likewise, m0_dtms0_rep object is used to receive the reply dtms0 fop reply.
 * It has fields for a return code and additional data. More fields can be added
 * to this structure depending on the caller.
 *
 * Request has associated state machine executed in a state machine group
 * provided during initialisation. All requests should be invoked with state
 * machine group mutex held. There are helpers to lock/unlock this state machine
 * group (m0_dtms0_lock(), m0_dtms0_unlock()). All request state transitions are
 * done in the context of this state machine group.
 *
 * Possible DTMS0 client request states are:
	DTMS0REQ_INIT,
	DTMS0REQ_OPEN,
	DTMS0REQ_CLOSE,
	DTMS0REQ_SENT,
	DTMS0REQ_FINAL,
	DTMS0REQ_FAILURE,
 */

/**
 * @defgroup dtms0-client
 *
 * @{
 *
 * To the user/application, a dtms0 client provides an interface to submit
 * dtm0 operations to dtms0 service.
 * To DTM0 a dtms0-client also functions as the originator of dtm0 calls,
 * automatically managing connections with dtm0 participants, sending dtm0
 * operation requests, receiving replies, managing request states and
 * participating in recovery actions. It can also be used to explicitly
 * specify destination of every request.
 *
 * General workflow with the request is:
 * - m0_dtms0_req_init(req, dtms0_rpc_session, sm_grp);
 * - Add clink to m0_dtms0_req::ccr_chan channel.
 * - Send a m0_dtms0_op request over RPC session.
 * - Wait asynchronously or synchronously until req->dr_sm reaches
 *   DTMS0REQ_FINAL or DTMS0REQ_FAILURE state;
 * - Check return code of operation with m0_dtms0_req_generic_rc(req);
 * - If return code is 0, then obtain operation-specific reply.
 *   m0_dtms0_get_rep(req, 0, rep);
 * - m0_dtms0_req_fini(req);
 *
 * Available requests:
 * - m0_dtms0_send_request()
 */

/**
 * @{
 */

/* Import */
struct m0_sm_conf;
struct m0_fom_type_ops;
struct m0_reqh_service_type;

/**
 * DTMS0 operation flags.
 */
enum m0_dtms0_op_flags {
	/**
	 * Define any operation flags
	 */
	DOF_NONE = 0,
	/**
	 * Delay reply until transaction is persisted.
	 */
	DOF_SYNC_WAIT = 1 << 0,
} M0_XCA_ENUM;

/**
* DTMS0 operation codes.
*/

enum m0_dtms0_opcode{
	DT_REQ,
	DT_REDO,
	DT_REPLY,
	DT_NR,
} M0_XCA_ENUM;

enum m0_dtms0_msg {
	DMT_EXECUTE_DTX,
	DMT_EXECUTED,
	DMT_PERSISTENT,
	DMT_REDO,
	DMT_REPLY,
	DMT_NR,
} M0_XCA_ENUM;

/**
 * DTMS0 ops.
 */

struct m0_dtms0_op {
	int32_t			 dto_rc;
	enum m0_dtms0_opcode   	 dto_opcode;
	enum m0_dtms0_msg   	 dto_opmsg;
	uint32_t		 dto_opflags;
	struct m0_rpc_at_buf	 dto_opdata;
} M0_XCA_RECORD M0_XCA_DOMAIN(rpc);

/**
 * DTMS0 fops reply.
 */
struct m0_dtms0_rep {
	/** Status code of dtm operation. */
	int32_t		dtr_rc;
	/** operation results. */
	struct m0_rpc_at_buf	 dtr_repdata;
} M0_XCA_RECORD M0_XCA_DOMAIN(rpc);

M0_EXTERN struct m0_reqh_service_type m0_dtms0_service_type;

M0_EXTERN struct m0_fop_type dtms0_dtx_req_fopt;
M0_EXTERN struct m0_fop_type dtms0_dtx_rep_fopt;
M0_EXTERN struct m0_fop_type dtms0_dtx_redo_req_fopt;

/**
 * Use stubs in kernel mode for service initialisation and deinitalisation.
 * Use NULLs for service-specific fields in DTMS0 FOP types.
 */
#ifndef __KERNEL__
M0_INTERNAL void m0_dtms0_svc_init(void);
M0_INTERNAL void m0_dtms0_svc_fini(void);
M0_INTERNAL void m0_dtms0_svc_fop_args(struct m0_sm_conf            **sm_conf,
				       const struct m0_fom_type_ops **fom_ops,
				       struct m0_reqh_service_type  **svctype);
#else
#define m0_dtms0_svc_init()
#define m0_dtms0_svc_fini()
#define m0_dtms0_svc_fop_args(sm_conf, fom_ops, svctype) \
do {                                                     \
	*(sm_conf) = NULL;                               \
	*(fom_ops) = NULL;                               \
	*(svctype) = NULL;                               \
} while (0);
#endif /* __KERNEL__ */

M0_INTERNAL bool dtms0_in_ut(void);

#endif /* __MOTR_DTMS0_DTMS0_H__ */

/*
 * }@
 */

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
