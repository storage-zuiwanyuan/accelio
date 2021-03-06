/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "xio_os.h"
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "libxio.h"
#include "xio_common.h"
#include "xio_observer.h"
#include "xio_context.h"
#include "xio_task.h"
#include "xio_transport.h"
#include "xio_protocol.h"
#include "get_clock.h"
#include "xio_mem.h"
#include "xio_rdma_transport.h"
#include "xio_rdma_utils.h"


/*---------------------------------------------------------------------------*/
/* forward declarations							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_req(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task);
static int xio_rdma_on_recv_rsp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task);
static int xio_rdma_on_setup_msg(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task);
static int xio_rdma_on_rsp_send_comp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task);
static int xio_rdma_on_req_send_comp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task);
static int xio_rdma_on_recv_nop(struct xio_rdma_transport *rdma_hndl,
				struct xio_task *task);
static int xio_rdma_on_recv_cancel_req(struct xio_rdma_transport *rdma_hndl,
				       struct xio_task *task);
static int xio_rdma_on_recv_cancel_rsp(struct xio_rdma_transport *rdma_hndl,
				       struct xio_task *task);
static int xio_rdma_send_nop(struct xio_rdma_transport *rdma_hndl);
static int xio_sched_rdma_wr_req(struct xio_rdma_transport *rdma_hndl,
				 struct xio_task *task);
static void xio_sched_consume_cq(xio_ctx_event_t *tev, void *data);
static void xio_sched_poll_cq(xio_ctx_event_t *tev, void *data);


/*---------------------------------------------------------------------------*/
/* xio_rdma_mr_lookup							     */
/*---------------------------------------------------------------------------*/
static inline struct ibv_mr *xio_rdma_mr_lookup(struct xio_mr *tmr,
				    struct xio_device *dev)
{
	struct xio_mr_elem *tmr_elem;

