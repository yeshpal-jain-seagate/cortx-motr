/* -*- C -*- */
/*
 * Copyright (c) 2015-2020 Seagate Technology LLC and/or its Affiliates
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
 * @addtogroup be
 *
 * @{
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_BE
#include "lib/trace.h"

#include "be/tx_bulk.h"

#include "lib/memory.h"         /* M0_ALLOC_ARR */
#include "lib/locality.h"       /* m0_locality_get */
#include "lib/chan.h"           /* m0_clink */
#include "lib/errno.h"          /* ENOENT */

#include "be/tx.h"              /* m0_be_tx */
#include "be/domain.h"          /* m0_be_domain__group_limits */

#include "sm/sm.h"              /* m0_sm_ast */

enum {
	/**
	 * Maximum number of be_tx_bulk_worker-s in m0_be_tx_bulk.
	 *
	 * This value can be tuned to increase performance.
	 */
	BE_TX_BULK_WORKER_MAX = 0x40,
};

struct be_tx_bulk_worker {
	struct m0_be_tx        tbw_tx;
	struct m0_be_tx_bulk  *tbw_tb;
	struct m0_be_tbq_data  tbw_queue_data;
	struct m0_sm_ast       tbw_queue_get;
	struct m0_sm_ast       tbw_init;
	struct m0_sm_ast       tbw_close;
	void                  *tbw_user;
	struct m0_clink        tbw_clink;
	struct m0_sm_group    *tbw_grp;
	int                    tbw_rc;
	struct m0_be_op        tbw_op;
	bool                   tbw_failed;
	bool                   tbw_done;
};

/* This function should be tuned using real life data. */
static uint32_t be_tx_bulk_worker_nr(struct m0_be_tx_bulk *tb,
                                     uint32_t              group_nr,
                                     uint32_t              tx_per_group)
{
	return min_check((uint32_t)BE_TX_BULK_WORKER_MAX,
			 tx_per_group * (group_nr + 1));
}

static void be_tx_bulk_queue_get_cb(struct m0_sm_group *grp,
				    struct m0_sm_ast   *ast);
static void be_tx_bulk_init_cb(struct m0_sm_group *grp, struct m0_sm_ast *ast);
static bool be_tx_bulk_open_cb(struct m0_clink *clink);
static void be_tx_bulk_close_cb(struct m0_sm_group *grp, struct m0_sm_ast *ast);
static void be_tx_bulk_gc_cb(struct m0_be_tx *tx, void *param);
static void be_tx_bulk_queue_get_done_cb(struct m0_be_op *op, void *param);

M0_INTERNAL int m0_be_tx_bulk_init(struct m0_be_tx_bulk     *tb,
                                   struct m0_be_tx_bulk_cfg *tb_cfg)
{
	struct be_tx_bulk_worker *worker;
	uint32_t                  group_nr;
	uint32_t                  tx_per_group;
	uint32_t                  i;
	int                       rc;

	M0_PRE(M0_IS0(tb));

	tb->btb_cfg = *tb_cfg;
	m0_be_domain__group_limits(tb->btb_cfg.tbc_dom,
				   &group_nr, &tx_per_group);
	tb->btb_worker_nr = be_tx_bulk_worker_nr(tb, group_nr, tx_per_group);
	tb->btb_tx_open_failed = false;
	tb->btb_the_end = false;
	tb->btb_done = false;
	tb->btb_termination_in_progress = false;
	m0_mutex_init(&tb->btb_lock);
	M0_ALLOC_ARR(tb->btb_worker, tb->btb_worker_nr);
	rc = tb->btb_worker == NULL ? -ENOMEM : 0;
	if (rc != 0) {
		m0_mutex_fini(&tb->btb_lock);
		return M0_ERR(rc);
	}
	rc = m0_be_tbq_init(&tb->btb_q, &tb->btb_cfg.tbc_q_cfg);
	if (rc != 0) {
		m0_free(tb->btb_worker);
		m0_mutex_fini(&tb->btb_lock);
		return M0_ERR(rc);
	}
	m0_be_op_init(&tb->btb_kill_put_op);
	for (i = 0; i < tb->btb_worker_nr; ++i) {
		worker = &tb->btb_worker[i];
		*worker = (struct be_tx_bulk_worker){
			.tbw_tb        = tb,
			.tbw_grp       = m0_locality_get(i)->lo_grp,
			.tbw_rc        = 0,
			.tbw_queue_get = {
				.sa_cb    = &be_tx_bulk_queue_get_cb,
				.sa_datum = worker,
			},
			.tbw_init      = {
				.sa_cb    = &be_tx_bulk_init_cb,
				.sa_datum = worker,
			},
			.tbw_close     = {
				.sa_cb    = &be_tx_bulk_close_cb,
				.sa_datum = worker,
			},
			.tbw_failed    = false,
			.tbw_done      = false,
		};
		m0_be_op_init(&worker->tbw_op);
		m0_be_op_callback_set(&worker->tbw_op,
		                      &be_tx_bulk_queue_get_done_cb,
		                      worker, M0_BOS_DONE);
	}
	return rc;
}

