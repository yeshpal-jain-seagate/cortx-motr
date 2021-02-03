/* -*- C -*- */
/*
 * Copyright (c) 2012-2020 Seagate Technology LLC and/or its Affiliates
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


#include "lib/errno.h"
#include "lib/assert.h"
#include "lib/memory.h"
#include "lib/chan.h"
#include "lib/finject.h"
#include "lib/time.h"
#include "lib/misc.h"           /* M0_IN() */
#include "fop/fop.h"
#include "fop/fom.h"
#include "fop/fom_generic.h"

#include "rpc/rpc.h"
#include "rpc/rpclib.h"
#include "fop/fop_item_type.h"

#include "dtm0/fop.h"
#include "dtm0/fop_xc.h"
#include "dtm0/service.h"
#include "rpc/rpc_opcodes.h"

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item);

/*
  RPC item operations structures.
 */
const struct m0_rpc_item_ops dtm0_req_fop_rpc_item_ops = {
        .rio_replied = dtm0_rpc_item_reply_cb,
};

struct m0_fop_type dtm0_req_fop_fopt;
struct m0_fop_type dtm0_rep_fop_fopt;

/*
  Fom specific routines for corresponding fops.
 */
static int dtm0_fom_tick(struct m0_fom *fom);
static int dtm0_req_fop_fom_create(struct m0_fop *fop, struct m0_fom **out,
				   struct m0_reqh *reqh);
static void dtm0_fom_fini(struct m0_fom *fom);
static size_t dtm0_fom_locality(const struct m0_fom *fom);

static const struct m0_fom_ops dtm0_req_fop_fom_ops = {
	.fo_fini = dtm0_fom_fini,
	.fo_tick = dtm0_fom_tick,
	.fo_home_locality = dtm0_fom_locality
};

extern struct m0_reqh_service_type dtm0_service_type;

enum dtm0_phases {
	DTM0_REQ_WHATEVER = M0_FOPH_NR + 1,
};

static const struct m0_fom_type_ops dtm0_req_fop_fom_type_ops = {
        .fto_create = dtm0_req_fop_fom_create,
};

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item)
{
	struct m0_fop *reply;

        M0_PRE(item != NULL);
	M0_PRE(M0_IN(m0_fop_opcode(m0_rpc_item_to_fop(item)),
		     (M0_DTM0_REQ_OPCODE)));

	if (m0_rpc_item_error(item) == 0) {
		reply = m0_rpc_item_to_fop(item->ri_reply);
		M0_ASSERT(M0_IN(m0_fop_opcode(reply), (M0_DTM0_REP_OPCODE)));
	}
}

void m0_dtm0_fop_fini(void)
{
	m0_fop_type_fini(&dtm0_req_fop_fopt);
	m0_fop_type_fini(&dtm0_rep_fop_fopt);
	m0_xc_dtm0_fop_fini();
}

int m0_dtm0_fop_init(void)
{
	static int init_once = 0;
	if (init_once++ > 0)
		return 0;

	m0_xc_dtm0_fop_init();
	M0_FOP_TYPE_INIT(&dtm0_req_fop_fopt,
			 .name      = "DTM0 request",
			 .opcode    = M0_DTM0_REQ_OPCODE,
			 .xt        = dtm0_req_fop_xc,
			 .rpc_flags = M0_RPC_MUTABO_REQ,
			 .fom_ops   = &dtm0_req_fop_fom_type_ops,
			 .sm        = &m0_generic_conf,
			 .svc_type  = &dtm0_service_type);
	M0_FOP_TYPE_INIT(&dtm0_rep_fop_fopt,
			 .name      = "DTM0 reply",
			 .opcode    = M0_DTM0_REP_OPCODE,
			 .xt        = dtm0_rep_fop_xc,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REPLY,
			 .fom_ops   = &dtm0_req_fop_fom_type_ops);
	return 0;
}

/*
  Allocates and initialises a fom.
 */
static int dtm0_req_fop_fom_create(struct m0_fop *fop,
				   struct m0_fom **out, struct m0_reqh *reqh)
{
        struct m0_fom           *fom;
	struct m0_fop           *rfop;

	M0_PRE(fop != NULL);
        M0_PRE(out != NULL);
	M0_PRE(M0_IN(m0_fop_opcode(fop), (M0_DTM0_REQ_OPCODE)));