	list_for_each_entry(tmr_elem, &tmr->dm_list, dm_list_entry) {
		if (dev == tmr_elem->dev)
			return tmr_elem->mr;
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_post_recv							     */
/*---------------------------------------------------------------------------*/
int xio_post_recv(struct xio_rdma_transport *rdma_hndl,
			   struct xio_task *task, int num_recv_bufs)
{
	XIO_TO_RDMA_TASK(task, rdma_task);

	struct ibv_recv_wr	*bad_wr	= NULL;
	int			retval, nr_posted;

	retval = ibv_post_recv(rdma_hndl->qp, &rdma_task->rxd.recv_wr, &bad_wr);
	if (likely(!retval)) {
		nr_posted = num_recv_bufs;
	} else {
		struct ibv_recv_wr *wr;
			nr_posted = 0;
		for (wr = &rdma_task->rxd.recv_wr; wr != bad_wr; wr = wr->next)
			nr_posted++;

		xio_set_error(retval);
		ERROR_LOG("ibv_post_recv failed. (errno=%d %s)\n",
			  retval, strerror(retval));
	}
	rdma_hndl->rqe_avail += nr_posted;

	/* credit updates */
	rdma_hndl->credits += nr_posted;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_post_send                                                           */
/*---------------------------------------------------------------------------*/
static int xio_post_send(struct xio_rdma_transport *rdma_hndl,
			   struct xio_work_req *xio_send,
			   int num_send_reqs)
{
	struct ibv_send_wr	*bad_wr;
	int			retval, nr_posted;

	/*
	TRACE_LOG("num_sge:%d, len1:%d, len2:%d, send_flags:%d\n",
		  xio_send->send_wr.num_sge,
		  xio_send->send_wr.sg_list[0].length,
		  xio_send->send_wr.sg_list[1].length,
		  xio_send->send_wr.send_flags);
	*/

	retval = ibv_post_send(rdma_hndl->qp, &xio_send->send_wr, &bad_wr);
	if (likely(!retval)) {
		nr_posted = num_send_reqs;
	} else {
		struct ibv_send_wr *wr;

		nr_posted = 0;
		for (wr = &xio_send->send_wr; wr != bad_wr; wr = wr->next)
			nr_posted++;

		xio_set_error(retval);

		ERROR_LOG("ibv_post_send failed. (errno=%d %s)  posted:%d/%d " \
			  "sge_sz:%d, sqe_avail:%d\n", retval, strerror(retval),
			  nr_posted, num_send_reqs, xio_send->send_wr.num_sge,
			  rdma_hndl->sqe_avail);
	}
	rdma_hndl->sqe_avail -= nr_posted;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_write_sn							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_write_sn(struct xio_task *task,
			     uint16_t sn, uint16_t ack_sn, uint16_t credits)
{
	uint16_t *psn;

	/* save the current place */
	xio_mbuf_push(&task->mbuf);
	/* goto to the first tlv */
	xio_mbuf_reset(&task->mbuf);
	/* goto the first transport header*/
	xio_mbuf_set_trans_hdr(&task->mbuf);

	/* jump over the first uint32_t */
	xio_mbuf_inc(&task->mbuf, sizeof(uint32_t));

	/* and set serial number */
	psn = xio_mbuf_get_curr_ptr(&task->mbuf);
	*psn = htons(sn);

	xio_mbuf_inc(&task->mbuf, sizeof(uint16_t));

	/* and set ack serial number */
	psn = xio_mbuf_get_curr_ptr(&task->mbuf);
	*psn = htons(ack_sn);

	xio_mbuf_inc(&task->mbuf, sizeof(uint16_t));

	/* and set credits */
	psn = xio_mbuf_get_curr_ptr(&task->mbuf);
	*psn = htons(credits);

	/* pop to the original place */
	xio_mbuf_pop(&task->mbuf);

	return 0;
}

static inline uint16_t tx_window_sz(struct xio_rdma_transport *rdma_hndl)
{
	return rdma_hndl->max_sn - rdma_hndl->sn;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_xmit							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_xmit(struct xio_rdma_transport *rdma_hndl)
{
	struct xio_task		*task = NULL, *task1, *task2;
	struct xio_rdma_task	*rdma_task = NULL;
	struct xio_rdma_task	*prev_rdma_task = NULL;
	struct xio_work_req	dummy_wr;
	struct xio_work_req	*first_wr = NULL;
	struct xio_work_req	*curr_wr = NULL;
	struct xio_work_req	*prev_wr = &dummy_wr;
	uint16_t		tx_window;
	uint16_t		window;
	uint16_t		retval;
	uint16_t		req_nr = 0;

	tx_window = tx_window_sz(rdma_hndl);
	window = min(rdma_hndl->peer_credits, tx_window);
	window = min(window, rdma_hndl->sqe_avail);
	/*
	TRACE_LOG("XMIT: tx_window:%d, peer_credits:%d, sqe_avail:%d\n",
		  tx_window,
		  rdma_hndl->peer_credits,
		  rdma_hndl->sqe_avail);
	*/
	if (window == 0) {
		xio_set_error(EAGAIN);
		return -1;
	}


	/* if "ready to send queue" is not empty */
	while (rdma_hndl->tx_ready_tasks_num) {
		task = list_first_entry(
				&rdma_hndl->tx_ready_list,
				struct xio_task,  tasks_list_entry);

		rdma_task = task->dd_data;

		/* prefetch next buffer */
		if (rdma_hndl->tx_ready_tasks_num > 2) {
			task1 = list_first_entry_or_null(
					&task->tasks_list_entry,
					struct xio_task,  tasks_list_entry);
			if (task1) {
				xio_prefetch(task1->mbuf.buf.head);
				task2 = list_first_entry_or_null(
						&task1->tasks_list_entry,
						struct xio_task,
						tasks_list_entry);
				if (task2)
					xio_prefetch(task2->mbuf.buf.head);
			}
		}

		/* phantom task */
		if (rdma_task->phantom_idx) {
			if (req_nr >= window)
				break;
			curr_wr = &rdma_task->rdmad;

			prev_wr->send_wr.next = &curr_wr->send_wr;

			prev_rdma_task	= rdma_task;
			prev_wr		= curr_wr;
			req_nr++;
			rdma_hndl->tx_ready_tasks_num--;

			rdma_task->txd.send_wr.send_flags &= ~IBV_SEND_SIGNALED;

			list_move_tail(&task->tasks_list_entry,
				       &rdma_hndl->in_flight_list);
			continue;
		}
		if (rdma_task->ib_op == XIO_IB_RDMA_WRITE) {
			if (req_nr >= (window - 1))
				break;

			curr_wr = &rdma_task->rdmad;
			/* prepare it for rdma wr and concatenate the send
			 * wr to it */
			rdma_task->rdmad.send_wr.next = &rdma_task->txd.send_wr;
			rdma_task->txd.send_wr.send_flags |= IBV_SEND_SIGNALED;

			curr_wr = &rdma_task->rdmad;
			req_nr++;
		} else {
			if (req_nr >= window)
				break;
			curr_wr = &rdma_task->txd;
		}
		xio_rdma_write_sn(task, rdma_hndl->sn, rdma_hndl->ack_sn,
				  rdma_hndl->credits);
		rdma_task->sn = rdma_hndl->sn;
		rdma_hndl->sn++;
		rdma_hndl->sim_peer_credits += rdma_hndl->credits;
		rdma_hndl->credits = 0;
		rdma_hndl->peer_credits--;
		rdma_hndl->last_send_was_signaled =
			rdma_task->txd.send_wr.send_flags & IBV_SEND_SIGNALED;

		prev_wr->send_wr.next = &curr_wr->send_wr;
		prev_wr = &rdma_task->txd;

		prev_rdma_task = rdma_task;
		req_nr++;
		rdma_hndl->tx_ready_tasks_num--;
		if (IS_REQUEST(task->tlv_type))
			rdma_hndl->reqs_in_flight_nr++;
		else
			rdma_hndl->rsps_in_flight_nr++;
		list_move_tail(&task->tasks_list_entry,
			       &rdma_hndl->in_flight_list);
	}
	if (req_nr) {
		first_wr = container_of(dummy_wr.send_wr.next,
					struct xio_work_req, send_wr);
		prev_rdma_task->txd.send_wr.next = NULL;
		if (tx_window_sz(rdma_hndl) < 1 ||
		    rdma_hndl->sqe_avail < req_nr + 1)
			prev_rdma_task->txd.send_wr.send_flags |=
				IBV_SEND_SIGNALED;
		retval = xio_post_send(rdma_hndl, first_wr, req_nr);
		if (retval != 0) {
			ERROR_LOG("xio_post_send failed\n");
			return -1;
		}
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_xmit_rdma_rd							     */
/*---------------------------------------------------------------------------*/
static int xio_xmit_rdma_rd(struct xio_rdma_transport *rdma_hndl)
{
	struct xio_task		*task = NULL;
	struct xio_rdma_task	*rdma_task = NULL;
	struct xio_work_req	*first_wr = NULL;
	struct xio_work_req	dummy_wr;
	struct xio_work_req	*prev_wr = &dummy_wr;
	struct xio_work_req	*curr_wr = NULL;
	int num_reqs = 0;
	int err;


	while (!list_empty(&rdma_hndl->rdma_rd_list) &&
	       rdma_hndl->sqe_avail > num_reqs) {

		task = list_first_entry(
				&rdma_hndl->rdma_rd_list,
				struct xio_task,  tasks_list_entry);
		list_move_tail(&task->tasks_list_entry,
			       &rdma_hndl->rdma_rd_in_flight_list);
		rdma_task = task->dd_data;

		/* pending "sends" that were delayed for rdma read completion
		 *  are moved to wait in the in_flight list
		 *   because of the need to keep order
		 */
		if (rdma_task->ib_op == XIO_IB_RECV) {
			rdma_hndl->rdma_in_flight++;
			continue;
		}

		/* prepare it for rdma read */
		curr_wr = &rdma_task->rdmad;
		prev_wr->send_wr.next = &curr_wr->send_wr;
		prev_wr = &rdma_task->rdmad;

		num_reqs++;
	}

	rdma_hndl->kick_rdma_rd = 0;
	if (num_reqs) {
		first_wr = container_of(dummy_wr.send_wr.next,
					struct xio_work_req, send_wr);
		prev_wr->send_wr.next = NULL;
		rdma_hndl->rdma_in_flight += num_reqs;
		/* submit the chain of rdma-rd requests, start from the first */
		err = xio_post_send(rdma_hndl, first_wr, num_reqs);
		if (err)
			ERROR_LOG("xio_post_send failed\n");

		/* ToDo: error handling */
	} else if (!list_empty(&rdma_hndl->rdma_rd_list)) {
		rdma_hndl->kick_rdma_rd = 1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_rearm_rq							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_rearm_rq(struct xio_rdma_transport *rdma_hndl)
{
	struct xio_task		*first_task = NULL;
	struct xio_task		*task = NULL;
	struct xio_task		*prev_task = NULL;
	struct xio_rdma_task	*rdma_task = NULL;
	struct xio_rdma_task	*prev_rdma_task = NULL;
	int			num_to_post;
	int			i;

	num_to_post = rdma_hndl->actual_rq_depth - rdma_hndl->rqe_avail;
	for (i = 0; i < num_to_post; i++) {
		/* get ready to receive message */
		task = xio_rdma_primary_task_alloc(rdma_hndl);
		if (task == 0) {
			ERROR_LOG("primary task pool is empty\n");
			return -1;
		}
		rdma_task = task->dd_data;
		if (first_task == NULL)
			first_task = task;
		else
			prev_rdma_task->rxd.recv_wr.next =
						&rdma_task->rxd.recv_wr;

		prev_task = task;
		prev_rdma_task = rdma_task;
		rdma_task->ib_op = XIO_IB_RECV;
		list_add_tail(&task->tasks_list_entry, &rdma_hndl->rx_list);
	}
	if (prev_task) {
		prev_rdma_task->rxd.recv_wr.next = NULL;
		xio_post_recv(rdma_hndl, first_task, num_to_post);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_rx_error_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_rx_error_handler(struct xio_rdma_transport *rdma_hndl,
	      struct xio_task *task)
{
	/* remove the task from rx list */
	xio_tasks_pool_put(task);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_tx_error_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_tx_error_handler(struct xio_rdma_transport *rdma_hndl,
	      struct xio_task *task)
{
	/* remove the task from in-flight list */
	xio_tasks_pool_put(task);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_rd_error_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_rd_error_handler(struct xio_rdma_transport *rdma_hndl,
	      struct xio_task *task)
{
	/* remove the task from rdma rd in-flight list */
	xio_tasks_pool_put(task);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_wr_error_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_wr_error_handler(struct xio_rdma_transport *rdma_hndl,
	      struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);

	/* wait for the concatenated "send" */
	rdma_task->ib_op = XIO_IB_SEND;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_handle_wc_error                                                       */
/*---------------------------------------------------------------------------*/
static void xio_handle_task_error(struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_rdma_transport       *rdma_hndl = rdma_task->rdma_hndl;

	switch (rdma_task->ib_op) {
	case XIO_IB_RECV:
		/* this should be the Flush, no task has been created yet */
		xio_rdma_rx_error_handler(rdma_hndl, task);
		break;
	case XIO_IB_SEND:
		/* the task should be completed now */
		xio_rdma_tx_error_handler(rdma_hndl, task);
		break;
	case XIO_IB_RDMA_READ:
		xio_rdma_rd_error_handler(rdma_hndl, task);
		break;
	case XIO_IB_RDMA_WRITE:
		xio_rdma_wr_error_handler(rdma_hndl, task);
		break;
	default:
		ERROR_LOG("unknown opcode: task:%p, type:0x%x, " \
			  "magic:0x%lx, ib_op:0x%x\n",
			  task, task->tlv_type,
			  task->magic, rdma_task->ib_op);
		break;
	}
}

/*---------------------------------------------------------------------------*/
/* xio_handle_wc_error                                                       */
/*---------------------------------------------------------------------------*/
static void xio_handle_wc_error(struct ibv_wc *wc)
{
	struct xio_task			*task = ptr_from_int64(wc->wr_id);
	struct xio_rdma_task		*rdma_task = NULL;
	struct xio_rdma_transport       *rdma_hndl = NULL;
	int				retval;


	if (task) {
		rdma_task = task->dd_data;
		rdma_hndl = rdma_task->rdma_hndl;
	}

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		TRACE_LOG("rdma_hndl:%p, rdma_task:%p, task:%p, " \
			  "wr_id:0x%lx, " \
			  "err:%s, vendor_err:0x%x\n",
			   rdma_hndl, rdma_task, task,
			   wc->wr_id,
			   ibv_wc_status_str(wc->status),
			   wc->vendor_err);
	} else  {
		if (rdma_hndl)
			ERROR_LOG("[%s] - state:%d, rdma_hndl:%p, rdma_task:%p, "  \
			  "task:%p, wr_id:0x%lx, " \
			  "err:%s, vendor_err:0x%x\n",
			  rdma_hndl->base.is_client ? "client" : "server",
			  rdma_hndl->state,
			  rdma_hndl, rdma_task, task,
			  wc->wr_id,
			  ibv_wc_status_str(wc->status),
			  wc->vendor_err);
		else
			ERROR_LOG("wr_id:0x%lx, err:%s, vendor_err:0x%x\n",
				  wc->wr_id,
				  ibv_wc_status_str(wc->status),
				  wc->vendor_err);


		ERROR_LOG("byte_len=%u, immdata=%u, qp_num=0x%x, src_qp=0x%x\n",
			  wc->byte_len, wc->imm_data, wc->qp_num, wc->src_qp);
	}

	if (task)
		xio_handle_task_error(task);

	/* temporary  */
	if (wc->status != IBV_WC_WR_FLUSH_ERR) {
		if (rdma_hndl) {
			ERROR_LOG("connection is disconnected\n");
			rdma_hndl->state = XIO_STATE_DISCONNECTED;
			retval = rdma_disconnect(rdma_hndl->cm_id);
			if (retval)
				ERROR_LOG("rdma_hndl:%p rdma_disconnect" \
					  "failed, %m\n", rdma_hndl);
		} else {
			/* TODO: handle each error specifically */
			ERROR_LOG("ASSERT: program abort\n");
			exit(0);
		}
	}
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_idle_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_idle_handler(struct xio_rdma_transport *rdma_hndl)
{
	if (rdma_hndl->state != XIO_STATE_CONNECTED)
		return 0;

	/* Does the local have resources to send message?  */
	if (!rdma_hndl->sqe_avail)
		return 0;

	/* Try to do some useful work, want to spend time before calling the
	 * pool, this increase the chance that more messages will arrive
	 * and request notify will not be necessary
	 */

	if (rdma_hndl->kick_rdma_rd)
		xio_xmit_rdma_rd(rdma_hndl);

	/* Does the local have resources to send message?
	 * xio_xmit_rdma_rd may consumed the sqe_avail
	 */
	if (!rdma_hndl->sqe_avail)
		return 0;

	/* Can the peer receive messages? */
	if (!rdma_hndl->peer_credits)
		return 0;

	/* If we have real messages to send there is no need for
	 * a special NOP message as credits are piggybacked
	 */
	if (rdma_hndl->tx_ready_tasks_num) {
		xio_rdma_xmit(rdma_hndl);
		return 0;
	}

	/* Send NOP if messages are not queued */

#if 0
	if ((rdma_hndl->reqs_in_flight_nr || rdma_hndl->rsps_in_flight_nr) &&
	    !rdma_hndl->last_send_was_signaled)
		goto send_final_nop;
#endif
	/* Does the peer have already maximum credits? */
	if (rdma_hndl->sim_peer_credits >= MAX_RECV_WR)
		return 0;

	/* Does the local have any credits to send? */
	if (!rdma_hndl->credits)
		return 0;
#if 0
send_final_nop:
#endif
	TRACE_LOG("peer_credits:%d, credits:%d sim_peer_credits:%d\n",
		  rdma_hndl->peer_credits, rdma_hndl->credits,
		  rdma_hndl->sim_peer_credits);

	rdma_hndl->last_send_was_signaled = 0;

	xio_rdma_send_nop(rdma_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_rx_handler							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_rx_handler(struct xio_rdma_transport *rdma_hndl,
			       struct xio_task *task)
{
	struct xio_task		*task1, *task2;
	struct xio_rdma_task	*rdma_task;
	int			must_send = 0;
	int			retval;

	/* prefetch next buffer */
	task1 = list_first_entry(&task->tasks_list_entry,
			 struct xio_task,  tasks_list_entry);
	xio_prefetch(task1->mbuf.buf.head);
	task2 = list_first_entry(&task1->tasks_list_entry,
			 struct xio_task,  tasks_list_entry);
	xio_prefetch(task2->mbuf.buf.head);


	rdma_hndl->rqe_avail--;
	rdma_hndl->sim_peer_credits--;

	/* rearm the receive queue  */
	if ((rdma_hndl->state == XIO_STATE_CONNECTED) &&
	    (rdma_hndl->rqe_avail <= rdma_hndl->rq_depth + 1))
		xio_rdma_rearm_rq(rdma_hndl);

	retval = xio_mbuf_read_first_tlv(&task->mbuf);

	task->tlv_type = xio_mbuf_tlv_type(&task->mbuf);
	list_move_tail(&task->tasks_list_entry, &rdma_hndl->io_list);


	/* call recv completion  */
	switch (task->tlv_type) {
	case XIO_CREDIT_NOP:
		xio_rdma_on_recv_nop(rdma_hndl, task);
		break;
	case XIO_CONN_SETUP_REQ:
	case XIO_CONN_SETUP_RSP:
		xio_rdma_on_setup_msg(rdma_hndl, task);
		break;
	case XIO_CANCEL_REQ:
		xio_rdma_on_recv_cancel_req(rdma_hndl, task);
		break;
	case XIO_CANCEL_RSP:
		xio_rdma_on_recv_cancel_rsp(rdma_hndl, task);
		break;
	default:
		if (IS_REQUEST(task->tlv_type))
			xio_rdma_on_recv_req(rdma_hndl, task);
		else if (IS_RESPONSE(task->tlv_type))
			xio_rdma_on_recv_rsp(rdma_hndl, task);
		else
			ERROR_LOG("unknown message type:0x%x\n",
				  task->tlv_type);
		break;
	}

	if (rdma_hndl->state != XIO_STATE_CONNECTED)
		return retval;

	/* transmit ready packets */
	if (rdma_hndl->tx_ready_tasks_num) {
		rdma_task = task->dd_data;
		must_send = (tx_window_sz(rdma_hndl) >= SEND_TRESHOLD);
		must_send |= (rdma_task->more_in_batch == 0);
	}
	/* resource are now available and rdma rd  requests are pending kick
	 * them
	 */
	if (rdma_hndl->kick_rdma_rd)
		xio_xmit_rdma_rd(rdma_hndl);


	if (must_send)
		xio_rdma_xmit(rdma_hndl);

	return retval;
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_tx_comp_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_tx_comp_handler(struct xio_rdma_transport *rdma_hndl,
	      struct xio_task *task)
{
	struct xio_task		*ptask, *next_ptask;
	struct xio_rdma_task	*rdma_task;
	int			found = 0;
	int			removed = 0;


	list_for_each_entry_safe(ptask, next_ptask, &rdma_hndl->in_flight_list,
				 tasks_list_entry) {
		list_move_tail(&ptask->tasks_list_entry,
			       &rdma_hndl->tx_comp_list);
		removed++;
		rdma_task = ptask->dd_data;

		rdma_hndl->sqe_avail++;

		/* phantom task */
		if (rdma_task->phantom_idx) {
			xio_tasks_pool_put(ptask);
			continue;
		}
		/* rdma wr utilizes two wqe but appears only once in the
		 * in flight list
		 */
		if (rdma_task->ib_op == XIO_IB_RDMA_WRITE)
			rdma_hndl->sqe_avail++;

		if (IS_REQUEST(ptask->tlv_type)) {
			rdma_hndl->max_sn++;
			rdma_hndl->reqs_in_flight_nr--;
			xio_rdma_on_req_send_comp(rdma_hndl, ptask);
			xio_tasks_pool_put(ptask);
		} else if (IS_RESPONSE(ptask->tlv_type)) {
			rdma_hndl->max_sn++;
			rdma_hndl->rsps_in_flight_nr--;
			xio_rdma_on_rsp_send_comp(rdma_hndl, ptask);
		} else if (IS_NOP(ptask->tlv_type)) {
			rdma_hndl->rsps_in_flight_nr--;
			xio_tasks_pool_put(ptask);
		} else {
			ERROR_LOG("unexpected task %p type:0x%x id:%d " \
				  "magic:0x%lx\n",
				  ptask, rdma_task->ib_op,
				  ptask->ltid, ptask->magic);
			continue;
		}
		if (ptask == task) {
			found  = 1;
			break;
		}
	}
	/* resource are now available and rdma rd  requests are pending kick
	 * them
	 */
	if (rdma_hndl->kick_rdma_rd)
		xio_xmit_rdma_rd(rdma_hndl);


	if (rdma_hndl->tx_ready_tasks_num)
		xio_rdma_xmit(rdma_hndl);


	if (!found && removed)
		ERROR_LOG("not found but removed %d type:0x%x\n",
			  removed, task->tlv_type);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_rd_comp_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_rd_comp_handler(
		struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	union xio_transport_event_data	event_data;
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_transport_base	*transport =
					(struct xio_transport_base *)rdma_hndl;

	rdma_hndl->rdma_in_flight--;
	rdma_hndl->sqe_avail++;

	if (rdma_task->phantom_idx == 0) {
		if (task->state == XIO_TASK_STATE_CANCEL_PENDING) {
			TRACE_LOG("[%d] - **** message is canceled\n",
				  rdma_task->sn);
			xio_rdma_cancel_rsp(transport, task, XIO_E_MSG_CANCELED,
					    NULL, 0);
			xio_tasks_pool_put(task);
			xio_xmit_rdma_rd(rdma_hndl);
			return;
		}

		list_move_tail(&task->tasks_list_entry, &rdma_hndl->io_list);

		xio_xmit_rdma_rd(rdma_hndl);

		/* fill notification event */
		event_data.msg.op		= XIO_WC_OP_RECV;
		event_data.msg.task		= task;

		xio_transport_notify_observer(&rdma_hndl->base,
					      XIO_TRANSPORT_NEW_MESSAGE,
					      &event_data);

		while (rdma_hndl->rdma_in_flight) {
			task = list_first_entry(
					&rdma_hndl->rdma_rd_in_flight_list,
					struct xio_task,  tasks_list_entry);

			rdma_task = task->dd_data;

			if (rdma_task->ib_op != XIO_IB_RECV)
				break;

			/* tasks that arrived in Send/Receive while pending
			 * "RDMA READ" tasks were in flight was fenced.
			 */
			rdma_hndl->rdma_in_flight--;
			list_move_tail(&task->tasks_list_entry,
				       &rdma_hndl->io_list);
			event_data.msg.op	= XIO_WC_OP_RECV;
			event_data.msg.task	= task;

			xio_transport_notify_observer(&rdma_hndl->base,
						 XIO_TRANSPORT_NEW_MESSAGE,
						 &event_data);
		}
	} else {
		xio_tasks_pool_put(task);
		xio_xmit_rdma_rd(rdma_hndl);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_wr_comp_handler						     */
/*---------------------------------------------------------------------------*/
static inline void xio_rdma_wr_comp_handler(
		struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
}

/*---------------------------------------------------------------------------*/
/* xio_handle_wc							     */
/*---------------------------------------------------------------------------*/
static inline void xio_handle_wc(struct ibv_wc *wc, int has_more)
{
	struct xio_task			*task = ptr_from_int64(wc->wr_id);
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_rdma_transport	*rdma_hndl = rdma_task->rdma_hndl;

	/*
	TRACE_LOG("received opcode :%s [%x]\n",
		  ibv_wc_opcode_str(wc->opcode), wc->opcode);
	*/

	switch (wc->opcode) {
	case IBV_WC_RECV:
		rdma_task->more_in_batch = has_more;
		xio_rdma_rx_handler(rdma_hndl, task);
		break;
	case IBV_WC_SEND:
		xio_rdma_tx_comp_handler(rdma_hndl, task);
		break;
	case IBV_WC_RDMA_READ:
		xio_rdma_rd_comp_handler(rdma_hndl, task);
		break;
	case IBV_WC_RDMA_WRITE:
		xio_rdma_wr_comp_handler(rdma_hndl, task);
		break;
	default:
		ERROR_LOG("unknown opcode :%s [%x]\n",
			  ibv_wc_opcode_str(wc->opcode), wc->opcode);
		break;
	}
}

/*
 * Could read as many entries as possible without blocking, but
 * that just fills up a list of tasks.  Instead pop out of here
 * so that tx progress, like issuing rdma reads and writes, can
 * happen periodically.
 */
static int xio_poll_cq(struct xio_cq *tcq, int max_wc, int timeout_us)
{
	int		err = 0;
	int		wclen = max_wc, i, numwc  = 0;
	int		last_recv = -1;
	int		timeouts_num = 0;
	int		polled = 0;
	cycles_t	timeout;
	cycles_t	start_time = 0;

	for (;;) {
		if (wclen > tcq->wc_array_len)
			wclen = tcq->wc_array_len;

		if (xio_context_is_loop_stopping(tcq->ctx) && polled) {
				err = 1; /* same as in budget */
				break;
		}
		err = ibv_poll_cq(tcq->cq, wclen, tcq->wc_array);
		polled = 1;
		if (err == 0) { /* no completions retrieved */
			if (timeout_us == 0)
				break;
			/* wait timeout before going out */
			if (timeouts_num == 0) {
				start_time = get_cycles();
			} else {
				/*calculate it again, need to spend time */
				timeout = timeout_us*g_mhz;
				if (timeout_us > 0 &&
				    (get_cycles() - start_time) > timeout)
					break;
			}
			if (xio_context_is_loop_stopping(tcq->ctx)) {
				err = 1; /* same as in budget */
				break;
			}

			timeouts_num++;
			continue;
		}

		if (unlikely(err < 0)) {
			ERROR_LOG("ibv_poll_cq failed\n");
			break;
		}
		timeouts_num = 0;
		for (i = err; i > 0; i--) {
			if (tcq->wc_array[i-1].opcode == IBV_WC_RECV) {
				last_recv = i-1;
				break;
			}
		}
		for (i = 0; i < err; i++) {
			if (likely(tcq->wc_array[i].status == IBV_WC_SUCCESS))
				xio_handle_wc(&tcq->wc_array[i],
					      (i != last_recv));
			else
				xio_handle_wc_error(
						&tcq->wc_array[i]);
		}
		numwc += err;
		if (numwc == max_wc) {
			err = 1;
			break;
		}
		wclen = max_wc - numwc;
	}

	return err;
}

/*---------------------------------------------------------------------------*/
/* xio_rearm_completions						     */
/*---------------------------------------------------------------------------*/
static void xio_rearm_completions(struct xio_cq *tcq)
{
	int err;

	err = ibv_req_notify_cq(tcq->cq, 0);
	if (unlikely(err)) {
		ERROR_LOG("ibv_req_notify_cq failed. (errno=%d %m)\n",
			  errno);
	}

	xio_ctx_init_event(&tcq->event_data,
			   xio_sched_consume_cq, tcq);
	xio_ctx_add_event(tcq->ctx, &tcq->event_data);

	tcq->num_delayed_arm = 0;
}

/*---------------------------------------------------------------------------*/
/* xio_poll_cq_armable							     */
/*---------------------------------------------------------------------------*/
static void xio_poll_cq_armable(struct xio_cq *tcq)
{
	int err;

	err = xio_poll_cq(tcq, MAX_POLL_WC, tcq->ctx->polling_timeout);
	if (unlikely(err < 0)) {
		xio_rearm_completions(tcq);
		return;
	}

	if (err == 0 && (++tcq->num_delayed_arm == MAX_NUM_DELAYED_ARM))
		/* no more completions on cq, give up and arm the interrupts */
		xio_rearm_completions(tcq);
	else {
		xio_ctx_init_event(&tcq->event_data,
				   xio_sched_poll_cq, tcq);
		xio_ctx_add_event(tcq->ctx, &tcq->event_data);
	}
}

/* xio_sched_consume_cq() is scheduled to consume completion events that
   could arrive after the cq had been seen empty, but just before
   the interrupts were re-armed.
   Intended to consume those remaining completions only, the function
   does not re-arm interrupts, but polls the cq until it's empty.
   As we always limit the number of completions polled at a time, we may
   need to schedule this functions few times.
   It may happen that during this process new completions occur, and
   we get an interrupt about that. Some of the "new" completions may be
   processed by the self-scheduling xio_sched_consume_cq(), which is
   a good thing, because we don't need to wait for the interrupt event.
   When the interrupt notification arrives, its handler will remove the
   scheduled event, and call xio_poll_cq_armable(), so that the polling
   cycle resumes normally.
*/
static void xio_sched_consume_cq(xio_ctx_event_t *tev, void *data)
{
	struct xio_cq *tcq = data;
	int err;

	err = xio_poll_cq(tcq, MAX_POLL_WC, tcq->ctx->polling_timeout);
	if (err > 0) {
		xio_ctx_init_event(&tcq->event_data,
				   xio_sched_consume_cq, tcq);
		xio_ctx_add_event(tcq->ctx, &tcq->event_data);
	}
}

/* Scheduled to poll cq after a completion event has been
   received and acknowledged, if no more completions are found
   the interrupts are re-armed */
static void xio_sched_poll_cq(xio_ctx_event_t *tev, void *data)
{
	struct xio_rdma_transport	*rdma_hndl;
	struct xio_cq			*tcq = data;

	xio_poll_cq_armable(tcq);

	list_for_each_entry(rdma_hndl, &tcq->trans_list, trans_list_entry) {
		xio_rdma_idle_handler(rdma_hndl);
	}
}

/*
 * Called from main event loop when a CQ notification is available.
 */
void xio_cq_event_handler(int fd  __attribute__ ((unused)),
			  int events __attribute__ ((unused)),
			  void *data)
{
	void				*cq_context;
	struct ibv_cq			*cq;
	struct xio_cq			*tcq = data;
	int				err;

	err = ibv_get_cq_event(tcq->channel, &cq, &cq_context);
	if (unlikely(err != 0)) {
		/* Just print the log message, if that was a serious problem,
		   it will express itself elsewhere */
		ERROR_LOG("failed to retrieve CQ event, cq:%p\n", cq);
		return;
	}
	/* accumulate number of cq events that need to
	 * be acked, and periodically ack them
	 */
	if (++tcq->cq_events_that_need_ack == 128/*UINT_MAX*/) {
		ibv_ack_cq_events(tcq->cq, 128/*UINT_MAX*/);
		tcq->cq_events_that_need_ack = 0;
	}

	/* if a poll was previously scheduled, remove it,
	   as it will be scheduled when necessary */
	xio_ctx_remove_event(tcq->ctx, &tcq->event_data);

	xio_poll_cq_armable(tcq);
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_poll							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_poll(struct xio_transport_base *transport,
		  long min_nr, long max_nr,
		  struct timespec *ts_timeout)
{
	int				retval;
	int				i;
	struct xio_rdma_transport	*rdma_hndl;
	struct xio_cq			*tcq;
	int				last_recv = -1;
	int				nr_comp = 0, recv_counter;
	int				nr;
	cycles_t			timeout = -1;
	cycles_t			start_time = get_cycles();

	if (min_nr > max_nr)
		return -1;

	if (ts_timeout)
		timeout = timespec_to_usecs(ts_timeout)*g_mhz;


	rdma_hndl = (struct xio_rdma_transport *)transport;
	tcq = rdma_hndl->tcq;

	while (1) {
		nr = min(max_nr, tcq->wc_array_len);
		retval = ibv_poll_cq(tcq->cq, nr, tcq->wc_array);
		if (likely(retval > 0)) {
			for (i = retval; i > 0; i--) {
				if (tcq->wc_array[i-1].opcode == IBV_WC_RECV) {
					last_recv = i-1;
					break;
				}
			}
			recv_counter = 0;
			for (i = 0; i < retval; i++) {
				if (tcq->wc_array[i].opcode == IBV_WC_RECV)
					recv_counter++;
					if (rdma_hndl->tcq->wc_array[i].status ==
				    IBV_WC_SUCCESS)
					xio_handle_wc(&tcq->wc_array[i],
						      (i != last_recv));
				else
					xio_handle_wc_error(&tcq->wc_array[i]);
			}
			nr_comp += recv_counter;
			max_nr -= recv_counter;
			if (nr_comp >= min_nr || max_nr == 0)
				break;
			if ((get_cycles() - start_time) >= timeout)
				break;
		} else if (retval == 0) {
			if ((get_cycles() - start_time) >= timeout)
				break;
		} else {
			ERROR_LOG("ibv_poll_cq failed. (errno=%d %m)\n", errno);
			xio_set_error(errno);
			return -1;
		}
	}

	/*
	retval = ibv_req_notify_cq(tcq->cq, 0);
	if (unlikely(retval)) {
		errno = retval;
		xio_set_error(errno);
		ERROR_LOG("ibv_req_notify_cq failed. (errno=%d %m)\n",
			  errno);
		return -1;
	}
	*/

	return nr_comp;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_write_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_write_req_header(struct xio_rdma_transport *rdma_hndl,
				     struct xio_task *task,
				     struct xio_req_hdr *req_hdr)
{
	struct xio_req_hdr		*tmp_req_hdr;
	struct xio_sge			*tmp_sge;
	struct xio_sge			sge;
	size_t				hdr_len;
	struct ibv_mr			*mr;
	uint8_t				i;
	XIO_TO_RDMA_TASK(task, rdma_task);


	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_req_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	tmp_req_hdr->version  = req_hdr->version;
	tmp_req_hdr->flags    = req_hdr->flags;
	PACK_SVAL(req_hdr, tmp_req_hdr, req_hdr_len);
	/* sn		shall be coded later */
	/* ack_sn	shall be coded later */
	/* credits	shall be coded later */
	PACK_SVAL(req_hdr, tmp_req_hdr, tid);
	tmp_req_hdr->opcode	   = req_hdr->opcode;
	tmp_req_hdr->recv_num_sge  = req_hdr->recv_num_sge;
	tmp_req_hdr->read_num_sge  = req_hdr->read_num_sge;
	tmp_req_hdr->write_num_sge = req_hdr->write_num_sge;

	PACK_SVAL(req_hdr, tmp_req_hdr, ulp_hdr_len);
	PACK_SVAL(req_hdr, tmp_req_hdr, ulp_pad_len);
	/*remain_data_len is not used		*/
	PACK_LLVAL(req_hdr, tmp_req_hdr, ulp_imm_len);

	tmp_sge = (void *)((uint8_t *)tmp_req_hdr +
			   sizeof(struct xio_req_hdr));

	/* IN: requester expect small input written via send */
	for (i = 0;  i < req_hdr->recv_num_sge; i++) {
		sge.addr = 0;
		sge.length = task->omsg->in.data_iov[i].iov_len;
		sge.stag = 0;
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}
	/* IN: requester expect big input written rdma write */
	for (i = 0;  i < req_hdr->read_num_sge; i++) {
		sge.addr = uint64_from_ptr(rdma_task->read_sge[i].addr);
		sge.length  = rdma_task->read_sge[i].length;
		if (rdma_task->read_sge[i].mr) {
			mr = xio_rdma_mr_lookup(
					rdma_task->read_sge[i].mr,
					rdma_hndl->tcq->dev);
			if (!mr)
				goto cleanup;

			sge.stag	= mr->rkey;
		} else {
			sge.stag	= 0;
		}
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}
	/* OUT: requester want to write data via rdma read */
	for (i = 0;  i < req_hdr->write_num_sge; i++) {
		sge.addr = uint64_from_ptr(rdma_task->write_sge[i].addr);
		sge.length  = rdma_task->write_sge[i].length;
		if (rdma_task->write_sge[i].mr) {
			mr = xio_rdma_mr_lookup(
					rdma_task->write_sge[i].mr,
					rdma_hndl->tcq->dev);
			if (!mr)
				goto cleanup;

			sge.stag	= mr->rkey;
		} else {
			sge.stag	= 0;
		}
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}
	hdr_len	= sizeof(struct xio_req_hdr);
	hdr_len += sizeof(struct xio_sge)*(req_hdr->recv_num_sge +
					   req_hdr->read_num_sge +
					   req_hdr->write_num_sge);
#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.curr,
			     hdr_len + 16);
#endif
	xio_mbuf_inc(&task->mbuf, hdr_len);

	return 0;

cleanup:
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_read_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_read_req_header(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task,
		struct xio_req_hdr *req_hdr)
{
	struct xio_req_hdr		*tmp_req_hdr;
	struct xio_sge			*tmp_sge;
	int				i;
	size_t				hdr_len;
	XIO_TO_RDMA_TASK(task, rdma_task);

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_req_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);


	req_hdr->version  = tmp_req_hdr->version;
	req_hdr->flags    = tmp_req_hdr->flags;
	UNPACK_SVAL(tmp_req_hdr, req_hdr, req_hdr_len);

	if (req_hdr->req_hdr_len != sizeof(struct xio_req_hdr)) {
		ERROR_LOG(
		"header length's read failed. arrived:%d  expected:%zd\n",
		req_hdr->req_hdr_len, sizeof(struct xio_req_hdr));
		return -1;
	}
	UNPACK_SVAL(tmp_req_hdr, req_hdr, sn);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, credits);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, tid);
	req_hdr->opcode		= tmp_req_hdr->opcode;
	req_hdr->recv_num_sge	= tmp_req_hdr->recv_num_sge;
	req_hdr->read_num_sge	= tmp_req_hdr->read_num_sge;
	req_hdr->write_num_sge	= tmp_req_hdr->write_num_sge;

	UNPACK_SVAL(tmp_req_hdr, req_hdr, ulp_hdr_len);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, ulp_pad_len);

	/* remain_data_len not in use */
	UNPACK_LLVAL(tmp_req_hdr, req_hdr, ulp_imm_len);

	tmp_sge = (void *)((uint8_t *)tmp_req_hdr +
			   sizeof(struct xio_req_hdr));

	rdma_task->sn = req_hdr->sn;

	/* params for SEND */
	for (i = 0;  i < req_hdr->recv_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &rdma_task->req_recv_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_recv_sge[i], length);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_recv_sge[i], stag);
		tmp_sge++;
	}
	rdma_task->req_recv_num_sge	= i;

	/* params for RDMA_WRITE */
	for (i = 0;  i < req_hdr->read_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &rdma_task->req_read_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_read_sge[i], length);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_read_sge[i], stag);
		tmp_sge++;
	}
	rdma_task->req_read_num_sge	= i;

	/* params for RDMA_READ */
	for (i = 0;  i < req_hdr->write_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &rdma_task->req_write_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_write_sge[i], length);
		UNPACK_LVAL(tmp_sge, &rdma_task->req_write_sge[i], stag);
		tmp_sge++;
	}
	rdma_task->req_write_num_sge	= i;

	hdr_len	= sizeof(struct xio_req_hdr);
	hdr_len += sizeof(struct xio_sge)*(req_hdr->recv_num_sge +
					   req_hdr->read_num_sge +
					   req_hdr->write_num_sge);

	xio_mbuf_inc(&task->mbuf, hdr_len);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_write_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_write_rsp_header(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_rsp_hdr *rsp_hdr)
{
	struct xio_rsp_hdr		*tmp_rsp_hdr;

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_rsp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	tmp_rsp_hdr->version  = rsp_hdr->version;
	tmp_rsp_hdr->flags    = rsp_hdr->flags;
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, rsp_hdr_len);
	/* sn		shall be coded later */
	/* ack_sn	shall be coded later */
	/* credits	shall be coded later */
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, tid);
	tmp_rsp_hdr->opcode = rsp_hdr->opcode;
	PACK_LVAL(rsp_hdr, tmp_rsp_hdr, status);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, ulp_hdr_len);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, ulp_pad_len);
	/* remain_data_len not in use */
	PACK_LLVAL(rsp_hdr, tmp_rsp_hdr, ulp_imm_len);
	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_rsp_hdr));

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head, 64);
#endif
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_read_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_read_rsp_header(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_rsp_hdr *rsp_hdr)
{
	struct xio_rsp_hdr		*tmp_rsp_hdr;

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_rsp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	rsp_hdr->version  = tmp_rsp_hdr->version;
	rsp_hdr->flags    = tmp_rsp_hdr->flags;
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, rsp_hdr_len);

	if (rsp_hdr->rsp_hdr_len != sizeof(struct xio_rsp_hdr)) {
		ERROR_LOG(
		"header length's read failed. arrived:%d expected:%zd\n",
		  rsp_hdr->rsp_hdr_len, sizeof(struct xio_rsp_hdr));
		return -1;
	}

	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, sn);
	/* ack_sn not used */
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, credits);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, tid);
	rsp_hdr->opcode = tmp_rsp_hdr->opcode;
	UNPACK_LVAL(tmp_rsp_hdr, rsp_hdr, status);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, ulp_hdr_len);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, ulp_pad_len);
	/* remain_data_len not in use */
	UNPACK_LLVAL(tmp_rsp_hdr, rsp_hdr, ulp_imm_len);

	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_rsp_hdr));

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_prep_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_prep_req_header(struct xio_rdma_transport *rdma_hndl,
				    struct xio_task	*task,
				    uint16_t ulp_hdr_len,
				    uint16_t ulp_pad_len,
				    uint64_t ulp_imm_len,
				    uint32_t status)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_req_hdr	req_hdr;