M0_INTERNAL void m0_be_tx_bulk_fini(struct m0_be_tx_bulk *tb)
{
	uint32_t i;

	for (i = 0; i < tb->btb_worker_nr; ++i)
		m0_be_op_fini(&tb->btb_worker[i].tbw_op);
	m0_be_op_fini(&tb->btb_kill_put_op);
	m0_be_tbq_fini(&tb->btb_q);
	m0_mutex_fini(&tb->btb_lock);
	m0_free(tb->btb_worker);
}

static void be_tx_bulk_lock(struct m0_be_tx_bulk *tb)
{
	m0_mutex_lock(&tb->btb_lock);
}

static void be_tx_bulk_unlock(struct m0_be_tx_bulk *tb)
{
	m0_mutex_unlock(&tb->btb_lock);
}

static void be_tx_bulk_workers_terminate(struct m0_be_tx_bulk     *tb,
                                         struct be_tx_bulk_worker *worker,
                                         bool                      terminated)
{
	struct m0_be_tbq_data data = { .bbd_done = true };
	uint32_t              done_nr = 0;
	uint32_t              i;
	bool                  done;
	bool                  terminate_next;
	int                   rc;

	M0_ENTRY("tb=%p worker=%p terminated=%d", tb, worker, !!terminated);
	be_tx_bulk_lock(tb);
	if (worker != NULL)
		worker->tbw_done = true;
	for (i = 0; i < tb->btb_worker_nr; ++i)
		done_nr += tb->btb_worker[i].tbw_done;
	M0_LOG(M0_DEBUG, "done_nr=%"PRIu32, done_nr);
	done = done_nr == tb->btb_worker_nr;
	if (done) {
		tb->btb_rc = 0;
		for (i = 0; i < tb->btb_worker_nr; ++i) {
			rc = tb->btb_worker[i].tbw_rc;
			if (rc != 0) {
				tb->btb_rc = rc;
				break;
			}
		}
		tb->btb_done = true;
	}
	be_tx_bulk_unlock(tb);
	if (done) {
		m0_be_op_done(tb->btb_op);
	} else {
		be_tx_bulk_lock(tb);
		if (terminated)
			tb->btb_termination_in_progress = false;
		if (!tb->btb_termination_in_progress) {
			terminate_next = true;
			tb->btb_termination_in_progress = true;
		}
		be_tx_bulk_unlock(tb);
		if (terminate_next) {
			M0_LOG(M0_DEBUG, "terminate next");
			m0_be_op_reset(&tb->btb_kill_put_op);
			m0_be_tbq_lock(&tb->btb_q);
			m0_be_tbq_put(&tb->btb_q, &tb->btb_kill_put_op, &data);
			m0_be_tbq_unlock(&tb->btb_q);
		}
	}
	M0_LEAVE();
}

static void be_tx_bulk_queue_get_cb(struct m0_sm_group *grp,
				    struct m0_sm_ast   *ast)
{
	struct be_tx_bulk_worker *worker = ast->sa_datum;
	struct m0_be_tbq_data     data;
	struct m0_be_tx_bulk     *tb = worker->tbw_tb;
	bool                      drain_the_queue;

	M0_ENTRY("worker=%p", worker);
	M0_PRE(ast == &worker->tbw_queue_get);