        M0_ALLOC_PTR(fom);
        if (fom == NULL)
                return -ENOMEM;

	rfop = m0_fop_reply_alloc(fop, &dtm0_rep_fop_fopt);

	if (rfop == NULL) {
		m0_free(fom);
		return -ENOMEM;
	}
	m0_fom_init(fom, &fop->f_type->ft_fom_type,
		    &dtm0_req_fop_fom_ops, fop, rfop, reqh);

        *out = fom;
        return 0;
}

static void dtm0_fom_fini(struct m0_fom *fom)
{
	M0_PRE(fom != NULL);

        m0_fom_fini(fom);
        m0_free(fom);
}

static size_t dtm0_fom_locality(const struct m0_fom *fom)
{
	static int locality = 0;
	M0_PRE(fom != NULL);
	return locality++;
}

#define M0_FID(c_, k_)  { .f_container = c_, .f_key = k_ }
M0_UNUSED static void dtm0_pong_back(struct m0_rpc_session *session)
{
        struct m0_fop         *fop;
	struct dtm0_req_fop   *req;
	struct m0_rpc_item    *item;
	int                    rc;

	M0_PRE(session != NULL);

	fop = m0_fop_alloc_at(session, &dtm0_req_fop_fopt);
	req = m0_fop_data(fop);
	req->drf_value = 555;

	item              = &fop->f_item;
	item->ri_ops      = &dtm0_req_fop_rpc_item_ops;
	item->ri_session  = session;
	item->ri_prio     = M0_RPC_ITEM_PRIO_MID;
	item->ri_deadline = M0_TIME_IMMEDIATELY;

	rc = m0_rpc_post(item);
	M0_ASSERT(rc == 0);
	m0_fop_put_lock(fop); // XXX: shall we lock here???
}

//static void reserve_credits(const struct m0_fom       *fom,
//			    struct m0_be_dtm0_log     *log,
//			    struct m0_be_tx_credit    *accum)
//{
//	struct m0_dtm0_log_record *log_rec;
//	struct m0_be_seg          *seg = fom->fo_tx.be_tx.seg;
//
//	m0_be_dtm0_log_find(log, &req->drf_txr.dtd_id, &log_rec);
//	if (log_rec != NULL) { /* log record exists */
//		m0_be_dtm0_log_credit(M0_DTML_PERSISTENT, seg, accum);
//	} else {
//		m0_be_dtm0_log_credit(M0_DTML_PERSISTENT, seg, accum);
//		m0_be_dtm0_log_credit(M0_DTML_CREATE    , seg, accum);
//	}
//
//	// 0. tx_reserve_credits(credit)
//	// tx.reg_area.reserve(credit)
//	//
//
//	// 1. memcpy()
//	// [    uuu1                        ]
//	// [         uuu2                   ]
//	// [              uuu3              ]
//
//	// 2. tx_capture()
//	// tx.reg_area.copy(uuu1);
//	// tx.reg_area.copy(uuu2);
//	// tx.reg_area.copy(uuu3);
//
//
//	// 3. log structures
//	// [       xxx---->yyU----->UUU
//	// [                          |
//	// [                          |
//	// [                          V
//	// [                        UUUUUUU
//	// [                           |
//	// [                          UUU  UUU
//
//
//}


static int dtm0_fom_tick(struct m0_fom *fom)
{
	int                  rc;
	struct dtm0_req_fop *req;
	struct dtm0_rep_fop *rep;

	if (m0_fom_phase(fom) < M0_FOPH_NR) {
		//if (m0_fom_phase(fom) == TX_OPEN &&
		//    m0_dtm0_service_is_persistent(fom->service)) {
		//	reserve_credits(req,
		//                      reqh_service_to_dtm0(fom->fo_service)->log,
		//                      &fom->fo_tx.be_tx_credit);
		//}

		rc = m0_fom_tick_generic(fom);
	} else {

		//m0_be_dtm0_log_update(reqh_service_to_dtm0(fom->fo_service)->log,
		//		      &fom->fo_tx.be_tx,
		//		      req->txr);


		req = m0_fop_data(fom->fo_fop);
		rep = m0_fop_data(fom->fo_rep_fop);
		rep->drf_rc = req->drf_value;
		m0_fom_phase_set(fom, M0_FOPH_SUCCESS);
		rc = M0_FSO_AGAIN;
	}

	return rc;
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