	if (!IS_REQUEST(task->tlv_type)) {
		ERROR_LOG("unknown message type\n");
		return -1;
	}

	/* write the headers */

	/* fill request header */
	req_hdr.version		= XIO_REQ_HEADER_VERSION;
	req_hdr.req_hdr_len	= sizeof(req_hdr);
	req_hdr.tid		= task->ltid;
	req_hdr.opcode		= rdma_task->ib_op;
	req_hdr.flags		= 0;
	req_hdr.ulp_hdr_len	= ulp_hdr_len;
	req_hdr.ulp_pad_len	= ulp_pad_len;
	req_hdr.ulp_imm_len	= ulp_imm_len;
	req_hdr.recv_num_sge	= rdma_task->recv_num_sge;
	req_hdr.read_num_sge	= rdma_task->read_num_sge;
	req_hdr.write_num_sge	= rdma_task->write_num_sge;

	if (xio_rdma_write_req_header(rdma_hndl, task, &req_hdr) != 0)
		goto cleanup;

	/* write the payload header */
	if (ulp_hdr_len) {
		if (xio_mbuf_write_array(
		    &task->mbuf,
		    task->omsg->out.header.iov_base,
		    task->omsg->out.header.iov_len) != 0)
			goto cleanup;
	}

	/* write the pad between header and data */
	if (ulp_pad_len)
		xio_mbuf_inc(&task->mbuf, ulp_pad_len);

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_rdma_write_req_header failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_prep_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_prep_rsp_header(struct xio_rdma_transport *rdma_hndl,
				    struct xio_task *task,
				    uint16_t ulp_hdr_len,
				    uint16_t ulp_pad_len,
				    uint64_t ulp_imm_len,
				    uint32_t status)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_rsp_hdr	rsp_hdr;