	if (worker->tbw_rc != 0 || worker->tbw_queue_data.bbd_done) {
		/* @see be_tx_bulk_open_cb() */
		if (worker->tbw_rc != 0)
			m0_be_tx_fini(&worker->tbw_tx);
		be_tx_bulk_lock(tb);
		drain_the_queue = tb->btb_tx_open_failed;
		be_tx_bulk_unlock(tb);
		if (drain_the_queue) {
			m0_be_tbq_lock(&tb->btb_q);
			while (m0_be_tbq_peek(&tb->btb_q, &data)) {
				M0_BE_OP_SYNC(op,
					      m0_be_tbq_get(&tb->btb_q,
							    &op, &data));
			}
			m0_be_tbq_unlock(&tb->btb_q);
		}
		be_tx_bulk_workers_terminate(tb, worker,
		                             worker->tbw_queue_data.bbd_done);
		/* nothing must be done here */
	} else {
		m0_be_op_reset(&worker->tbw_op);
		m0_be_tbq_lock(&tb->btb_q);
		m0_be_tbq_get(&tb->btb_q, &worker->tbw_op,
		              &worker->tbw_queue_data);
		m0_be_tbq_unlock(&tb->btb_q);
		M0_LEAVE("worker=%p", worker);
	}
}

static void be_tx_bulk_queue_get_done_cb(struct m0_be_op *op, void *param)
{
	struct be_tx_bulk_worker *worker = param;

	M0_ENTRY("worker=%p", worker);

	m0_sm_ast_post(worker->tbw_grp,
	               worker->tbw_queue_data.bbd_done ?
	               &worker->tbw_queue_get : &worker->tbw_init);
}

static void be_tx_bulk_open(struct be_tx_bulk_worker *worker,
                            struct m0_be_tx_credit   *cred,
                            m0_bcount_t               cred_payload)
{
	struct m0_be_tx_bulk     *tb     = worker->tbw_tb;
	struct m0_be_tx_bulk_cfg *tb_cfg = &tb->btb_cfg;
	struct m0_be_tx          *tx     = &worker->tbw_tx;

	M0_SET0(tx);
	m0_be_tx_init(tx, 0, tb_cfg->tbc_dom, worker->tbw_grp,
		      NULL, NULL, NULL, NULL);
	m0_be_tx_gc_enable(tx, &be_tx_bulk_gc_cb, worker);

	M0_SET0(&worker->tbw_clink);
	m0_clink_init(&worker->tbw_clink, &be_tx_bulk_open_cb);
	m0_clink_add(&tx->t_sm.sm_chan, &worker->tbw_clink);

	m0_be_tx_prep(tx, cred);
	m0_be_tx_payload_prep(tx, cred_payload);
	m0_be_tx_open(tx);
}

static void be_tx_bulk_init_cb(struct m0_sm_group *grp, struct m0_sm_ast *ast)
{
	struct be_tx_bulk_worker *worker = ast->sa_datum;

	M0_PRE(ast == &worker->tbw_init);

	M0_ENTRY("worker=%p", worker);
	be_tx_bulk_open(worker, &worker->tbw_queue_data.bbd_credit,
	                worker->tbw_queue_data.bbd_payload_size);
	M0_LEAVE("worker=%p", worker);
}

static bool be_tx_bulk_open_cb(struct m0_clink *clink)
{
	struct be_tx_bulk_worker *worker;
	struct m0_be_tx_bulk     *tb;
	struct m0_be_tx          *tx;
	bool                      killing_has_started;

	worker = container_of(clink, struct be_tx_bulk_worker, tbw_clink);
	M0_ENTRY("worker=%p", worker);
	tx = &worker->tbw_tx;
	tb =  worker->tbw_tb;
	if (M0_IN(m0_be_tx_state(tx), (M0_BTS_ACTIVE, M0_BTS_FAILED))) {
		m0_clink_del(&worker->tbw_clink);
		m0_clink_fini(&worker->tbw_clink);

		if (m0_be_tx_state(tx) == M0_BTS_ACTIVE) {
			m0_sm_ast_post(worker->tbw_grp, &worker->tbw_close);
		} else {
			be_tx_bulk_lock(tb);
			killing_has_started =
				tb->btb_the_end || tb->btb_tx_open_failed;
			tb->btb_tx_open_failed = true;
			be_tx_bulk_unlock(tb);
			if (!killing_has_started)
				worker->tbw_queue_data.bbd_done = true;
			worker->tbw_rc = tx->t_sm.sm_rc;
			M0_LOG(M0_ERROR, "tx=%p rc=%d", tx, worker->tbw_rc);
			/*
			 * Can't call m0_be_tx_fini(tx) here because
			 * m0_be_tx_put() for M0_BTS_FAILED transaction
			 * is called after worker transition.
			 *
			 * be_tx_bulk_init_cb() will do this.
			 */
			be_tx_bulk_gc_cb(tx, worker);
		}
	}
	M0_LEAVE("worker=%p", worker);
	return false;
}

