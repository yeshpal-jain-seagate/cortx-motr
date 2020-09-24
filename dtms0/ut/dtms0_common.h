/* -*- C -*- */
/*
 * COPYRIGHT 2012 XYRATEX TECHNOLOGY LIMITED
 *
 * THIS DRAWING/DOCUMENT, ITS SPECIFICATIONS, AND THE DATA CONTAINED
 * HEREIN, ARE THE EXCLUSIVE PROPERTY OF XYRATEX TECHNOLOGY
 * LIMITED, ISSUED IN STRICT CONFIDENCE AND SHALL NOT, WITHOUT
 * THE PRIOR WRITTEN PERMISSION OF XYRATEX TECHNOLOGY LIMITED,
 * BE REPRODUCED, COPIED, OR DISCLOSED TO A THIRD PARTY, OR
 * USED FOR ANY PURPOSE WHATSOEVER, OR STORED IN A RETRIEVAL SYSTEM
 * EXCEPT AS ALLOWED BY THE TERMS OF XYRATEX LICENSES AND AGREEMENTS.
 *
 * YOU SHOULD HAVE RECEIVED A COPY OF XYRATEX'S LICENSE ALONG WITH
 * THIS RELEASE. IF NOT PLEASE CONTACT A XYRATEX REPRESENTATIVE
 * http://www.xyratex.com/contact
 *
 * Original author: Anand Vidwansa <Anand_Vidwansa@xyratex.com>
 * Original creation date: 02/21/2012
 */

#pragma once

#include "dtms0/client.h"
#ifndef __MOTR_DTMS0_ST_COMMON_H__
#define __MOTR_DTMS0_ST_COMMON_H__

#include "lib/list.h"
#include "motr/init.h"
#include "lib/memory.h"
#include "lib/misc.h"
#include "dtms0/dtms0.h"  /* m0_io_fop */
#include "rpc/rpc.h"            /* m0_rpc_bulk, m0_rpc_bulk_buf */
#include "rpc/rpc_opcodes.h"    /* M0_RPC_OPCODES */
#include "lib/thread.h"         /* M0_THREAD_INIT */
#include "lib/misc.h"           /* M0_SET_ARR0 */

enum DTMS0_UT_VALUES {
	DTMS0_FOPS_NR           = 4,
	DTMS0_FOP_SINGLE        = 1,
	DTMS0_XPRT_NR           = 1,
	DTMS0_CLIENT_SVC_ID     = 2,
	DTMS0_SERVER_SVC_ID     = 1,
	DTMS0_ADDR_LEN          = 32,
	DTMS0_STR_LEN           = 16,
	DTMS0_RPC_ITEM_TIMEOUT  = 300,
};
#define DTMS0_CLIENT_DBNAME   "dtms0_db"
#define DTMS0_SERVER_DBFILE   "dtms0_st.db"
#define DTMS0_SERVER_LOGFILE  "dtms0_st.log"

/* Structure containing data needed for UT. */
struct dtms0_params {
	/* In-memory fops for dtms0 IO. */
	struct m0_dtms0_req      **dtp_req;
	int                        dtp_req_count;

	/* buffers for data transfer. */
	struct m0_net_buffer     **dtp_buf;

	/* Threads to post rpc items to rpc layer. */
	struct m0_thread         **dtp_threads;

	/* Structures used by client-side rpc code. */
	struct m0_net_domain       dtp_cnetdom;

	const char                *dtp_caddr;
	char                      *dtp_cdbname;
	const char                *dtp_saddr;
	char                      *dtp_slogfile;

	struct m0_rpc_client_ctx  *dtp_cctx;
	struct m0_rpc_server_ctx  *dtp_sctx;

	struct m0_net_xprt        *dtp_xprt;
};

/* A structure used to pass as argument to io threads. */
struct thrd_arg {
	/* Index in fops array to be posted to rpc layer. */
	int                  ta_index;
	/* Type of fop to be sent (execute/persistent/...). */
	enum M0_RPC_OPCODES  ta_op;
	/* bulkio_params structure which contains common data. */
	struct dtms0_params *ta_dtp;
};

/* Common APIs used by bulk client as well as UT code. */
int dtms0_client_start(struct dtms0_params *dtp, const char *caddr,
			const char *saddr);

void dtms0_client_stop(struct m0_rpc_client_ctx *cctx);

int dtms0_server_start(struct dtms0_params *bp, const char *saddr);

void dtms0_server_stop(struct m0_rpc_server_ctx *sctx);

void dtms0_params_init(struct dtms0_params *dtp);

void dtms0_params_fini(struct dtms0_params *dtp);

void dtms0_test(struct dtms0_params *dtp, int fops_nr);

extern int m0_bufvec_alloc_aligned(struct m0_bufvec *bufvec, uint32_t num_segs,
				   m0_bcount_t seg_size, unsigned shift);

/**
 * Sends an fsync fop request through the session provided within an io
 * thread args struct.
 * @param remid Remote ID of the transaction that is to be fsynced.
 * @param t Bulkio parameters.
 * @return the rc included in the fsync fop reply.
 */
int dtms0_send_fop(struct m0_be_tx_remid *remid, struct thrd_arg *t);
void dtms0_rpc_submit(struct thrd_arg *t);

void dtms0_fops_destroy(struct dtms0_params *dtp);

void dtms0_fops_create(struct dtms0_params *dtp, enum M0_RPC_OPCODES op,
		     int fops_nr);

#endif /* __MOTR_DTMS0_ST_COMMON_H__ */
