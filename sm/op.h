/* -*- C -*- */
/*
 * Copyright (c) 2011-2020 Seagate Technology LLC and/or its Affiliates
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

#pragma once

#ifndef __MOTR___SM_OP_H__
#define __MOTR___SM_OP_H__

/**
 * @defgroup sm
 *
 * @{
 */

#include "lib/tlist.h"
#include "lib/chan.h"
#include "sm/sm.h"

struct m0_fom;

struct m0_sm_op;
struct m0_sm_op_ops;
struct m0_sm_op_exec;

enum m0_sm_op_state {
	M0_SOS_INIT,
	M0_SOS_SUBO,
	M0_SOS_DONE,

	M0_SOS_NR
};

enum {
	M0_SMOP_STACK_LEN = 4,
	M0_SMOP_WAIT_BIT  = 50,
	M0_SMOP_SAME_BIT,
	M0_SMOP_WAIT = M0_BITS(M0_SMOP_WAIT_BIT),
	M0_SMOP_SAME = M0_BITS(M0_SMOP_SAME_BIT)
};

struct m0_sm_op {
	uint64_t              o_magix;
	struct m0_sm          o_sm;
	struct m0_sm_op_exec *o_ceo;
	struct m0_tlink       o_linkage;
	struct m0_sm_op      *o_subo;
	int                   o_stack[M0_SMOP_STACK_LEN];
	bool                  o_finalise;
	int64_t             (*o_tick)(struct m0_sm_op *);
};

void     m0_sm_op_init(struct m0_sm_op *op, int64_t (*tick)(struct m0_sm_op *),
		       struct m0_sm_op_exec *ceo,
		       const struct m0_sm_conf *conf, struct m0_sm_group *grp);
void     m0_sm_op_fini(struct m0_sm_op *op);
bool     m0_sm_op_tick(struct m0_sm_op *op);
int64_t  m0_sm_op_prep(struct m0_sm_op *op, int state, struct m0_chan *chan);
int      m0_sm_op_sub (struct m0_sm_op *op, int state, int ret_state);
int      m0_sm_op_ret (struct m0_sm_op *op);
int      m0_sm_op_subo(struct m0_sm_op *op, struct m0_sm_op *subo, int state,
		       bool finalise);

void m0_sm_op_init_sub(struct m0_sm_op *op, int64_t (*tick)(struct m0_sm_op *),
		       struct m0_sm_op *super, const struct m0_sm_conf *conf);

struct m0_sm_op_exec_ops {
	bool    (*eo_is_armed)(struct m0_sm_op_exec *ceo);
	int64_t (*eo_prep)    (struct m0_sm_op_exec *ceo,
			       struct m0_sm_op *op, struct m0_chan *chan);
};

struct m0_sm_op_exec {
	struct m0_tl                    oe_op;
	const struct m0_sm_op_exec_ops *oe_vec;
};

void m0_sm_op_exec_init(struct m0_sm_op_exec *ceo);
void m0_sm_op_exec_fini(struct m0_sm_op_exec *ceo);

struct m0_fom_exec {
	struct m0_sm_op_exec  fe_ceo;
	struct m0_fom        *fe_fom;
};

void m0_fom_exec_init(struct m0_fom_exec *fe, struct m0_fom *fom);
void m0_fom_exec_fini(struct m0_fom_exec *fe);

struct m0_chan_exec {
	struct m0_sm_op_exec ce_ceo;
	struct m0_clink      ce_clink;
	struct m0_sm_op     *ce_top;
};

void m0_chan_exec_init(struct m0_chan_exec *ce, struct m0_sm_op *top);
void m0_chan_exec_fini(struct m0_chan_exec *ce);

struct m0_thread_exec {
	struct m0_sm_op_exec te_ceo;
};

void m0_thread_exec_init(struct m0_thread_exec *te);
void m0_thread_exec_fini(struct m0_thread_exec *te);

struct m0_ast_exec {
	struct m0_sm_op_exec ae_ceo;
	struct m0_sm_ast     ae_ast;
	struct m0_clink      ae_clink;
	struct m0_sm_group  *ae_grp;
	struct m0_sm_op     *ae_top;
	bool                 ae_armed;
};

void m0_ast_exec_init(struct m0_ast_exec *ae, struct m0_sm_op *top,
		      struct m0_sm_group *grp);
void m0_ast_exec_fini(struct m0_ast_exec *ae);

/** @} end of sm group */
#endif /* __MOTR___SM_OP_H__ */

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