static void be_tx_bulk_close_cb(struct m0_sm_group *grp, struct m0_sm_ast *ast)
{
	struct m0_be_tx_bulk_cfg  *tb_cfg;
	struct be_tx_bulk_worker  *worker = ast->sa_datum;
	struct m0_be_tx_bulk      *tb;

	M0_ENTRY("worker=%p", worker);
	M0_PRE(ast == &worker->tbw_close);
	tb = worker->tbw_tb;
	tb_cfg = &tb->btb_cfg;
	M0_BE_OP_SYNC(op, tb_cfg->tbc_do(tb, &worker->tbw_tx, &op,
	                                 tb_cfg->tbc_datum,
					 worker->tbw_queue_data.bbd_user));
	m0_be_tx_close(&worker->tbw_tx);
	M0_LEAVE("worker=%p", worker);
}

static void be_tx_bulk_gc_cb(struct m0_be_tx *tx, void *param)
{
	struct be_tx_bulk_worker *worker = param;

	M0_ENTRY("worker=%p", worker);
	M0_PRE(tx == &worker->tbw_tx);

	m0_sm_ast_post(worker->tbw_grp, &worker->tbw_queue_get);

	M0_LEAVE("worker=%p", worker);
}

M0_INTERNAL void m0_be_tx_bulk_run(struct m0_be_tx_bulk *tb,
                                   struct m0_be_op      *op)
{
	uint32_t i;

	M0_ENTRY();
	tb->btb_op = op;
	m0_be_op_active(tb->btb_op);
	for (i = 0; i < tb->btb_worker_nr; ++i) {
		M0_LOG(M0_DEBUG, "i=%"PRIu32" worker=%p",
		       i, &tb->btb_worker[i]);
		m0_sm_ast_post(tb->btb_worker[i].tbw_grp,
		               &tb->btb_worker[i].tbw_queue_get);
	}
	M0_LEAVE();
}

M0_INTERNAL bool m0_be_tx_bulk_put(struct m0_be_tx_bulk   *tb,
                                   struct m0_be_op        *op,
                                   struct m0_be_tx_credit *credit,
                                   m0_bcount_t             payload_credit,
                                   uint64_t                partition,
                                   void                   *user)
{
	struct m0_be_tbq_data data = {
		.bbd_user         = user,
		.bbd_credit       = *credit,
		.bbd_payload_size = payload_credit,
		.bbd_done         = false,
	};
	bool                  put_fail;

	be_tx_bulk_lock(tb);
	M0_ASSERT(!tb->btb_the_end);
	put_fail = tb->btb_tx_open_failed;
	be_tx_bulk_unlock(tb);

	if (put_fail)
		return false;

	M0_PRE(!tb->btb_the_end);

	m0_be_tbq_lock(&tb->btb_q);
	m0_be_tbq_put(&tb->btb_q, op, &data);
	m0_be_tbq_unlock(&tb->btb_q);

	M0_POST(!tb->btb_the_end);   /* not taking the lock is intentional */
	return true;
}

M0_INTERNAL void m0_be_tx_bulk_end(struct m0_be_tx_bulk *tb)
{
	bool tx_open_failed;

	be_tx_bulk_lock(tb);
	M0_ASSERT(!tb->btb_the_end);
	tb->btb_the_end = true;
	tx_open_failed = tb->btb_tx_open_failed;
	be_tx_bulk_unlock(tb);

	if (!tx_open_failed)
		be_tx_bulk_workers_terminate(tb, NULL, false);
}

M0_INTERNAL int m0_be_tx_bulk_status(struct m0_be_tx_bulk *tb)
{
	int rc;

	be_tx_bulk_lock(tb);
	M0_PRE(tb->btb_done);
	rc = tb->btb_rc;
	be_tx_bulk_unlock(tb);
	return rc;
}

#undef M0_TRACE_SUBSYSTEM

/** @} end of be group */

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