	if (!IS_RESPONSE(task->tlv_type)) {
		ERROR_LOG("unknown message type\n");
		return -1;
	}

	/* fill response header */
	rsp_hdr.version		= XIO_RSP_HEADER_VERSION;
	rsp_hdr.rsp_hdr_len	= sizeof(rsp_hdr);
	rsp_hdr.tid		= task->rtid;
	rsp_hdr.opcode		= rdma_task->ib_op;
	rsp_hdr.flags		= 0;
	rsp_hdr.ulp_hdr_len	= ulp_hdr_len;
	rsp_hdr.ulp_pad_len	= ulp_pad_len;
	rsp_hdr.ulp_imm_len	= ulp_imm_len;
	rsp_hdr.status		= status;
	if (xio_rdma_write_rsp_header(rdma_hndl, task, &rsp_hdr) != 0)
		goto cleanup;

	/* write the payload header */
	if (ulp_hdr_len) {
		if (xio_mbuf_write_array(
		    &task->mbuf,
		    task->omsg->out.header.iov_base,
		    task->omsg->out.header.iov_len) != 0)
			goto cleanup;
	}

	/* write the pad between header and data */
	if (ulp_pad_len)
		xio_mbuf_inc(&task->mbuf, ulp_pad_len);

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_rdma_write_rsp_header failed\n");
	return -1;
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_write_send_data						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_write_send_data(
		struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	size_t			i;
	struct ibv_mr		*mr;

	/* user provided mr */
	if (task->omsg->out.data_iov[0].mr) {
		struct ibv_sge	*sge = &rdma_task->txd.sge[1];
		struct xio_iovec_ex *iov =
			&task->omsg->out.data_iov[0];
		for (i = 0; i < task->omsg->out.data_iovlen; i++)  {
			if (iov->mr == NULL) {
				ERROR_LOG("failed to find mr on iov\n");
				goto cleanup;
			}

			/* get the crresopnding key of the
			 * outgoing adapter */
			mr = xio_rdma_mr_lookup(iov->mr,
					rdma_hndl->tcq->dev);
			if (mr == NULL) {
				ERROR_LOG("failed to find memory " \
						"handle\n");
				goto cleanup;
			}
			/* copy the iovec */
			/* send it on registered memory */
			sge->addr    = uint64_from_ptr(iov->iov_base);
			sge->length  = (uint32_t)iov->iov_len;
			sge->lkey    = mr->lkey;
			iov++;
			sge++;
		}
		rdma_task->txd.send_wr.num_sge =
			task->omsg->out.data_iovlen + 1;
	} else {
		/* copy to internal buffer */
		for (i = 0; i < task->omsg->out.data_iovlen; i++) {
			/* copy the data into internal buffer */
			if (xio_mbuf_write_array(
				&task->mbuf,
				task->omsg->out.data_iov[i].iov_base,
				task->omsg->out.data_iov[i].iov_len) != 0)
				goto cleanup;
		}
		rdma_task->txd.send_wr.num_sge = 1;
	}

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_rdma_send_msg failed\n");
	return -1;
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_prep_req_out_data						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_prep_req_out_data(
		struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_vmsg		*vmsg = &task->omsg->out;
	uint64_t		xio_hdr_len;
	uint64_t		ulp_out_hdr_len;
	uint64_t		ulp_pad_len = 0;
	uint64_t		ulp_out_imm_len;
	size_t			retval;
	int			i;
	/*int			data_alignment = DEF_DATA_ALIGNMENT;*/

	/* calculate headers */
	ulp_out_hdr_len	= vmsg->header.iov_len;
	ulp_out_imm_len	= xio_iovex_length(vmsg->data_iov,
					   vmsg->data_iovlen);

	xio_hdr_len = xio_mbuf_get_curr_offset(&task->mbuf);
	xio_hdr_len += sizeof(struct xio_req_hdr);

	if (rdma_hndl->max_send_buf_sz	 < (xio_hdr_len + ulp_out_hdr_len)) {
		ERROR_LOG("header size %lu exceeds max header %lu\n",
			  ulp_out_hdr_len, rdma_hndl->max_send_buf_sz -
			  xio_hdr_len);
		xio_set_error(XIO_E_MSG_SIZE);
		return -1;
	}

	/* the data is outgoing via SEND */
	if ((ulp_out_hdr_len + ulp_out_imm_len +
	    MAX_HDR_SZ) < rdma_hndl->max_send_buf_sz) {
		/*
		if (data_alignment && ulp_out_imm_len) {
			uint16_t hdr_len = xio_hdr_len + ulp_out_hdr_len;
			ulp_pad_len = ALIGN(hdr_len, data_alignment) - hdr_len;
		}
		*/
		rdma_task->ib_op = XIO_IB_SEND;
		/* user has small request - no rdma operation expected */
		rdma_task->write_num_sge = 0;

		/* write xio header to the buffer */
		retval = xio_rdma_prep_req_header(
				rdma_hndl, task,
				ulp_out_hdr_len, ulp_pad_len, ulp_out_imm_len,
				XIO_E_SUCCESS);
		if (retval)
			return -1;

		/* if there is data, set it to buffer or directly to the sge */
		if (ulp_out_imm_len) {
			retval = xio_rdma_write_send_data(rdma_hndl, task);
			if (retval)
				return -1;
		}
	} else {
		/* the data is outgoing via SEND but the peer will do
		 * RDMA_READ */
		rdma_task->ib_op = XIO_IB_RDMA_READ;
		/* user provided mr */
		if (task->omsg->out.data_iov[0].mr) {
			for (i = 0; i < vmsg->data_iovlen; i++) {
				rdma_task->write_sge[i].addr =
					vmsg->data_iov[i].iov_base;
				rdma_task->write_sge[i].cache = NULL;
				rdma_task->write_sge[i].mr =
					task->omsg->out.data_iov[i].mr;

				rdma_task->write_sge[i].length =
					vmsg->data_iov[i].iov_len;
			}
		} else {
			if (rdma_hndl->rdma_mempool == NULL) {
				xio_set_error(XIO_E_NO_BUFS);
				ERROR_LOG(
					"message /read/write failed - " \
					"library's memory pool disabled\n");
				goto cleanup;
			}

			/* user did not provide mr - take buffers from pool
			 * and do copy */
			for (i = 0; i < vmsg->data_iovlen; i++) {
				retval = xio_mempool_alloc(
						rdma_hndl->rdma_mempool,
						vmsg->data_iov[i].iov_len,
						&rdma_task->write_sge[i]);
				if (retval) {
					rdma_task->write_num_sge = i;
					xio_set_error(ENOMEM);
					ERROR_LOG(
					"mempool is empty for %zd bytes\n",
					vmsg->data_iov[i].iov_len);
					goto cleanup;
				}

				rdma_task->write_sge[i].length =
					vmsg->data_iov[i].iov_len;

				/* copy the data to the buffer */
				memcpy(rdma_task->write_sge[i].addr,
				       vmsg->data_iov[i].iov_base,
				       vmsg->data_iov[i].iov_len);
			}
		}
		rdma_task->write_num_sge = vmsg->data_iovlen;

		/* write xio header to the buffer */
		retval = xio_rdma_prep_req_header(
				rdma_hndl, task,
				ulp_out_hdr_len, 0, 0, XIO_E_SUCCESS);

		if (retval) {
			ERROR_LOG("Failed to write header\n");
			goto cleanup;
		}
	}

	return 0;

cleanup:
	for (i = 0; i < rdma_task->write_num_sge; i++)
		xio_mempool_free(&rdma_task->write_sge[i]);

	rdma_task->write_num_sge = 0;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_prep_req_in_data						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_prep_req_in_data(
		struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	size_t				hdr_len;
	size_t				data_len;
	struct xio_vmsg			*vmsg = &task->omsg->in;
	int				i, retval;



	data_len  = xio_iovex_length(vmsg->data_iov, vmsg->data_iovlen);
	hdr_len  = vmsg->header.iov_len;

	/* requester may insist on RDMA for small buffers to eliminate copy
	 * from receive buffers to user buffers
	 */
	if (!(task->omsg_flags & XIO_MSG_FLAG_SMALL_ZERO_COPY) &&
	    data_len + hdr_len + MAX_HDR_SZ < rdma_hndl->max_send_buf_sz) {
		/* user has small response - no rdma operation expected */
		rdma_task->read_num_sge = 0;
		if (data_len) {
			rdma_task->recv_num_sge = vmsg->data_iovlen;
			rdma_task->read_num_sge = 0;
		}
	} else  {
		/* user provided buffers with length for RDMA WRITE */
		/* user provided mr */
		if (vmsg->data_iov[0].mr) {
			for (i = 0; i < vmsg->data_iovlen; i++) {
				rdma_task->read_sge[i].addr =
					vmsg->data_iov[i].iov_base;
				rdma_task->read_sge[i].cache = NULL;
				rdma_task->read_sge[i].mr =
					vmsg->data_iov[i].mr;

				rdma_task->read_sge[i].length =
					vmsg->data_iov[i].iov_len;

			}
		} else  {
			if (rdma_hndl->rdma_mempool == NULL) {
				xio_set_error(XIO_E_NO_BUFS);
				ERROR_LOG(
					"message /read/write failed - " \
					"library's memory pool disabled\n");
				goto cleanup;
			}

			/* user did not provide mr */
			for (i = 0; i < vmsg->data_iovlen; i++) {
				retval = xio_mempool_alloc(
						rdma_hndl->rdma_mempool,
						vmsg->data_iov[i].iov_len,
						&rdma_task->read_sge[i]);

				if (retval) {
					rdma_task->read_num_sge = i;
					xio_set_error(ENOMEM);
					ERROR_LOG(
					"mempool is empty for %zd bytes\n",
					vmsg->data_iov[i].iov_len);
					goto cleanup;
				}
				rdma_task->read_sge[i].length =
					vmsg->data_iov[i].iov_len;
			}
		}
		rdma_task->read_num_sge = vmsg->data_iovlen;
		rdma_task->recv_num_sge = 0;
	}

	return 0;

cleanup:
	for (i = 0; i < rdma_task->read_num_sge; i++)
		xio_mempool_free(&rdma_task->read_sge[i]);

	rdma_task->read_num_sge = 0;
	rdma_task->recv_num_sge = 0;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_req							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_req(struct xio_rdma_transport *rdma_hndl,
	struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	uint64_t		payload;
	size_t			retval;
	int			i;
	int			must_send = 0;
	size_t			sge_len;


	if (rdma_hndl->reqs_in_flight_nr + rdma_hndl->rsps_in_flight_nr >
	    rdma_hndl->max_tx_ready_tasks_num) {
		xio_set_error(EAGAIN);
		return -1;
	}

	if (rdma_hndl->reqs_in_flight_nr >=
			rdma_hndl->max_tx_ready_tasks_num - 1) {
		xio_set_error(EAGAIN);
		return -1;
	}
	/* tx ready is full - refuse request */
	if (rdma_hndl->tx_ready_tasks_num >=
			rdma_hndl->max_tx_ready_tasks_num) {
		xio_set_error(EAGAIN);
		return -1;
	}

	/* prepare buffer for RDMA response  */
	retval = xio_rdma_prep_req_in_data(rdma_hndl, task);
	if (retval != 0) {
		ERROR_LOG("rdma_prep_req_in_data failed\n");
		return -1;
	}

	/* prepare the out message  */
	retval = xio_rdma_prep_req_out_data(rdma_hndl, task);
	if (retval != 0) {
		ERROR_LOG("rdma_prep_req_out_data failed\n");
		return -1;
	}

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0) {
		ERROR_LOG("write tlv failed\n");
		return -1;
	}

	/* set the length */
	rdma_task->txd.sge[0].length = xio_mbuf_get_curr_offset(&task->mbuf);
	sge_len = rdma_task->txd.sge[0].length;

	/* validate header */
	if (XIO_TLV_LEN + payload != sge_len) {
		ERROR_LOG("header validation failed\n");
		return -1;
	}
	xio_task_addref(task);

	/* check for inline */
	rdma_task->txd.send_wr.send_flags = 0;

	for (i = 1; i < rdma_task->txd.send_wr.num_sge; i++)
		sge_len += rdma_task->txd.sge[i].length;

	if (sge_len < rdma_hndl->max_inline_data)
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_INLINE;


	if (IS_FIN(task->tlv_type)) {
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_FENCE;
		must_send = 1;
	}

	if (unlikely(++rdma_hndl->req_sig_cnt >= HARD_CQ_MOD ||
		     task->is_control)) {
		/* avoid race between send completion and response arrival */
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_SIGNALED;
		rdma_hndl->req_sig_cnt = 0;
	}

	rdma_task->ib_op = XIO_IB_SEND;

	list_move_tail(&task->tasks_list_entry, &rdma_hndl->tx_ready_list);

	rdma_hndl->tx_ready_tasks_num++;

	/* transmit only if  available */
	if (task->omsg->more_in_batch == 0) {
		must_send = 1;
	} else {
		if (tx_window_sz(rdma_hndl) >= SEND_TRESHOLD)
			must_send = 1;
	}
	/* resource are now available and rdma rd  requests are pending kick
	 * them
	 */
	if (rdma_hndl->kick_rdma_rd) {
		retval = xio_xmit_rdma_rd(rdma_hndl);
		if (retval) {
			if (xio_errno() != EAGAIN) {
				ERROR_LOG("xio_xmit_rdma_rd failed\n");
				return -1;
			}
			retval = 0;
		}
	}

	if (must_send) {
		retval = xio_rdma_xmit(rdma_hndl);
		if (retval) {
			if (xio_errno() != EAGAIN) {
				ERROR_LOG("xio_rdma_xmit failed\n");
				return -1;
			}
			retval = 0;
		}
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_rsp							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_rsp(struct xio_rdma_transport *rdma_hndl,
	struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_rsp_hdr	rsp_hdr;
	uint64_t		payload;
	uint64_t		xio_hdr_len;
	uint64_t		ulp_hdr_len;
	uint64_t		ulp_pad_len = 0;
	uint64_t		ulp_imm_len;
	size_t			retval;
	int			i;
	/*int			data_alignment = DEF_DATA_ALIGNMENT;*/
	size_t			sge_len;
	int			must_send = 0;

	if (rdma_hndl->reqs_in_flight_nr + rdma_hndl->rsps_in_flight_nr >
	    rdma_hndl->max_tx_ready_tasks_num) {
		xio_set_error(EAGAIN);
		return -1;
	}

	if (rdma_hndl->rsps_in_flight_nr >=
			rdma_hndl->max_tx_ready_tasks_num - 1) {
		xio_set_error(EAGAIN);
		return -1;
	}
	/* tx ready is full - refuse request */
	if (rdma_hndl->tx_ready_tasks_num >=
			rdma_hndl->max_tx_ready_tasks_num) {
		xio_set_error(EAGAIN);
		return -1;
	}

	/* calculate headers */
	ulp_hdr_len	= task->omsg->out.header.iov_len;
	ulp_imm_len	= xio_iovex_length(task->omsg->out.data_iov,
					   task->omsg->out.data_iovlen);
	xio_hdr_len = xio_mbuf_get_curr_offset(&task->mbuf);
	xio_hdr_len += sizeof(rsp_hdr);

	if (rdma_hndl->max_send_buf_sz < xio_hdr_len + ulp_hdr_len) {
		ERROR_LOG("header size %lu exceeds max header %lu\n",
			  ulp_hdr_len,
			  rdma_hndl->max_send_buf_sz - xio_hdr_len);
		xio_set_error(XIO_E_MSG_SIZE);
		goto cleanup;
	}

	/* Small data is outgoing via SEND unless the requester explicitly
	 * insisted on RDMA operation and provided resources.
	 */
	if ((ulp_imm_len == 0) || (!rdma_task->req_read_num_sge &&
	    ((xio_hdr_len + ulp_hdr_len /*+ data_alignment*/ + ulp_imm_len)
				< rdma_hndl->max_send_buf_sz))) {
		/*
		if (data_alignment && ulp_imm_len) {
			uint16_t hdr_len = xio_hdr_len + ulp_hdr_len;
			ulp_pad_len = ALIGN(hdr_len, data_alignment) - hdr_len;
		}
		*/
		rdma_task->ib_op = XIO_IB_SEND;
		/* write xio header to the buffer */
		retval = xio_rdma_prep_rsp_header(
				rdma_hndl, task,
				ulp_hdr_len, ulp_pad_len, ulp_imm_len,
				XIO_E_SUCCESS);
		if (retval)
			goto cleanup;

		/* if there is data, set it to buffer or directly to the sge */
		if (ulp_imm_len) {
			retval = xio_rdma_write_send_data(rdma_hndl, task);
			if (retval)
				goto cleanup;
		} else {
			/* no data at all */
			task->omsg->out.data_iov[0].iov_base	= NULL;
			task->omsg->out.data_iovlen		= 0;
		}
	} else {
		if (rdma_task->req_read_sge[0].addr &&
		    rdma_task->req_read_sge[0].length &&
		    rdma_task->req_read_sge[0].stag) {
			/* the data is sent via RDMA_WRITE */

			/* prepare rdma write */
			xio_sched_rdma_wr_req(rdma_hndl, task);

			/* and the header is sent via SEND */
			/* write xio header to the buffer */
			retval = xio_rdma_prep_rsp_header(
					rdma_hndl, task,
					ulp_hdr_len, 0, ulp_imm_len,
					XIO_E_SUCCESS);
		} else {
			ERROR_LOG("partial completion of request due " \
				  "to missing, response buffer\n");

			/* the client did not provide buffer for response */
			retval = xio_rdma_prep_rsp_header(
					rdma_hndl, task,
					ulp_hdr_len, 0, 0,
					XIO_E_PARTIAL_MSG);
		}
	}

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		goto cleanup;

	/* set the length */
	rdma_task->txd.sge[0].length = xio_mbuf_get_curr_offset(&task->mbuf);
	sge_len = rdma_task->txd.sge[0].length;

	/* validate header */
	if (XIO_TLV_LEN + payload != rdma_task->txd.sge[0].length) {
		ERROR_LOG("header validation failed\n");
		goto cleanup;
	}

	rdma_task->txd.send_wr.send_flags = 0;
	if (++rdma_hndl->rsp_sig_cnt >= SOFT_CQ_MOD || task->is_control) {
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_SIGNALED;
		rdma_hndl->rsp_sig_cnt = 0;
	}

	/* check for inline */
	if (rdma_task->ib_op == XIO_IB_SEND) {
		for (i = 1; i < rdma_task->txd.send_wr.num_sge; i++)
			sge_len += rdma_task->txd.sge[i].length;

		if (sge_len < rdma_hndl->max_inline_data)
			rdma_task->txd.send_wr.send_flags |= IBV_SEND_INLINE;

		list_move_tail(&task->tasks_list_entry,
			       &rdma_hndl->tx_ready_list);
		rdma_hndl->tx_ready_tasks_num++;
	}

	if (IS_FIN(task->tlv_type)) {
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_FENCE;
		must_send = 1;
	}

	/* transmit only if  available */
	if (task->omsg->more_in_batch == 0) {
		must_send = 1;
	} else {
		if (tx_window_sz(rdma_hndl) >= SEND_TRESHOLD)
			must_send = 1;
	}
	/* resource are now available and rdma rd  requests are pending kick
	 * them
	 */
	if (rdma_hndl->kick_rdma_rd) {
		retval = xio_xmit_rdma_rd(rdma_hndl);
		if (retval) {
			retval = xio_errno();
			if (retval != EAGAIN) {
				ERROR_LOG("xio_xmit_rdma_rd failed. %s\n",
					  xio_strerror(retval));
				return -1;
			}
			retval = 0;
		}
	}

	if (must_send) {
		retval = xio_rdma_xmit(rdma_hndl);
		if (retval) {
			retval = xio_errno();
			if (retval != EAGAIN) {
				ERROR_LOG("xio_xmit_rdma failed. %s\n",
					  xio_strerror(retval));
				return -1;
			}
			retval = 0;
		}
	}

	return retval;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_rdma_send_msg failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_rsp_send_comp						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_rsp_send_comp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	union xio_transport_event_data event_data;

	if (IS_CANCEL(task->tlv_type))
		return 0;

	event_data.msg.op	= XIO_WC_OP_SEND;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_SEND_COMPLETION,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_req_send_comp						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_req_send_comp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	union xio_transport_event_data event_data;

	if (IS_CANCEL(task->tlv_type))
		return 0;

	event_data.msg.op	= XIO_WC_OP_SEND;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_SEND_COMPLETION,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_recv_rsp							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_rsp(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	int			retval = 0;
	union xio_transport_event_data event_data;
	struct xio_rsp_hdr	rsp_hdr;
	struct xio_msg		*imsg;
	struct xio_msg		*omsg;
	void			*ulp_hdr;
	XIO_TO_RDMA_TASK(task, rdma_task);
	XIO_TO_RDMA_TASK(task, rdma_sender_task);
	int			i;

	/* read the response header */
	retval = xio_rdma_read_rsp_header(rdma_hndl, task, &rsp_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}
	/* update receive + send window */
	if (rdma_hndl->exp_sn == rsp_hdr.sn) {
		rdma_hndl->exp_sn++;
		rdma_hndl->ack_sn = rsp_hdr.sn;
		rdma_hndl->peer_credits += rsp_hdr.credits;
	} else {
		ERROR_LOG("ERROR: expected sn:%d, arrived sn:%d\n",
			  rdma_hndl->exp_sn, rsp_hdr.sn);
	}
	/* read the sn */
	rdma_task->sn = rsp_hdr.sn;

	task->imsg.more_in_batch = rdma_task->more_in_batch;

	/* find the sender task */
	task->sender_task =
		xio_rdma_primary_task_lookup(rdma_hndl,
					     rsp_hdr.tid);

	rdma_sender_task = task->sender_task->dd_data;

	/* mark the sender task as arrived */
	task->sender_task->state = XIO_TASK_STATE_RESPONSE_RECV;

	omsg = task->sender_task->omsg;
	imsg = &task->imsg;

	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);
	/* msg from received message */
	if (rsp_hdr.ulp_hdr_len) {
		imsg->in.header.iov_base	= ulp_hdr;
		imsg->in.header.iov_len		= rsp_hdr.ulp_hdr_len;
	} else {
		imsg->in.header.iov_base	= NULL;
		imsg->in.header.iov_len		= 0;
	}
	omsg->status = rsp_hdr.status;

	/* handle the headers */
	if (omsg->in.header.iov_base) {
		/* copy header to user buffers */
		size_t hdr_len = 0;
		if (imsg->in.header.iov_len > omsg->in.header.iov_len)  {
			hdr_len = omsg->in.header.iov_len;
			omsg->status = XIO_E_MSG_SIZE;
		} else {
			hdr_len = imsg->in.header.iov_len;
			omsg->status = XIO_E_SUCCESS;
		}
		if (hdr_len)
			memcpy(omsg->in.header.iov_base,
			       imsg->in.header.iov_base,
			       hdr_len);
		else
			*((char *)omsg->in.header.iov_base) = 0;

		omsg->in.header.iov_len = hdr_len;
	} else {
		/* no copy - just pointers */
		memclonev(&omsg->in.header, 1, &imsg->in.header, 1);
	}

	switch (rsp_hdr.opcode) {
	case XIO_IB_SEND:
		/* if data arrived, set the pointers */
		if (rsp_hdr.ulp_imm_len) {
			imsg->in.data_iov[0].iov_base	= ulp_hdr +
				imsg->in.header.iov_len + rsp_hdr.ulp_pad_len;
			imsg->in.data_iov[0].iov_len	= rsp_hdr.ulp_imm_len;
			imsg->in.data_iovlen		= 1;
		} else {
			imsg->in.data_iov[0].iov_base	= NULL;
			imsg->in.data_iov[0].iov_len	= 0;
			imsg->in.data_iovlen		= 0;
		}
		if (omsg->in.data_iovlen) {
			/* deep copy */
			if (imsg->in.data_iovlen) {
				size_t idata_len  = xio_iovex_length(
					imsg->in.data_iov,
					imsg->in.data_iovlen);
				size_t odata_len  = xio_iovex_length(
					omsg->in.data_iov,
					omsg->in.data_iovlen);

				if (idata_len > odata_len) {
					omsg->status = XIO_E_MSG_SIZE;
					goto partial_msg;
				} else {
					omsg->status = XIO_E_SUCCESS;
				}
				if (omsg->in.data_iov[0].iov_base)  {
					/* user provided buffer so do copy */
					omsg->in.data_iovlen = memcpyv(
					  (struct xio_iovec *)omsg->in.data_iov,
					  omsg->in.data_iovlen,
					  (struct xio_iovec *)imsg->in.data_iov,
					  imsg->in.data_iovlen);
				} else {
					/* use provided only length - set user
					 * pointers */
					omsg->in.data_iovlen =  memclonev(
					(struct xio_iovec *)omsg->in.data_iov,
					omsg->in.data_iovlen,
					(struct xio_iovec *)imsg->in.data_iov,
					imsg->in.data_iovlen);
				}
			} else {
				omsg->in.data_iovlen = imsg->in.data_iovlen;
			}
		} else {
			omsg->in.data_iovlen =
				memclonev((struct xio_iovec *)omsg->in.data_iov,
					  XIO_MAX_IOV,
					  (struct xio_iovec *)imsg->in.data_iov,
					  imsg->in.data_iovlen);
		}
		break;
	case XIO_IB_RDMA_WRITE:
		imsg->in.data_iov[0].iov_base	=
			ptr_from_int64(rdma_sender_task->read_sge[0].addr);
		imsg->in.data_iov[0].iov_len	= rsp_hdr.ulp_imm_len;
		imsg->in.data_iovlen		= 1;

		/* user provided mr */
		if (omsg->in.data_iov[0].mr)  {
			/* data was copied directly to user buffer */
			/* need to update the buffer length */
			omsg->in.data_iov[0].iov_len =
				imsg->in.data_iov[0].iov_len;
		} else  {
			/* user provided buffer but not mr */
			/* deep copy */

			if (omsg->in.data_iov[0].iov_base)  {
				omsg->in.data_iovlen = memcpyv(
					(struct xio_iovec *)omsg->in.data_iov,
					omsg->in.data_iovlen,
					(struct xio_iovec *)imsg->in.data_iov,
					imsg->in.data_iovlen);

				/* put buffers back to pool */
				for (i = 0; i < rdma_sender_task->read_num_sge;
						i++) {
					xio_mempool_free(
						&rdma_sender_task->read_sge[i]);
					rdma_sender_task->read_sge[i].cache = 0;
				}
				rdma_sender_task->read_num_sge = 0;
			} else {
				/* use provided only length - set user
				 * pointers */
				omsg->in.data_iovlen = memclonev(
					(struct xio_iovec *)omsg->in.data_iov,
					omsg->in.data_iovlen,
					(struct xio_iovec *)imsg->in.data_iov,
					imsg->in.data_iovlen);
			}
		}
		break;
	default:
		ERROR_LOG("unexpected opcode\n");
		break;
	}

partial_msg:
	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	/* notify the upper layer of received message */
	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE,
				      &event_data);
	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_rdma_on_recv_rsp failed. (errno=%d %s)\n",
		  retval, xio_strerror(retval));
	xio_transport_notify_observer_error(&rdma_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_notify_assign_in_buf					     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_assign_in_buf(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task, int *is_assigned)
{
	union xio_transport_event_data event_data = {
			.assign_in_buf.task	   = task,
			.assign_in_buf.is_assigned = 0
	};

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_ASSIGN_IN_BUF,
				      &event_data);

	*is_assigned = event_data.assign_in_buf.is_assigned;
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_prep_rdma_op							     */
/*---------------------------------------------------------------------------*/
static int xio_prep_rdma_op(
		struct xio_task *task,
		struct xio_rdma_transport *rdma_hndl,
		enum xio_ib_op_code  xio_ib_op,
		enum ibv_wc_opcode   opcode,
		struct xio_sge *lsg_list, size_t lsize, size_t *out_lsize,
		struct xio_sge *rsg_list, size_t rsize,
		uint32_t op_size,
		int	signaled,
		struct list_head *target_list,
		int	*tasks_used)
{
	struct xio_task		*tmp_task;
	struct xio_rdma_task	*tmp_rdma_task;

	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_work_req	*rdmad = &rdma_task->rdmad;
	struct xio_task		*ptask, *next_ptask;


	uint64_t laddr  = lsg_list[0].addr;
	uint64_t raddr  = rsg_list[0].addr;

	uint32_t llen	= lsg_list[0].length;
	uint32_t rlen	= rsg_list[0].length;
	uint32_t lkey	= lsg_list[0].stag;
	uint32_t rkey	= rsg_list[0].stag;
	int	l = 0, r = 0, k = 0;
	uint32_t	tot_len = 0;
	uint32_t	int_len = 0;

	LIST_HEAD(tmp_list);

	*tasks_used = 0;

	if (lsize < 1 || rsize < 1) {
		ERROR_LOG("iovec size < 1 lsize:%zd, rsize:%zd\n",
			  lsize, rsize);
		return -1;
	}
	if (rsize == 1) {
		tmp_task = task;
	} else {
		/* take new task */
		tmp_task = xio_rdma_primary_task_alloc(rdma_hndl);
		if (!tmp_task) {
			ERROR_LOG("primary task pool is empty\n");
			return -1;
		}
	}
	(*tasks_used)++;
	tmp_rdma_task =
		(struct xio_rdma_task *)tmp_task->dd_data;
	rdmad = &tmp_rdma_task->rdmad;

	while (1) {
		if (rlen < llen) {
			rdmad->send_wr.num_sge		= k + 1;
			rdmad->send_wr.wr_id		=
					uint64_from_ptr(tmp_task);
			rdmad->send_wr.next		= NULL;
			rdmad->send_wr.opcode		= opcode;
			rdmad->send_wr.send_flags	=
					(signaled ? IBV_SEND_SIGNALED : 0);
			rdmad->send_wr.wr.rdma.remote_addr = raddr;
			rdmad->send_wr.wr.rdma.rkey	   = rkey;

			rdmad->sge[k].addr		= laddr;
			rdmad->sge[k].length		= rlen;
			rdmad->sge[k].lkey		= lkey;
			k				= 0;

			tot_len				+= rlen;
			int_len				+= rlen;
			tmp_rdma_task->ib_op		= xio_ib_op;
			tmp_rdma_task->phantom_idx	= rsize - r - 1;

			/* close the task */
			list_move_tail(&tmp_task->tasks_list_entry, &tmp_list);
			/* advance the remote index */
			r++;
			if (r == rsize) {
				lsg_list[l].length = int_len;
				int_len = 0;
				l++;
				break;
			} else if (r < rsize - 1) {
				/* take new task */
				tmp_task =
					xio_rdma_primary_task_alloc(rdma_hndl);
				if (!tmp_task) {
					ERROR_LOG(
					      "primary task pool is empty\n");
					goto cleanup;
				}
			} else {
				tmp_task = task;
			}
			(*tasks_used)++;

			tmp_rdma_task =
				(struct xio_rdma_task *)tmp_task->dd_data;
			rdmad = &tmp_rdma_task->rdmad;

			llen	-= rlen;
			laddr	+= rlen;
			raddr	= rsg_list[r].addr;
			rlen	= rsg_list[r].length;
			rkey	= rsg_list[r].stag;
		} else if (llen < rlen) {
			rdmad->sge[k].addr	= laddr;
			rdmad->sge[k].length	= llen;
			rdmad->sge[k].lkey	= lkey;
			tot_len			+= llen;
			int_len			+= llen;

			lsg_list[l].length = int_len;
			int_len = 0;
			/* advance the local index */
			l++;
			if (l == lsize) {
				rdmad->send_wr.num_sge		   = k + 1;
				rdmad->send_wr.wr_id		   =
						uint64_from_ptr(tmp_task);
				rdmad->send_wr.next		   = NULL;
				rdmad->send_wr.opcode		   = opcode;
				rdmad->send_wr.send_flags	=
					   (signaled ? IBV_SEND_SIGNALED : 0);
				rdmad->send_wr.wr.rdma.remote_addr = raddr;
				rdmad->send_wr.wr.rdma.rkey	   = rkey;
				tmp_rdma_task->ib_op		   = xio_ib_op;
				tmp_rdma_task->phantom_idx	   =
								rsize - r - 1;
				/* close the task */
				list_move_tail(&tmp_task->tasks_list_entry,
					       &tmp_list);
				break;
			}
			k++;
			rlen	-= llen;
			raddr	+= llen;
			laddr	= lsg_list[l].addr;
			llen	= lsg_list[l].length;
			lkey	= lsg_list[l].stag;
		} else {
			rdmad->send_wr.num_sge		= k + 1;
			rdmad->send_wr.wr_id		=
						uint64_from_ptr(tmp_task);
			rdmad->send_wr.next		= NULL;
			rdmad->send_wr.opcode		= opcode;
			rdmad->send_wr.send_flags	=
					(signaled ? IBV_SEND_SIGNALED : 0);
			rdmad->send_wr.wr.rdma.remote_addr = raddr;
			rdmad->send_wr.wr.rdma.rkey	   = rkey;

			rdmad->sge[k].addr		= laddr;
			rdmad->sge[k].length		= llen;
			rdmad->sge[k].lkey		= lkey;
			k				= 0;

			tot_len			       += llen;
			int_len			       += llen;
			tmp_rdma_task->ib_op		= xio_ib_op;
			tmp_rdma_task->phantom_idx	= rsize - r - 1;

			/* close the task */
			list_move_tail(&tmp_task->tasks_list_entry,
				       &tmp_list);
			/* advance the remote index */
			r++;
			if (r == rsize) {
				lsg_list[l].length = int_len;
				int_len = 0;
				l++;
				break;
			} else if (r < rsize - 1) {
				/* take new task */
				tmp_task = xio_rdma_primary_task_alloc(
								   rdma_hndl);
				if (!tmp_task) {
					ERROR_LOG(
					       "primary task pool is empty\n");
					goto cleanup;
				}
			} else {
				tmp_task = task;
			}
			(*tasks_used)++;
			tmp_rdma_task =
				(struct xio_rdma_task *)tmp_task->dd_data;
			rdmad = &tmp_rdma_task->rdmad;

			lsg_list[l].length = int_len;
			int_len = 0;
			/* advance the local index */
			l++;
			if (l == lsize)
				break;

			laddr	= lsg_list[l].addr;
			llen	= lsg_list[l].length;
			lkey	= lsg_list[l].stag;

			raddr	= rsg_list[r].addr;
			rlen	= rsg_list[r].length;
			rkey	= rsg_list[r].stag;
		}
	}
	*out_lsize = l;

	if (tot_len < op_size) {
		ERROR_LOG("iovec exhausted\n");
		goto cleanup;
	}

	list_splice_tail(&tmp_list, target_list);

	return 0;
cleanup:

	/* list does not contain the original task */
	list_for_each_entry_safe(ptask, next_ptask, &tmp_list,
				 tasks_list_entry) {
		/* the tmp tasks are returned back to pool */
		xio_tasks_pool_put(task);
	}
	(*tasks_used) = 0;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_set_msg_in_data_iovec						     */
/*---------------------------------------------------------------------------*/
static inline void xio_set_msg_in_data_iovec(struct xio_task *task,
					     struct xio_sge *lsg_list,
					     size_t lsize)
{
	int i;

	for (i = 0; i < lsize; i++)
		task->imsg.in.data_iov[i].iov_len = lsg_list[i].length;

	task->imsg.in.data_iovlen = lsize;
}

/*---------------------------------------------------------------------------*/
/* xio_sched_rdma_rd_req						     */
/*---------------------------------------------------------------------------*/
static int xio_sched_rdma_rd_req(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	int			i, retval;
	int			user_assign_flag = 0;
	size_t			llen = 0, rlen = 0;
	int			tasks_used = 0;
	struct xio_sge		lsg_list[XIO_MAX_IOV];
	size_t			lsg_list_len;
	size_t			lsg_out_list_len;
	struct ibv_mr		*mr;

	/* responder side got request for rdma read */

	/* need for buffer to do rdma read. there are two options:	   */
	/* option 1: user provides call back that fills application memory */
	/* option 2: use internal buffer pool				   */

	/* hint the upper layer of sizes */
	for (i = 0;  i < rdma_task->req_write_num_sge; i++) {
		task->imsg.in.data_iov[i].iov_base  = NULL;
		task->imsg.in.data_iov[i].iov_len  =
					rdma_task->req_write_sge[i].length;
		rlen += rdma_task->req_write_sge[i].length;
		rdma_task->read_sge[i].cache = NULL;
	}
	task->imsg.in.data_iovlen = rdma_task->req_write_num_sge;

	for (i = 0;  i < rdma_task->req_read_num_sge; i++) {
		task->imsg.out.data_iov[i].iov_base  = NULL;
		task->imsg.out.data_iov[i].iov_len  =
					rdma_task->req_read_sge[i].length;
		rdma_task->write_sge[i].cache = NULL;
	}
	for (i = 0;  i < rdma_task->req_recv_num_sge; i++) {
		task->imsg.out.data_iov[i].iov_base  = NULL;
		task->imsg.out.data_iov[i].iov_len  =
					rdma_task->req_recv_sge[i].length;
		task->imsg.out.data_iov[i].mr  = NULL;
	}
	if (rdma_task->req_read_num_sge)
		task->imsg.out.data_iovlen = rdma_task->req_read_num_sge;
	else if (rdma_task->req_recv_num_sge)
		task->imsg.out.data_iovlen = rdma_task->req_recv_num_sge;
	else
		task->imsg.out.data_iovlen = 0;

	xio_rdma_assign_in_buf(rdma_hndl, task, &user_assign_flag);

	if (user_assign_flag) {
		/* if user does not have buffers ignore */
		if (task->imsg.in.data_iovlen == 0) {
			WARN_LOG("application has not provided buffers\n");
			WARN_LOG("rdma read is ignored\n");
			task->imsg.status = XIO_E_PARTIAL_MSG;
			return -1;
		}
		for (i = 0;  i < task->imsg.in.data_iovlen; i++) {
			if (task->imsg.in.data_iov[i].mr == NULL) {
				ERROR_LOG("application has not provided mr\n");
				ERROR_LOG("rdma read is ignored\n");
				task->imsg.status = EINVAL;
				return -1;
			}
			llen += task->imsg.in.data_iov[i].iov_len;
		}
		if (rlen  > llen) {
			ERROR_LOG("application provided too small iovec\n");
			ERROR_LOG("remote peer want to write %zd bytes while" \
				  "local peer provided buffer size %zd bytes\n",
				  rlen, llen);
			ERROR_LOG("rdma read is ignored\n");
			task->imsg.status = EINVAL;
			return -1;
		}
	} else {
		if (rdma_hndl->rdma_mempool == NULL) {
				xio_set_error(XIO_E_NO_BUFS);
				ERROR_LOG(
					"message /read/write failed - " \
					"library's memory pool disabled\n");
				goto cleanup;
		}

		for (i = 0;  i < rdma_task->req_write_num_sge; i++) {
			retval = xio_mempool_alloc(
					rdma_hndl->rdma_mempool,
					rdma_task->req_write_sge[i].length,
					&rdma_task->read_sge[i]);

			if (retval) {
				rdma_task->read_num_sge = i;
				ERROR_LOG("mempool is empty for %zd bytes\n",
					  rdma_task->read_sge[i].length);

				task->imsg.status = ENOMEM;
				goto cleanup;
			}
			task->imsg.in.data_iov[i].iov_base =
					rdma_task->read_sge[i].addr;
			task->imsg.in.data_iov[i].iov_len  =
					rdma_task->read_sge[i].length;
			task->imsg.in.data_iov[i].mr =
					rdma_task->read_sge[i].mr;

			llen += task->imsg.in.data_iov[i].iov_len;
		}
		task->imsg.in.data_iovlen = rdma_task->req_write_num_sge;
		rdma_task->read_num_sge = rdma_task->req_write_num_sge;
	}

	for (i = 0;  i < task->imsg.in.data_iovlen; i++) {
		lsg_list[i].addr = uint64_from_ptr(
					task->imsg.in.data_iov[i].iov_base);
		lsg_list[i].length = task->imsg.in.data_iov[i].iov_len;
		mr = xio_rdma_mr_lookup(task->imsg.in.data_iov[i].mr,
					rdma_hndl->tcq->dev);
		lsg_list[i].stag	= mr->rkey;
	}
	lsg_list_len = task->imsg.in.data_iovlen;

	retval = xio_validate_rdma_op(
			lsg_list, lsg_list_len,
			rdma_task->req_write_sge, rdma_task->req_write_num_sge,
			min(rlen, llen));
	if (retval) {
		ERROR_LOG("failed to invalidate input iovecs\n");
		ERROR_LOG("rdma read is ignored\n");
		task->imsg.status = EINVAL;
		return -1;
	}

	xio_prep_rdma_op(task, rdma_hndl,
			 XIO_IB_RDMA_READ,
			 IBV_WR_RDMA_READ,
			 lsg_list,
			 lsg_list_len, &lsg_out_list_len,
			 rdma_task->req_write_sge,
			 rdma_task->req_write_num_sge,
			 min(rlen, llen),
			 1,
			 &rdma_hndl->rdma_rd_list, &tasks_used);

	/* prepare the in side of the message */
	xio_set_msg_in_data_iovec(task, lsg_list, lsg_out_list_len);

	xio_xmit_rdma_rd(rdma_hndl);

	return 0;
cleanup:
	for (i = 0; i < rdma_task->read_num_sge; i++)
		xio_mempool_free(&rdma_task->read_sge[i]);

	rdma_task->read_num_sge = 0;
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_sched_rdma_wr_req						     */
/*---------------------------------------------------------------------------*/
static int xio_sched_rdma_wr_req(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	XIO_TO_RDMA_TASK(task, rdma_task);
	int			i, retval = 0;
	struct xio_sge		lsg_list[XIO_MAX_IOV];
	struct ibv_mr		*mr;
	size_t			lsg_list_len;
	size_t			lsg_out_list_len;
	size_t			rlen = 0, llen = 0;
	int			tasks_used = 0;


	/* user did not provided mr */
	if (task->omsg->out.data_iov[0].mr == NULL) {
		if (rdma_hndl->rdma_mempool == NULL) {
			xio_set_error(XIO_E_NO_BUFS);
			ERROR_LOG(
					"message /read/write failed - " \
					"library's memory pool disabled\n");
			goto cleanup;
		}
		/* user did not provide mr - take buffers from pool
		 * and do copy */
		for (i = 0; i < task->omsg->out.data_iovlen; i++) {
			retval = xio_mempool_alloc(
					rdma_hndl->rdma_mempool,
					task->omsg->out.data_iov[i].iov_len,
					&rdma_task->write_sge[i]);
			if (retval) {
				rdma_task->write_num_sge = i;
				xio_set_error(ENOMEM);
				ERROR_LOG("mempool is empty for %zd bytes\n",
					  task->omsg->out.data_iov[i].iov_len);
				goto cleanup;
			}
			lsg_list[i].addr	= uint64_from_ptr(
						rdma_task->write_sge[i].addr);
			lsg_list[i].length	=
					  task->omsg->out.data_iov[i].iov_len;
			mr = xio_rdma_mr_lookup(rdma_task->write_sge[i].mr,
						rdma_hndl->tcq->dev);
			lsg_list[i].stag	= mr->lkey;

			llen		+= lsg_list[i].length;

			/* copy the data to the buffer */
			memcpy(rdma_task->write_sge[i].addr,
			       task->omsg->out.data_iov[i].iov_base,
			       task->omsg->out.data_iov[i].iov_len);
		}
	} else {
		for (i = 0; i < task->omsg->out.data_iovlen; i++) {
			lsg_list[i].addr	= uint64_from_ptr(
					task->omsg->out.data_iov[i].iov_base);
			lsg_list[i].length	=
					   task->omsg->out.data_iov[i].iov_len;
			mr = xio_rdma_mr_lookup(task->omsg->out.data_iov[i].mr,
						rdma_hndl->tcq->dev);
			lsg_list[i].stag	= mr->lkey;

			llen += lsg_list[i].length;
		}
	}
	lsg_list_len = task->omsg->out.data_iovlen;

	for (i = 0;  i < rdma_task->req_read_num_sge; i++)
		rlen += rdma_task->req_read_sge[i].length;

	if (rlen  < llen) {
		ERROR_LOG("peer provided too small iovec\n");
		ERROR_LOG("rdma write is ignored\n");
		task->omsg->status = EINVAL;
		goto cleanup;
	}
	retval = xio_validate_rdma_op(
			lsg_list, lsg_list_len,
			rdma_task->req_read_sge,
			rdma_task->req_read_num_sge,
			min(rlen, llen));
	if (retval) {
		ERROR_LOG("failed to invalidate input iovecs\n");
		ERROR_LOG("rdma write is ignored\n");
		task->omsg->status = EINVAL;
		goto cleanup;
	}
	xio_prep_rdma_op(task, rdma_hndl,
			 XIO_IB_RDMA_WRITE,
			 IBV_WR_RDMA_WRITE,
			 lsg_list, lsg_list_len, &lsg_out_list_len,
			 rdma_task->req_read_sge,
			 rdma_task->req_read_num_sge,
			 min(rlen, llen),
			 0,
			 &rdma_hndl->tx_ready_list, &tasks_used);
	/* xio_prep_rdma_op used splice to transfer "tasks_used"  to
	 * tx_ready_list
	 */
	rdma_hndl->tx_ready_tasks_num += tasks_used;
	return 0;
cleanup:
	for (i = 0; i < rdma_task->write_num_sge; i++)
		xio_mempool_free(&rdma_task->write_sge[i]);

	rdma_task->write_num_sge = 0;
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_recv_req							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_req(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	int			retval = 0;
	XIO_TO_RDMA_TASK(task, rdma_task);
	union xio_transport_event_data event_data;
	struct xio_req_hdr	req_hdr;
	struct xio_msg		*imsg;
	void			*ulp_hdr;
	int			i;

	/* read header */
	retval = xio_rdma_read_req_header(rdma_hndl, task, &req_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}
	if (rdma_hndl->exp_sn == req_hdr.sn) {
		rdma_hndl->exp_sn++;
		rdma_hndl->ack_sn = req_hdr.sn;
		rdma_hndl->peer_credits += req_hdr.credits;
	} else {
		ERROR_LOG("ERROR: sn expected:%d, sn arrived:%d\n",
			  rdma_hndl->exp_sn, req_hdr.sn);
	}

	/* save originator identifier */
	task->rtid		= req_hdr.tid;
	task->imsg.more_in_batch = rdma_task->more_in_batch;

	imsg = &task->imsg;
	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	imsg->type = task->tlv_type;
	imsg->in.header.iov_len	= req_hdr.ulp_hdr_len;

	if (req_hdr.ulp_hdr_len)
		imsg->in.header.iov_base	= ulp_hdr;
	else
		imsg->in.header.iov_base	= NULL;

	/* hint upper layer about expected response */
	for (i = 0;  i < rdma_task->req_read_num_sge; i++) {
		imsg->out.data_iov[i].iov_base  = NULL;
		imsg->out.data_iov[i].iov_len  =
					rdma_task->req_read_sge[i].length;
		imsg->out.data_iov[i].mr  = NULL;
	}
	for (i = 0;  i < rdma_task->req_recv_num_sge; i++) {
		imsg->out.data_iov[i].iov_base  = NULL;
		imsg->out.data_iov[i].iov_len  =
					rdma_task->req_recv_sge[i].length;
		imsg->out.data_iov[i].mr  = NULL;
	}
	if (rdma_task->req_read_num_sge)
		imsg->out.data_iovlen = rdma_task->req_read_num_sge;
	else if (rdma_task->req_recv_num_sge)
		imsg->out.data_iovlen = rdma_task->req_recv_num_sge;
	else
		imsg->out.data_iovlen = 0;

	switch (req_hdr.opcode) {
	case XIO_IB_SEND:
		if (req_hdr.ulp_imm_len) {
			/* incoming data via SEND */
			/* if data arrived, set the pointers */
			imsg->in.data_iov[0].iov_len	= req_hdr.ulp_imm_len;
			imsg->in.data_iov[0].iov_base	= ulp_hdr +
				imsg->in.header.iov_len +
				req_hdr.ulp_pad_len;
			imsg->in.data_iovlen		= 1;
		} else {
			/* no data at all */
			imsg->in.data_iov[0].iov_base	= NULL;
			imsg->in.data_iovlen		= 0;
		}
		break;
	case XIO_IB_RDMA_READ:
		/* schedule request for RDMA READ. in case of error
		 * don't schedule the rdma read operation */
		TRACE_LOG("scheduling rdma read\n");
		retval = xio_sched_rdma_rd_req(rdma_hndl, task);
		if (retval == 0)
			return 0;
		ERROR_LOG("scheduling rdma read failed\n");
		goto cleanup;
		break;
	default:
		ERROR_LOG("unexpected opcode\n");
		goto cleanup;
		break;
	};

	/* must delay the send due to pending rdma read requests
	 * if not user will get out of order messages - need fence
	 */
	if (!list_empty(&rdma_hndl->rdma_rd_list)) {
		list_move_tail(&task->tasks_list_entry,
			       &rdma_hndl->rdma_rd_list);
		return 0;
	}
	if (rdma_hndl->rdma_in_flight) {
		rdma_hndl->rdma_in_flight++;
		list_move_tail(&task->tasks_list_entry,
			       &rdma_hndl->rdma_rd_in_flight_list);
		return 0;
	}

	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE, &event_data);

	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_rdma_on_recv_req failed. (errno=%d %s)\n", retval,
		  xio_strerror(retval));
	xio_transport_notify_observer_error(&rdma_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_write_setup_msg						     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_write_setup_msg(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_rdma_setup_msg *msg)
{
	struct xio_rdma_setup_msg	*tmp_msg;

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* jump after connection setup header */
	if (rdma_hndl->base.is_client)
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_conn_setup_req));
	else
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_conn_setup_rsp));

	tmp_msg = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	PACK_LLVAL(msg, tmp_msg, buffer_sz);
	PACK_SVAL(msg, tmp_msg, sq_depth);
	PACK_SVAL(msg, tmp_msg, rq_depth);
	PACK_SVAL(msg, tmp_msg, credits);

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_rdma_setup_msg));
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_read_setup_msg						     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_read_setup_msg(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_rdma_setup_msg *msg)
{
	struct xio_rdma_setup_msg	*tmp_msg;

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* jump after connection setup header */
	if (rdma_hndl->base.is_client)
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_conn_setup_rsp));
	else
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_conn_setup_req));

	tmp_msg = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	UNPACK_LLVAL(tmp_msg, msg, buffer_sz);
	UNPACK_SVAL(tmp_msg, msg, sq_depth);
	UNPACK_SVAL(tmp_msg, msg, rq_depth);
	UNPACK_SVAL(tmp_msg, msg, credits);

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.curr,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_rdma_setup_msg));
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_send_setup_req						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_setup_req(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	uint16_t payload;
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct xio_rdma_setup_msg  req;

	req.buffer_sz		= rdma_hndl->max_send_buf_sz;
	req.sq_depth		= rdma_hndl->sq_depth;
	req.rq_depth		= rdma_hndl->rq_depth;
	req.credits		= 0;

	xio_rdma_write_setup_msg(rdma_hndl, task, &req);

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	/* set the length */
	rdma_task->txd.sge[0].length	= xio_mbuf_data_length(&task->mbuf);

	rdma_task->txd.send_wr.send_flags = IBV_SEND_SIGNALED;
	if (rdma_task->txd.sge[0].length < rdma_hndl->max_inline_data)
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_INLINE;

	rdma_task->txd.send_wr.next	= NULL;
	rdma_task->ib_op		= XIO_IB_SEND;
	rdma_task->txd.send_wr.num_sge	= 1;

	xio_task_addref(task);
	rdma_hndl->reqs_in_flight_nr++;
	list_move_tail(&task->tasks_list_entry, &rdma_hndl->in_flight_list);

	rdma_hndl->peer_credits--;
	xio_post_send(rdma_hndl, &rdma_task->txd, 1);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_setup_rsp						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_setup_rsp(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task)
{
	uint16_t payload;
	XIO_TO_RDMA_TASK(task, rdma_task);

	rdma_hndl->sim_peer_credits += rdma_hndl->credits;

	rdma_hndl->setup_rsp.credits = rdma_hndl->credits;
	xio_rdma_write_setup_msg(rdma_hndl, task, &rdma_hndl->setup_rsp);
	rdma_hndl->credits = 0;


	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	/* set the length */
	rdma_task->txd.sge[0].length = xio_mbuf_data_length(&task->mbuf);
	rdma_task->txd.send_wr.send_flags = IBV_SEND_SIGNALED;
	if (rdma_task->txd.sge[0].length < rdma_hndl->max_inline_data)
		rdma_task->txd.send_wr.send_flags |= IBV_SEND_INLINE;
	rdma_task->txd.send_wr.next	= NULL;
	rdma_task->ib_op		= XIO_IB_SEND;
	rdma_task->txd.send_wr.num_sge	= 1;

	rdma_hndl->rsps_in_flight_nr++;
	list_move(&task->tasks_list_entry, &rdma_hndl->in_flight_list);

	rdma_hndl->peer_credits--;
	xio_post_send(rdma_hndl, &rdma_task->txd, 1);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_setup_msg						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_setup_msg(struct xio_rdma_transport *rdma_hndl,
			    struct xio_task *task)
{
	union xio_transport_event_data event_data;
	struct xio_rdma_setup_msg *rsp  = &rdma_hndl->setup_rsp;

	if (rdma_hndl->base.is_client) {
		struct xio_task *sender_task = NULL;
		if (!list_empty(&rdma_hndl->in_flight_list))
			sender_task = list_first_entry(
					&rdma_hndl->in_flight_list,
					struct xio_task,  tasks_list_entry);
		else if (!list_empty(&rdma_hndl->tx_comp_list))
			sender_task = list_first_entry(
					&rdma_hndl->tx_comp_list,
					struct xio_task,  tasks_list_entry);
		else
			ERROR_LOG("could not find sender task\n");

		task->sender_task = sender_task;
		xio_rdma_read_setup_msg(rdma_hndl, task, rsp);
		/* get the initial credits */
		rdma_hndl->peer_credits += rsp->credits;
	} else {
		struct xio_rdma_setup_msg req;

		xio_rdma_read_setup_msg(rdma_hndl, task, &req);

		/* current implementation is symmetric */
		rsp->buffer_sz	= min(req.buffer_sz,
				      rdma_hndl->max_send_buf_sz);
		rsp->sq_depth	= min(req.sq_depth, rdma_hndl->rq_depth);
		rsp->rq_depth	= min(req.rq_depth, rdma_hndl->sq_depth);
	}

	/* save the values */
	rdma_hndl->rq_depth		= rsp->rq_depth;
	rdma_hndl->actual_rq_depth	= rdma_hndl->rq_depth + EXTRA_RQE;
	rdma_hndl->sq_depth		= rsp->sq_depth;
	rdma_hndl->membuf_sz		= rsp->buffer_sz;
	rdma_hndl->max_send_buf_sz	= rsp->buffer_sz;

	/* initialize send window */
	rdma_hndl->sn = 0;
	rdma_hndl->ack_sn = ~0;
	rdma_hndl->credits = 0;
	rdma_hndl->max_sn = rdma_hndl->sq_depth;

	/* initialize receive window */
	rdma_hndl->exp_sn = 0;
	rdma_hndl->max_exp_sn = 0;

	/* now we can calculate  primary pool size */
	xio_rdma_calc_pool_size(rdma_hndl);

	rdma_hndl->state = XIO_STATE_CONNECTED;

	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE, &event_data);
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_write_nop							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_write_nop(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_nop_hdr *nop)
{
	struct  xio_nop_hdr *tmp_nop;

	xio_mbuf_reset(&task->mbuf);

	/* set start of the tlv */
	if (xio_mbuf_tlv_start(&task->mbuf) != 0)
		return;

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* get the pointer */
	tmp_nop = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	PACK_SVAL(nop, tmp_nop, hdr_len);
	PACK_SVAL(nop, tmp_nop, sn);
	PACK_SVAL(nop, tmp_nop, ack_sn);
	PACK_SVAL(nop, tmp_nop, credits);
	tmp_nop->opcode = nop->opcode;
	tmp_nop->flags = nop->flags;

#ifdef EYAL_TODO
	print_hex_dump_bytes("write_nop: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(*nop));
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_nop							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_nop(struct xio_rdma_transport *rdma_hndl)
{
	uint64_t		payload;
	struct xio_task		*task;
	struct xio_rdma_task	*rdma_task;
	struct  xio_nop_hdr	nop = {
		.hdr_len	= sizeof(nop),
		.sn		= rdma_hndl->sn,
		.ack_sn		= rdma_hndl->ack_sn,
		.credits	= rdma_hndl->credits,
		.opcode		= 0,
		.flags		= 0,
	};

	TRACE_LOG("SEND_NOP\n");

	task = xio_rdma_primary_task_alloc(rdma_hndl);
	if (!task) {
		ERROR_LOG("primary task pool is empty\n");
		return -1;
	}

	task->tlv_type	= XIO_CREDIT_NOP;
	rdma_task	= (struct xio_rdma_task *)task->dd_data;

	/* write the message */
	xio_rdma_write_nop(rdma_hndl, task, &nop);
	rdma_hndl->sim_peer_credits += rdma_hndl->credits;
	rdma_hndl->credits = 0;

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	/* set the length */
	rdma_task->txd.sge[0].length	= xio_mbuf_data_length(&task->mbuf);
	rdma_task->txd.send_wr.send_flags =
		IBV_SEND_SIGNALED | IBV_SEND_INLINE | IBV_SEND_FENCE;
	rdma_task->txd.send_wr.next	= NULL;
	rdma_task->ib_op		= XIO_IB_SEND;
	rdma_task->txd.send_wr.num_sge	= 1;

	rdma_hndl->rsps_in_flight_nr++;
	list_add_tail(&task->tasks_list_entry, &rdma_hndl->in_flight_list);

	rdma_hndl->peer_credits--;
	xio_post_send(rdma_hndl, &rdma_task->txd, 1);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_read_nop							     */
/*---------------------------------------------------------------------------*/
static void xio_rdma_read_nop(struct xio_rdma_transport *rdma_hndl,
		struct xio_task *task, struct xio_nop_hdr *nop)
{
	struct  xio_nop_hdr *tmp_nop;

	/* goto to the first tlv */
	xio_mbuf_reset(&task->mbuf);

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* get the pointer */
	tmp_nop = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	UNPACK_SVAL(tmp_nop, nop, hdr_len);
	UNPACK_SVAL(tmp_nop, nop, sn);
	UNPACK_SVAL(tmp_nop, nop, ack_sn);
	UNPACK_SVAL(tmp_nop, nop, credits);
	nop->opcode = tmp_nop->opcode;
	nop->flags = tmp_nop->flags;

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(*nop));
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_recv_nop							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_nop(struct xio_rdma_transport *rdma_hndl,
				struct xio_task *task)
{
	struct xio_nop_hdr	nop;

	TRACE_LOG("RECV_NOP\n");
	xio_rdma_read_nop(rdma_hndl, task, &nop);

	if (rdma_hndl->exp_sn == nop.sn)
		rdma_hndl->peer_credits += nop.credits;
	else
		ERROR_LOG("ERROR: sn expected:%d, sn arrived:%d\n",
			  rdma_hndl->exp_sn, nop.sn);

	/* the rx task is returned back to pool */
	xio_tasks_pool_put(task);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_cancel							     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_send_cancel(struct xio_rdma_transport *rdma_hndl,
				    uint16_t tlv_type,
				    struct  xio_rdma_cancel_hdr *cancel_hdr,
				    void *ulp_msg, size_t ulp_msg_sz)
{
	uint64_t		payload;
	uint16_t		ulp_hdr_len;
	int			retval;
	struct xio_task		*task;
	struct xio_rdma_task	*rdma_task;
	void			*buff;
	struct xio_msg		omsg;

	task = xio_rdma_primary_task_alloc(rdma_hndl);
	if (!task) {
		ERROR_LOG("primary task pool is empty\n");
		return -1;
	}
	xio_mbuf_reset(&task->mbuf);

	/* set start of the tlv */
	if (xio_mbuf_tlv_start(&task->mbuf) != 0)
		return -1;

	task->tlv_type			= tlv_type;
	rdma_task			= (struct xio_rdma_task *)task->dd_data;
	rdma_task->ib_op		= XIO_IB_SEND;
	rdma_task->write_num_sge	= 0;
	rdma_task->read_num_sge		= 0;

	ulp_hdr_len = sizeof(*cancel_hdr) + sizeof(uint16_t) + ulp_msg_sz;
	omsg.out.header.iov_base = ucalloc(1, ulp_hdr_len);
	omsg.out.header.iov_len = ulp_hdr_len;


	/* write the message */
	/* get the pointer */
	buff = omsg.out.header.iov_base;

	/* pack relevant values */
	buff += xio_write_uint16(cancel_hdr->hdr_len, 0, buff);
	buff += xio_write_uint16(cancel_hdr->sn, 0, buff);
	buff += xio_write_uint32(cancel_hdr->result, 0, buff);
	buff += xio_write_uint16((uint16_t)(ulp_msg_sz), 0, buff);
	buff += xio_write_array(ulp_msg, ulp_msg_sz, 0, buff);

	task->omsg = &omsg;

	/* write xio header to the buffer */
	retval = xio_rdma_prep_req_header(
			rdma_hndl, task,
			ulp_hdr_len, 0, 0,
			XIO_E_SUCCESS);
	if (retval)
		return -1;

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	/* set the length */
	rdma_task->txd.sge[0].length	= xio_mbuf_data_length(&task->mbuf);
	rdma_task->txd.send_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	rdma_task->txd.send_wr.next	= NULL;
	rdma_task->txd.send_wr.num_sge	= 1;

	task->omsg = NULL;
	free(omsg.out.header.iov_base);

	rdma_hndl->tx_ready_tasks_num++;
	list_move_tail(&task->tasks_list_entry, &rdma_hndl->tx_ready_list);

	xio_rdma_xmit(rdma_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_send(struct xio_transport_base *transport,
		struct xio_task *task)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	int	retval = -1;

	switch (task->tlv_type) {
	case XIO_CONN_SETUP_REQ:
		retval = xio_rdma_send_setup_req(rdma_hndl, task);
		break;
	case XIO_CONN_SETUP_RSP:
		retval = xio_rdma_send_setup_rsp(rdma_hndl, task);
		break;
	default:
		if (IS_REQUEST(task->tlv_type))
			retval = xio_rdma_send_req(rdma_hndl, task);
		else if (IS_RESPONSE(task->tlv_type))
			retval = xio_rdma_send_rsp(rdma_hndl, task);
		else
			ERROR_LOG("unknown message type:0x%x\n",
				  task->tlv_type);
		break;
	}

	return retval;
}


/*---------------------------------------------------------------------------*/
/* xio_rdma_cancel_req_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_cancel_req_handler(struct xio_rdma_transport *rdma_hndl,
				       struct  xio_rdma_cancel_hdr *cancel_hdr,
				       void	*ulp_msg, size_t ulp_msg_sz)
{
	union xio_transport_event_data	event_data;
	struct xio_task			*ptask, *next_ptask;
	struct xio_rdma_task		*rdma_task;
	int				found = 0;

	/* start by looking for the task rdma_rd  */
	list_for_each_entry_safe(ptask, next_ptask, &rdma_hndl->rdma_rd_list,
				 tasks_list_entry) {
		rdma_task = ptask->dd_data;
		if (rdma_task->phantom_idx == 0 &&
		    rdma_task->sn == cancel_hdr->sn) {
			TRACE_LOG("[%u] - message found on rdma_rd_list\n",
				  cancel_hdr->sn);
			ptask->state = XIO_TASK_STATE_CANCEL_PENDING;
			found = 1;
			break;
		}
	}
	if (!found) {
		list_for_each_entry_safe(ptask, next_ptask,
					 &rdma_hndl->rdma_rd_in_flight_list,
					 tasks_list_entry) {
			rdma_task = ptask->dd_data;
			if (rdma_task->phantom_idx == 0 &&
			    rdma_task->sn == cancel_hdr->sn) {
				TRACE_LOG("[%u] - message found on " \
					  "rdma_rd_in_flight_list\n",
					  cancel_hdr->sn);
				ptask->state = XIO_TASK_STATE_CANCEL_PENDING;
				found = 1;
				break;
			}
		}
	}

	if (!found) {
		TRACE_LOG("[%u] - was not found\n", cancel_hdr->sn);
		/* fill notification event */
		event_data.cancel.ulp_msg	   =  ulp_msg;
		event_data.cancel.ulp_msg_sz	   =  ulp_msg_sz;
		event_data.cancel.task		   =  NULL;
		event_data.cancel.result	   =  0;


		xio_transport_notify_observer(&rdma_hndl->base,
					      XIO_TRANSPORT_CANCEL_REQUEST,
					      &event_data);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_cancel_req_handler						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_cancel_rsp_handler(struct xio_rdma_transport *rdma_hndl,
				       struct xio_rdma_cancel_hdr *cancel_hdr,
				       void *ulp_msg, size_t ulp_msg_sz)
{
	union xio_transport_event_data	event_data;
	struct xio_task			*ptask, *next_ptask;
	struct xio_rdma_task		*rdma_task;
	struct xio_task			*task_to_cancel = NULL;


	if ((cancel_hdr->result ==  XIO_E_MSG_CANCELED) ||
	    (cancel_hdr->result ==  XIO_E_MSG_CANCEL_FAILED)) {
		/* look in the in_flight */
		list_for_each_entry_safe(ptask, next_ptask,
					 &rdma_hndl->in_flight_list,
				tasks_list_entry) {
			rdma_task = ptask->dd_data;
			if (rdma_task->sn == cancel_hdr->sn) {
				task_to_cancel = ptask;
				break;
			}
		}
		if (!task_to_cancel) {
			/* look in the tx_comp */
			list_for_each_entry_safe(ptask, next_ptask,
						 &rdma_hndl->tx_comp_list,
					tasks_list_entry) {
				rdma_task = ptask->dd_data;
				if (rdma_task->sn == cancel_hdr->sn) {
					task_to_cancel = ptask;
					break;
				}
			}
		}

		if (!task_to_cancel)  {
			ERROR_LOG("[%u] - Failed to found canceled message\n",
				  cancel_hdr->sn);
			/* fill notification event */
			event_data.cancel.ulp_msg	=  ulp_msg;
			event_data.cancel.ulp_msg_sz	=  ulp_msg_sz;
			event_data.cancel.task		=  NULL;
			event_data.cancel.result	=  XIO_E_MSG_NOT_FOUND;

			xio_transport_notify_observer(
					&rdma_hndl->base,
					XIO_TRANSPORT_CANCEL_RESPONSE,
					&event_data);
			return 0;
		}
	}

	/* fill notification event */
	event_data.cancel.ulp_msg	   =  ulp_msg;
	event_data.cancel.ulp_msg_sz	   =  ulp_msg_sz;
	event_data.cancel.task		   =  task_to_cancel;
	event_data.cancel.result	   =  cancel_hdr->result;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_CANCEL_RESPONSE,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_recv_cancel_rsp						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_cancel_rsp(struct xio_rdma_transport *rdma_hndl,
				       struct xio_task *task)
{
	int			retval = 0;
	struct xio_rsp_hdr	rsp_hdr;
	struct xio_msg		*imsg;
	void			*ulp_hdr;
	void			*buff;
	uint16_t		ulp_msg_sz;
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct  xio_rdma_cancel_hdr cancel_hdr;

	/* read the response header */
	retval = xio_rdma_read_rsp_header(rdma_hndl, task, &rsp_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}
	/* update receive + send window */
	if (rdma_hndl->exp_sn == rsp_hdr.sn) {
		rdma_hndl->exp_sn++;
		rdma_hndl->ack_sn = rsp_hdr.sn;
		rdma_hndl->peer_credits += rsp_hdr.credits;
	} else {
		ERROR_LOG("ERROR: expected sn:%d, arrived sn:%d\n",
			  rdma_hndl->exp_sn, rsp_hdr.sn);
	}
	/* read the sn */
	rdma_task->sn = rsp_hdr.sn;

	imsg = &task->imsg;
	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	imsg->type = task->tlv_type;
	imsg->in.header.iov_len		= rsp_hdr.ulp_hdr_len;
	imsg->in.header.iov_base	= ulp_hdr;
	imsg->in.data_iov[0].iov_base	= NULL;
	imsg->in.data_iovlen		= 0;

	buff = imsg->in.header.iov_base;
	buff += xio_read_uint16(&cancel_hdr.hdr_len, 0, buff);
	buff += xio_read_uint16(&cancel_hdr.sn, 0, buff);
	buff += xio_read_uint32(&cancel_hdr.result, 0, buff);
	buff += xio_read_uint16(&ulp_msg_sz, 0, buff);

	xio_rdma_cancel_rsp_handler(rdma_hndl, &cancel_hdr,
				    buff, ulp_msg_sz);
	/* return the the cancel response task to pool */
	xio_tasks_pool_put(task);

	return 0;
cleanup:
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_recv_cancel_req						     */
/*---------------------------------------------------------------------------*/
static int xio_rdma_on_recv_cancel_req(struct xio_rdma_transport *rdma_hndl,
				       struct xio_task *task)
{
	int			retval = 0;
	XIO_TO_RDMA_TASK(task, rdma_task);
	struct  xio_rdma_cancel_hdr cancel_hdr;
	struct xio_req_hdr	req_hdr;
	struct xio_msg		*imsg;
	void			*ulp_hdr;
	void			*buff;
	uint16_t		ulp_msg_sz;

	/* read header */
	retval = xio_rdma_read_req_header(rdma_hndl, task, &req_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}
	if (rdma_hndl->exp_sn == req_hdr.sn) {
		rdma_hndl->exp_sn++;
		rdma_hndl->ack_sn = req_hdr.sn;
		rdma_hndl->peer_credits += req_hdr.credits;
	} else {
		ERROR_LOG("ERROR: sn expected:%d, sn arrived:%d\n",
			  rdma_hndl->exp_sn, req_hdr.sn);
	}

	/* read the sn */
	rdma_task->sn = req_hdr.sn;

	imsg	= &task->imsg;
	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* set header pointers */
	imsg->type = task->tlv_type;
	imsg->in.header.iov_len		= req_hdr.ulp_hdr_len;
	imsg->in.header.iov_base	= ulp_hdr;
	imsg->in.data_iov[0].iov_base	= NULL;
	imsg->in.data_iovlen		= 0;

	buff = imsg->in.header.iov_base;
	buff += xio_read_uint16(&cancel_hdr.hdr_len, 0, buff);
	buff += xio_read_uint16(&cancel_hdr.sn, 0, buff);
	buff += xio_read_uint32(&cancel_hdr.result, 0, buff);
	buff += xio_read_uint16(&ulp_msg_sz, 0, buff);

	xio_rdma_cancel_req_handler(rdma_hndl, &cancel_hdr,
				    buff, ulp_msg_sz);
	/* return the the cancel request task to pool */
	xio_tasks_pool_put(task);

	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_rdma_on_recv_req failed. (errno=%d %s)\n", retval,
		  xio_strerror(retval));
	xio_transport_notify_observer_error(&rdma_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_cancel_req							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_cancel_req(struct xio_transport_base *transport,
			    struct xio_msg *req, uint64_t stag,
			    void *ulp_msg, size_t ulp_msg_sz)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	struct xio_task			*ptask, *next_ptask;
	union xio_transport_event_data	event_data;
	struct xio_rdma_task		*rdma_task;
	struct  xio_rdma_cancel_hdr	cancel_hdr = {
		.hdr_len	= sizeof(cancel_hdr),
		.result		= 0
	};

	/* look in the tx_ready */
	list_for_each_entry_safe(ptask, next_ptask, &rdma_hndl->tx_ready_list,
				 tasks_list_entry) {
		if (ptask->omsg &&
		    (ptask->omsg->sn == req->sn) &&
		    (ptask->stag == stag)) {
			TRACE_LOG("[%lu] - message found on tx_ready_list\n",
				  req->sn);

			/* return decrease ref count from task */
			xio_tasks_pool_put(ptask);
			rdma_hndl->tx_ready_tasks_num--;
			list_move_tail(&ptask->tasks_list_entry,
				       &rdma_hndl->tx_comp_list);

			/* fill notification event */
			event_data.cancel.ulp_msg	=  ulp_msg;
			event_data.cancel.ulp_msg_sz	=  ulp_msg_sz;
			event_data.cancel.task		=  ptask;
			event_data.cancel.result	=  XIO_E_MSG_CANCELED;

			xio_transport_notify_observer(&rdma_hndl->base,
						      XIO_TRANSPORT_CANCEL_RESPONSE,
						      &event_data);
			return 0;
		}
	}
	/* look in the in_flight */
	list_for_each_entry_safe(ptask, next_ptask, &rdma_hndl->in_flight_list,
				 tasks_list_entry) {
		if (ptask->omsg &&
		    (ptask->omsg->sn == req->sn) &&
		    (ptask->stag == stag) &&
		    (ptask->state != XIO_TASK_STATE_RESPONSE_RECV)) {
			TRACE_LOG("[%lu] - message found on in_flight_list\n",
				  req->sn);

			rdma_task	= ptask->dd_data;
			cancel_hdr.sn	= rdma_task->sn;

			xio_rdma_send_cancel(rdma_hndl, XIO_CANCEL_REQ,
					     &cancel_hdr,
					     ulp_msg, ulp_msg_sz);
			return 0;
		}
	}
	/* look in the tx_comp */
	list_for_each_entry_safe(ptask, next_ptask, &rdma_hndl->tx_comp_list,
				 tasks_list_entry) {
		if (ptask->omsg &&
		    (ptask->omsg->sn == req->sn) &&
		    (ptask->stag == stag) &&
		    (ptask->state != XIO_TASK_STATE_RESPONSE_RECV)) {
			TRACE_LOG("[%lu] - message found on tx_comp_list\n",
				  req->sn);
			rdma_task	= ptask->dd_data;
			cancel_hdr.sn	= rdma_task->sn;

			xio_rdma_send_cancel(rdma_hndl, XIO_CANCEL_REQ,
					     &cancel_hdr,
					     ulp_msg, ulp_msg_sz);
			return 0;
		}
	}
	TRACE_LOG("[%lu] - message not found on tx path\n", req->sn);

	/* fill notification event */
	event_data.cancel.ulp_msg	   =  ulp_msg;
	event_data.cancel.ulp_msg_sz	   =  ulp_msg_sz;
	event_data.cancel.task		   =  NULL;
	event_data.cancel.result	   =  XIO_E_MSG_NOT_FOUND;

	xio_transport_notify_observer(&rdma_hndl->base,
				      XIO_TRANSPORT_CANCEL_RESPONSE,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_cancel_rsp							     */
/*---------------------------------------------------------------------------*/
int xio_rdma_cancel_rsp(struct xio_transport_base *transport,
			struct xio_task *task, enum xio_status result,
			void *ulp_msg, size_t ulp_msg_sz)
{
	struct xio_rdma_transport *rdma_hndl =
		(struct xio_rdma_transport *)transport;
	struct xio_rdma_task	*rdma_task;

	struct  xio_rdma_cancel_hdr cancel_hdr = {
		.hdr_len	= sizeof(cancel_hdr),
		.result		= result,
	};

	if (task) {
		rdma_task = task->dd_data;
		cancel_hdr.sn = rdma_task->sn;
	} else {
		cancel_hdr.sn = 0;
	}

	/* fill dummy transport header since was handled by upper layer
	 */
	return xio_rdma_send_cancel(rdma_hndl, XIO_CANCEL_RSP,
				    &cancel_hdr, ulp_msg, ulp_msg_sz);
}

