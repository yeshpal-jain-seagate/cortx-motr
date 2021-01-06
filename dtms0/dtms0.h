/* -*- C -*- */
/*
 * Original author: Mehul Joshi <mehul.joshi@seagate.com>
 * Original creation date: 20/07/2020
 */

#pragma once

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
 * DTMS0 service registers following FOP types during initialisation:
 * - @ref dtms0_dtx_req_fopt
 * - @ref dtms0_dtx_redo_fopt
 * - @ref dtms0_dtx_rep_fopt
 *
 * @see @ref dtms0_dfspec "Detailed Functional Specification"
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

enum m0_dtms0_msg_type {
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
	int		    dto_rc;
	uint32_t	    dto_opcode;
	uint32_t	    dto_opflags;
	uint32_t	    dto_size;
	void		   *dto_data;
} M0_XCA_RECORD M0_XCA_DOMAIN(rpc);

/**
 * DTMS0 fops reply.
 */
struct m0_dtms0_rep {
	/** Status code of dtm operation. */
	int32_t		dtr_rc;
	/** operation results. */
	struct m0_buf	dtr_rep;
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
