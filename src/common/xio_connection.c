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
#include "libxio.h"
#include "xio_common.h"
#include "xio_task.h"
#include "xio_msg_list.h"
#include "xio_observer.h"
#include "xio_conn.h"
#include "xio_connection.h"
#include "xio_session.h"
#include "xio_context.h"

#define MSG_POOL_SZ			1024
#define XIO_CONNECTION_INFLIGHT_BUDGET	64
#define XIO_CONNECTION_APP_BUDGET	256

#define		IS_APPLICATION_MSG(msg) \
		  (IS_MESSAGE((msg)->type) || IS_ONE_WAY((msg)->type))

static struct xio_transition xio_transition_table[][2] = {
/* INIT */	  {
		   {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		   {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  },

/* ESTABLISHED */ {
		   {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		   {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  },

/* ONLINE */      {
		   {.valid = 1, .next_state = XIO_CONNECTION_STATE_CLOSE_WAIT, .send_flags = SEND_ACK },
		   {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  },

/* FIN_WAIT_1 */  {
		   {.valid = 1, .next_state = XIO_CONNECTION_STATE_CLOSING, .send_flags = SEND_ACK },
		   {.valid = 1, .next_state = XIO_CONNECTION_STATE_FIN_WAIT_2, .send_flags = 0 },
		  },
/* FIN_WAIT_2 */ {
		  {.valid = 1, .next_state = XIO_CONNECTION_STATE_TIME_WAIT, .send_flags = SEND_ACK },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* CLOSING */    {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 1, .next_state = XIO_CONNECTION_STATE_TIME_WAIT, .send_flags = 0 },
		 },
/* TIME_WAIT */  {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* CLOSE_WAIT */ {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* LAST_ACK */   {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 1, .next_state = XIO_CONNECTION_STATE_CLOSED, .send_flags = 0  },
		 },
/* CLOSED */	 {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* DISCONNECTED */{
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* ERROR */	 {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		 },
/* INVALID */	  {
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0,},
		  {.valid = 0, .next_state = XIO_CONNECTION_STATE_INVALID, .send_flags = 0 },
		  },
};

/*---------------------------------------------------------------------------*/
/* xio_connection_next_transit					     */
/*---------------------------------------------------------------------------*/
struct xio_transition *xio_connection_next_transit(
				 enum xio_connection_state state, int fin_ack)
{
	return &xio_transition_table[state][fin_ack];
}

char *xio_connection_state_str(enum xio_connection_state state)
{
	switch (state) {
	case XIO_CONNECTION_STATE_INIT:
		return "INIT";
	case XIO_CONNECTION_STATE_ESTABLISHED:
		return "ESTABLISHED";
	case XIO_CONNECTION_STATE_ONLINE:
		return "ONLINE";
	case XIO_CONNECTION_STATE_FIN_WAIT_1:
		return "FIN_WAIT_1";
	case XIO_CONNECTION_STATE_FIN_WAIT_2:
		return "FIN_WAIT_2";
	case XIO_CONNECTION_STATE_CLOSING:
		return "CLOSING";
	case XIO_CONNECTION_STATE_TIME_WAIT:
		return "TIME_WAIT";
	case XIO_CONNECTION_STATE_CLOSE_WAIT:
		return "CLOSE_WAIT";
	case XIO_CONNECTION_STATE_LAST_ACK:
		return "LAST_ACK";
	case XIO_CONNECTION_STATE_CLOSED:
		return "CLOSED";
	case XIO_CONNECTION_STATE_DISCONNECTED:
		return "DISCONNECTED";
	case XIO_CONNECTION_STATE_ERROR:
		return "ERROR";
	case XIO_CONNECTION_STATE_INVALID:
		return "INVALID";
	}

	return NULL;
};

/*---------------------------------------------------------------------------*/
/* xio_get_connection_context						     */
/*---------------------------------------------------------------------------*/
struct xio_context *xio_get_connection_context(
					struct xio_connection *connection)
{
	return connection->ctx;
}

/*---------------------------------------------------------------------------*/
/* xio_is_connection_online						     */
/*---------------------------------------------------------------------------*/
static int xio_is_connection_online(struct xio_connection *connection)
{
	    return connection->session->state == XIO_SESSION_STATE_ONLINE &&
		   connection->state == XIO_CONNECTION_STATE_ONLINE;
}

/*---------------------------------------------------------------------------*/
/* xio_init_ow_msg_pool							     */
/*---------------------------------------------------------------------------*/
static int xio_init_ow_msg_pool(struct xio_connection *connection)
{
	int i;

	connection->msg_array = kcalloc(MSG_POOL_SZ, sizeof(struct xio_msg),
				  GFP_KERNEL);
	if (!connection->msg_array) {
		ERROR_LOG("failed to allocate ow message pool\n");
		xio_set_error(ENOMEM);
		return -1;
	}

	xio_msg_list_init(&connection->one_way_msg_pool);
	for (i = 0; i < MSG_POOL_SZ; i++)
		xio_msg_list_insert_head(&connection->one_way_msg_pool,
					 &connection->msg_array[i], pdata);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_free_ow_msg_pool							     */
/*---------------------------------------------------------------------------*/
static int xio_free_ow_msg_pool(struct xio_connection *connection)
{
	xio_msg_list_init(&connection->one_way_msg_pool);
	kfree(connection->msg_array);
	connection->msg_array = NULL;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_init							     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_connection_init(struct xio_session *session,
					       struct xio_context *ctx,
					       int conn_idx,
					       void *cb_user_context)
{
		struct xio_connection *connection;

		if ((ctx == NULL) || (session == NULL)) {
			xio_set_error(EINVAL);
			return NULL;
		}

		connection = kcalloc(1, sizeof(*connection), GFP_KERNEL);
		if (connection == NULL) {
			xio_set_error(ENOMEM);
			return NULL;
		}

		connection->session	= session;
		connection->conn	= NULL;
		connection->ctx		= ctx;
		connection->conn_idx	= conn_idx;
		connection->in_flight_reqs_budget = XIO_CONNECTION_INFLIGHT_BUDGET;
		connection->in_flight_sends_budget = XIO_CONNECTION_INFLIGHT_BUDGET;
		connection->app_io_budget = XIO_CONNECTION_APP_BUDGET;
		connection->cb_user_context = cb_user_context;
		memcpy(&connection->ses_ops, &session->ses_ops,
		       sizeof(session->ses_ops));

		INIT_LIST_HEAD(&connection->io_tasks_list);
		INIT_LIST_HEAD(&connection->post_io_tasks_list);
		INIT_LIST_HEAD(&connection->pre_send_list);

		xio_msg_list_init(&connection->reqs_msgq);
		xio_msg_list_init(&connection->rsps_msgq);

		xio_msg_list_init(&connection->in_flight_reqs_msgq);
		xio_msg_list_init(&connection->in_flight_rsps_msgq);

		xio_init_ow_msg_pool(connection);

		kref_init(&connection->kref);
		list_add_tail(&connection->ctx_list_entry, &ctx->ctx_list);

		return connection;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_send							     */
/*---------------------------------------------------------------------------*/
int xio_connection_send(struct xio_connection *connection,
			struct xio_msg *msg)
{
	int			retval = 0;
	struct xio_task		*task = NULL;
	struct xio_task		*req_task = NULL;
	struct xio_session_hdr	hdr = {0};
	int			is_req = 0;
	int 			rc = EFAULT;

	/*  control of the number of messages sent */
	if (msg->type == XIO_MSG_TYPE_REQ &&
	    connection->in_flight_reqs_budget == 0)
		return -EAGAIN;

	if (msg->type == XIO_ONE_WAY_REQ &&
	    connection->in_flight_sends_budget == 0)
		return -EAGAIN;

	if (connection->app_io_budget < 0)
		return -EAGAIN;

	if (IS_RESPONSE(msg->type) &&
	    ((msg->flags & (XIO_MSG_RSP_FLAG_FIRST | XIO_MSG_RSP_FLAG_LAST)) ==
	    XIO_MSG_RSP_FLAG_FIRST)) {
		/* this is a receipt message */
		task = xio_conn_get_primary_task(connection->conn);
		if (task == NULL) {
			ERROR_LOG("tasks pool is empty\n");
			return -ENOMEM;
		}
		req_task = container_of(msg->request, struct xio_task, imsg);
		list_move_tail(&task->tasks_list_entry,
			       &connection->pre_send_list);

		task->sender_task	= req_task;
		task->omsg		= msg;
		task->rtid		= req_task->rtid;

		hdr.serial_num		= msg->request->sn;
		hdr.receipt_result	= msg->receipt_res;
		is_req			= 1;
	} else {
		if (IS_REQUEST(msg->type)) {
			task = xio_conn_get_primary_task(connection->conn);
			if (task == NULL) {
				ERROR_LOG("tasks pool is empty\n");
				return -ENOMEM;
			}
			task->omsg	= msg;
			hdr.serial_num	= task->omsg->sn;
			is_req = 1;
			list_move_tail(&task->tasks_list_entry,
				       &connection->pre_send_list);
		} else {
			task = container_of(msg->request,
					    struct xio_task, imsg);

			list_move_tail(&task->tasks_list_entry,
				       &connection->pre_send_list);

			hdr.serial_num	= msg->request->sn;
		}
	}

	/* reset the task mbuf */
	xio_mbuf_reset(&task->mbuf);

	/* set the the mbuf to begining of tlv */
	if (xio_mbuf_tlv_start(&task->mbuf) != 0)
		goto cleanup;


	task->tlv_type		= msg->type;
	task->session		= connection->session;
	task->stag		= uint64_from_ptr(task->session);
	task->conn		= connection->conn;
	task->connection	= connection;
	task->omsg		= msg;
	task->omsg_flags	= msg->flags;

	/* mark as a control message */
	task->is_control = !IS_APPLICATION_MSG(msg);

	/* write session header */
	hdr.flags		= msg->flags;
	hdr.dest_session_id	= connection->session->peer_session_id;
	xio_session_write_header(task, &hdr);

	/* send it */
	retval = xio_conn_send(connection->conn, task);
	if (retval != 0) {
		rc = (retval == -EAGAIN) ? EAGAIN : xio_errno();
		goto cleanup;
	}
	if (!task->is_control) {
		if (msg->type == XIO_MSG_TYPE_REQ)
			connection->in_flight_reqs_budget--;
		if (msg->type == XIO_ONE_WAY_REQ)
			connection->in_flight_sends_budget--;
	}
	return 0;

cleanup:
	if (is_req)
		xio_tasks_pool_put(task);
	else
		list_move(&task->tasks_list_entry, &connection->io_tasks_list);


	return -rc;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_flush_msgs						     */
/*---------------------------------------------------------------------------*/
int xio_connection_flush_msgs(struct xio_connection *connection)
{
	struct xio_msg		*pmsg, *tmp_pmsg, *omsg = NULL;

	if (!xio_msg_list_empty(&connection->reqs_msgq))
		omsg = xio_msg_list_first(&connection->reqs_msgq);
	xio_msg_list_foreach_safe(pmsg, &connection->in_flight_reqs_msgq,
				  tmp_pmsg, pdata) {
		xio_msg_list_remove(&connection->in_flight_reqs_msgq,
				    pmsg, pdata);
		if (omsg)
			xio_msg_list_insert_before(omsg, pmsg, pdata);
		else
			xio_msg_list_insert_tail(&connection->reqs_msgq,
						 pmsg, pdata);
		if (pmsg->type == XIO_MSG_TYPE_REQ)
			connection->in_flight_reqs_budget++;
		if (pmsg->type == XIO_ONE_WAY_REQ)
			connection->in_flight_sends_budget++;
	}

	if (!xio_msg_list_empty(&connection->rsps_msgq))
		omsg = xio_msg_list_first(&connection->rsps_msgq);
	else
		omsg = NULL;

	xio_msg_list_foreach_safe(pmsg, &connection->in_flight_rsps_msgq,
				  tmp_pmsg, pdata) {
		xio_msg_list_remove(&connection->in_flight_rsps_msgq,
				    pmsg, pdata);
		if (omsg)
			xio_msg_list_insert_before(omsg, pmsg, pdata);
		else
			xio_msg_list_insert_tail(&connection->rsps_msgq,
						 pmsg, pdata);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_notify_msgs_flush					     */
/*---------------------------------------------------------------------------*/
int xio_connection_notify_msgs_flush(struct xio_connection *connection)
{
	struct xio_msg		*pmsg, *tmp_pmsg;

	xio_msg_list_foreach_safe(pmsg, &connection->reqs_msgq,
				  tmp_pmsg, pdata) {
		xio_msg_list_remove(&connection->reqs_msgq, pmsg, pdata);
		xio_session_notify_msg_error(connection, pmsg,
					     XIO_E_MSG_FLUSHED);

	}

	xio_msg_list_foreach_safe(pmsg, &connection->rsps_msgq,
				  tmp_pmsg, pdata) {
		xio_msg_list_remove(&connection->rsps_msgq, pmsg, pdata);
		if (pmsg->type == XIO_ONE_WAY_RSP) {
			xio_msg_list_insert_head(
					&connection->one_way_msg_pool,
					pmsg, pdata);
			continue;
		}
		xio_session_notify_msg_error(connection, pmsg,
					     XIO_E_MSG_FLUSHED);
	}

	connection->is_flushed = 1;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_flush_tasks						     */
/*---------------------------------------------------------------------------*/
int xio_connection_flush_tasks(struct xio_connection *connection)
{
	struct xio_task		*ptask, *pnext_task;

	if (!(connection->conn))
		return 0;

	if (!list_empty(&connection->post_io_tasks_list)) {
		TRACE_LOG("post_io_list not empty!\n");
		list_for_each_entry_safe(ptask, pnext_task,
					 &connection->post_io_tasks_list,
					 tasks_list_entry) {
			TRACE_LOG("post_io_list: task %p" \
				  "type 0x%x ltid:%d\n",
				  ptask,
				  ptask->tlv_type, ptask->ltid);
			xio_tasks_pool_put(ptask);
		}
	}

	if (!list_empty(&connection->pre_send_list)) {
		TRACE_LOG("pre_send_list not empty!\n");
		list_for_each_entry_safe(ptask, pnext_task,
					 &connection->pre_send_list,
					 tasks_list_entry) {
			TRACE_LOG("pre_send_list: task %p, " \
				  "type 0x%x ltid:%d\n",
				  ptask,
				  ptask->tlv_type, ptask->ltid);
			if (ptask->sender_task) {
				/* the tx task is returend back to pool */
				xio_tasks_pool_put(ptask->sender_task);
				ptask->sender_task = NULL;
			}
			xio_tasks_pool_put(ptask);
		}
	}

	if (!list_empty(&connection->io_tasks_list)) {
		TRACE_LOG("io_tasks_list not empty!\n");
		list_for_each_entry_safe(ptask, pnext_task,
					 &connection->io_tasks_list,
					 tasks_list_entry) {
			TRACE_LOG("io_tasks_list: task %p, " \
				  "type 0x%x ltid:%d\n",
				  ptask,
				  ptask->tlv_type, ptask->ltid);
		}
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_xmit							     */
/*---------------------------------------------------------------------------*/
static int xio_connection_xmit(struct xio_connection *connection)
{
	struct xio_msg *msg;
	int    retval = 0;
	int    retry_cnt = 0;

	struct xio_msg_list *msg_lists[] = {
		&connection->reqs_msgq,
		&connection->rsps_msgq
	};
	struct xio_msg_list *in_flight_msg_lists[] = {
		&connection->in_flight_reqs_msgq,
		&connection->in_flight_rsps_msgq
	};
	struct xio_msg_list *msgq, *in_flight_msgq;


	while (retry_cnt < 2) {
		msgq		= msg_lists[connection->send_req_toggle];
		in_flight_msgq	=
			in_flight_msg_lists[connection->send_req_toggle];
		connection->send_req_toggle =
			1 - connection->send_req_toggle;
		msg = xio_msg_list_first(msgq);
		if (msg != NULL) {
			retval = xio_connection_send(connection, msg);
			if (retval) {
				if (retval == -EAGAIN) {
					/* if user requested not to
					 * queue messages */
					if (xio_session_not_queueing(
							connection->session)) {
						xio_msg_list_remove(msgq, msg,
								    pdata);
						break;
					}
					retval = 0;
					retry_cnt++;
					continue;
				} else if (retval == -ENOMSG) {
					/* message error was notified */
					TRACE_LOG(
					    "xio_connection_send failed.\n");
					retval = 0;
					/* while error drain the messages */
					retry_cnt = 0;
					continue;
				} else  {
					xio_msg_list_remove(msgq, msg, pdata);
					break;
				}
			} else {
				retry_cnt = 0;
				xio_msg_list_remove(msgq, msg, pdata);
				if (IS_APPLICATION_MSG(msg)) {
					xio_msg_list_insert_tail(
							in_flight_msgq, msg,
							pdata);

				}
			}
		} else {
			retry_cnt++;
		}
	}

	if (retval != 0) {
		xio_set_error(-retval);
		ERROR_LOG("failed to send message - %s\n",
			  xio_strerror(-retval));
	}

	return retval ? -1 : 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_remove_in_flight					     */
/*---------------------------------------------------------------------------*/
int xio_connection_remove_in_flight(struct xio_connection *connection,
				    struct xio_msg *msg)
{
	if (!IS_APPLICATION_MSG(msg))
		return 0;

	if (IS_REQUEST(msg->type)) {
		xio_msg_list_remove(
				&connection->in_flight_reqs_msgq, msg, pdata);
		if (msg->type == XIO_MSG_TYPE_REQ)
			connection->in_flight_reqs_budget++;
		if (msg->type == XIO_ONE_WAY_REQ)
			connection->in_flight_sends_budget++;
	} else {
		xio_msg_list_remove(
				&connection->in_flight_rsps_msgq, msg, pdata);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_remove_msg_from_queue					     */
/*---------------------------------------------------------------------------*/
int xio_connection_remove_msg_from_queue(struct xio_connection *connection,
					 struct xio_msg *msg)
{
	if (!IS_APPLICATION_MSG(msg))
		return 0;

	if (IS_REQUEST(msg->type))
		xio_msg_list_remove(
				&connection->reqs_msgq, msg, pdata);
	else
		xio_msg_list_remove(
				&connection->rsps_msgq, msg, pdata);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_send_request							     */
/*---------------------------------------------------------------------------*/
int xio_send_request(struct xio_connection *connection,
		     struct xio_msg *msg)
{
	int			valid;
	struct xio_statistics	*stats;
	struct xio_vmsg		*vmsg;
	struct xio_msg		*pmsg;

	if (connection  == NULL || msg == NULL) {
		xio_set_error(EINVAL);
		return -1;
	}


	if (unlikely(xio_session_not_queueing(connection->session) &&
		     !xio_is_connection_online(connection))) {
		xio_set_error(EAGAIN);
		return -1;
	}

	pmsg = msg;
	stats = &connection->ctx->stats;
	while (pmsg) {
		valid = xio_session_is_valid_in_req(connection->session, pmsg);
		if (!valid) {
			xio_set_error(EINVAL);
			ERROR_LOG("invalid in message\n");
			return -1;
		}
		valid = xio_session_is_valid_out_msg(connection->session, pmsg);
		if (!valid) {
			xio_set_error(EINVAL);
			ERROR_LOG("invalid out message\n");
			return -1;
		}

		if (unlikely((connection->state != XIO_CONNECTION_STATE_ONLINE &&
			     connection->state != XIO_CONNECTION_STATE_ESTABLISHED &&
			     connection->state != XIO_CONNECTION_STATE_INIT) ||
			     connection->in_close)) {
			xio_set_error(ESHUTDOWN);
			xio_session_notify_msg_error(connection, pmsg,
						     XIO_E_MSG_FLUSHED);
			if (pmsg->next == NULL) {
				pmsg = pmsg->next;
				continue;
			} else {
				return -1;
			}
		}

		vmsg = &pmsg->out;
		pmsg->timestamp = get_cycles();
		xio_stat_inc(stats, XIO_STAT_TX_MSG);
		xio_stat_add(stats, XIO_STAT_TX_BYTES,
			     vmsg->header.iov_len +
			     xio_iovex_length(vmsg->data_iov,
					      vmsg->data_iovlen));

		pmsg->sn = xio_session_get_sn(connection->session);
		pmsg->type = XIO_MSG_TYPE_REQ;

		xio_msg_list_insert_tail(&connection->reqs_msgq, pmsg, pdata);

		pmsg = pmsg->next;
	}

	/* do not xmit until connection is assigned */
	if (xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_send_response							     */
/*---------------------------------------------------------------------------*/
int xio_send_response(struct xio_msg *msg)
{
	struct xio_task		*task;
	struct xio_connection	*connection = NULL;
	struct xio_statistics	*stats;
	struct xio_vmsg		*vmsg;
	struct xio_msg		*pmsg = msg;
	int			valid;

	while (pmsg) {
		task	   = container_of(msg->request, struct xio_task, imsg);
		connection = task->connection;
		stats	   = &connection->ctx->stats;
		vmsg	   = &msg->out;

		if (task->imsg.sn != msg->request->sn) {
			ERROR_LOG("match not found: request sn:%llu, " \
				  "response sn:%llu\n",
				  task->imsg.sn, msg->request->sn);
			xio_set_error(EINVAL);
			return -1;
		}
		connection->app_io_budget++;

		if (unlikely(
		     (connection->state != XIO_CONNECTION_STATE_ONLINE  &&
		     connection->state != XIO_CONNECTION_STATE_ESTABLISHED &&
		     connection->state != XIO_CONNECTION_STATE_INIT) ||
		     connection->in_close)) {
			xio_set_error(ESHUTDOWN);
			xio_tasks_pool_put(task);
			xio_session_notify_msg_error(connection, pmsg,
						     XIO_E_MSG_FLUSHED);
			if (pmsg->next == NULL) {
				pmsg = pmsg->next;
				continue;
			} else {
				return -1;
			}
		}

		if (unlikely((xio_session_not_queueing(connection->session) &&
			      !xio_is_connection_online(connection)))) {
			xio_set_error(EAGAIN);
			return -1;
		}

		/* Server latency */
		xio_stat_add(stats, XIO_STAT_APPDELAY,
			     get_cycles() - task->imsg.timestamp);


		valid = xio_session_is_valid_out_msg(connection->session, pmsg);
		if (!valid) {
			xio_set_error(EINVAL);
			ERROR_LOG("invalid out message\n");
			return -1;
		}


		xio_stat_inc(stats, XIO_STAT_TX_MSG);
		xio_stat_add(stats, XIO_STAT_TX_BYTES,
			     vmsg->header.iov_len +
			     xio_iovex_length(vmsg->data_iov,
					      vmsg->data_iovlen));

		pmsg->flags = XIO_MSG_RSP_FLAG_LAST;
		if ((pmsg->request->flags &
		     XIO_MSG_FLAG_REQUEST_READ_RECEIPT) &&
		    (task->state == XIO_TASK_STATE_DELIVERED))
			pmsg->flags |= XIO_MSG_RSP_FLAG_FIRST;
		task->state = XIO_TASK_STATE_READ;

		pmsg->type = XIO_MSG_TYPE_RSP;

		xio_msg_list_insert_tail(&connection->rsps_msgq, pmsg, pdata);

		pmsg = pmsg->next;
	}
	/* do not xmit until connection is assigned */
	if (connection && xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_send_read_receipt					     */
/*---------------------------------------------------------------------------*/
int xio_connection_send_read_receipt(struct xio_connection *connection,
				     struct xio_msg *msg)
{
	struct xio_msg *rsp;
	struct xio_task *task;


	if (xio_msg_list_empty(&connection->one_way_msg_pool)) {
		xio_set_error(ENOMEM);
		ERROR_LOG("one way msg pool is empty\n");
		return -1;
	}
	task = container_of(msg, struct xio_task, imsg);

	rsp = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, rsp, pdata);

	rsp->type = (msg->type & ~XIO_REQUEST) | XIO_RESPONSE;
	rsp->request = msg;

	rsp->flags = XIO_MSG_RSP_FLAG_FIRST;
	task->state = XIO_TASK_STATE_READ;

	rsp->out.header.iov_len = 0;
	rsp->out.data_iovlen = 0;

	xio_msg_list_insert_tail(&connection->rsps_msgq, rsp, pdata);

	/* do not xmit until connection is assigned */
	if (xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

int xio_connection_release_read_receipt(struct xio_connection *connection,
				  struct xio_msg *msg)
{
	xio_msg_list_insert_head(&connection->one_way_msg_pool, msg, pdata);
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_send_msg								     */
/*---------------------------------------------------------------------------*/
int xio_send_msg(struct xio_connection *connection,
		 struct xio_msg *msg)
{
	struct xio_statistics	*stats = &connection->ctx->stats;
	struct xio_vmsg		*vmsg;
	struct xio_msg		*pmsg = msg;
	int			valid;


	if (xio_session_not_queueing(connection->session) &&
	    (connection->state != XIO_CONNECTION_STATE_ONLINE)) {
		xio_set_error(EAGAIN);
		return -1;
	}

	while (pmsg) {
		valid = xio_session_is_valid_out_msg(connection->session, pmsg);
		if (!valid) {
			xio_set_error(EINVAL);
			ERROR_LOG("invalid out message\n");
			return -1;
		}
		if (unlikely((connection->state != XIO_CONNECTION_STATE_ONLINE &&
			     connection->state != XIO_CONNECTION_STATE_ESTABLISHED &&
			     connection->state != XIO_CONNECTION_STATE_INIT) ||
			     connection->in_close)) {
			xio_set_error(ESHUTDOWN);
			xio_session_notify_msg_error(connection, pmsg,
						     XIO_E_MSG_FLUSHED);
			if (pmsg->next == NULL) {
				pmsg = pmsg->next;
				continue;
			} else {
				return -1;
			}
		}

		vmsg	= &pmsg->out;
		pmsg->timestamp = get_cycles();
		xio_stat_inc(stats, XIO_STAT_TX_MSG);
		xio_stat_add(stats, XIO_STAT_TX_BYTES,
			     vmsg->header.iov_len +
			     xio_iovex_length(vmsg->data_iov,
			     vmsg->data_iovlen));

		pmsg->sn = xio_session_get_sn(connection->session);
		pmsg->type = XIO_ONE_WAY_REQ;

		xio_msg_list_insert_tail(&connection->reqs_msgq, pmsg, pdata);

		pmsg = pmsg->next;
	}

	/* do not xmit until connection is assigned */
	if (xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_xmit_msgs						     */
/*---------------------------------------------------------------------------*/
int xio_connection_xmit_msgs(struct xio_connection *connection)
{
	if (connection->state == XIO_CONNECTION_STATE_ONLINE ||
	    connection->state == XIO_CONNECTION_STATE_FIN_WAIT_1) {
		return xio_connection_xmit(connection);
	} else if (xio_session_not_queueing(connection->session)) {
		xio_set_error(EAGAIN);
		return -1;
	}

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_close							     */
/*---------------------------------------------------------------------------*/
static void xio_connection_release(struct kref *kref)
{
	struct xio_connection *connection = container_of(kref,
							 struct xio_connection,
							 kref);

	if (xio_is_work_pending(&connection->hello_work))
		xio_ctx_del_work(connection->ctx,
				 &connection->hello_work);

	if (xio_is_delayed_work_pending(&connection->fin_delayed_work))
		xio_ctx_del_delayed_work(connection->ctx,
					&connection->fin_delayed_work);

	if (xio_is_work_pending(&connection->fin_work))
		xio_ctx_del_work(connection->ctx,
				 &connection->fin_work);

	xio_free_ow_msg_pool(connection);
	list_del(&connection->ctx_list_entry);

	kfree(connection);
}

/*---------------------------------------------------------------------------*/
/* xio_connection_close							     */
/*---------------------------------------------------------------------------*/
int xio_connection_close(struct xio_connection *connection)
{
	kref_put(&connection->kref, xio_connection_release);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_queue_io_task						     */
/*---------------------------------------------------------------------------*/
void xio_connection_queue_io_task(struct xio_connection *connection,
				    struct xio_task *task)
{
	if (task->tlv_type != XIO_ONE_WAY_RSP)
		connection->app_io_budget--;
	list_move_tail(&task->tasks_list_entry, &connection->io_tasks_list);
}

/*---------------------------------------------------------------------------*/
/* xio_release_response_task						     */
/*---------------------------------------------------------------------------*/
void xio_release_response_task(struct xio_task *task)
{
	/* the tx task is returned back to pool */
	if (task->sender_task) {
		xio_tasks_pool_put(task->sender_task);
		task->sender_task = NULL;
	}

	/* the rx task is returned back to pool */
	xio_tasks_pool_put(task);
}

/*---------------------------------------------------------------------------*/
/* xio_release_response							     */
/*---------------------------------------------------------------------------*/
int xio_release_response(struct xio_msg *msg)
{
	struct xio_task		*task;
	struct xio_connection	*connection = NULL;
	struct xio_msg		*pmsg = msg;


	while (pmsg) {
		task = container_of(pmsg->request, struct xio_task, imsg);
		if (task->sender_task == NULL) {
			/* do not release response in responder */
			xio_set_error(EINVAL);
			return -1;
		}
		connection = task->connection;
		connection->app_io_budget++;
		list_move_tail(&task->tasks_list_entry,
			       &connection->post_io_tasks_list);


		xio_release_response_task(task);

		pmsg = pmsg->next;
	}
	if (connection && xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_release_msg							     */
/*---------------------------------------------------------------------------*/
int xio_release_msg(struct xio_msg *msg)
{
	struct xio_task		*task;
	struct xio_connection	*connection = NULL;
	struct xio_msg		*pmsg = msg;

	while (pmsg) {
		task = container_of(pmsg, struct xio_task, imsg);
		if (task->tlv_type != XIO_ONE_WAY_REQ) {
			ERROR_LOG("xio_release_msg failed. invalid type:0x%x\n",
				  task->tlv_type);
			xio_set_error(EINVAL);
			return -1;
		}

		connection = task->connection;
		connection->app_io_budget++;
		list_move_tail(&task->tasks_list_entry,
			       &connection->post_io_tasks_list);

		pmsg = pmsg->next;

		/* the rx task is returned back to pool */
		xio_tasks_pool_put(task);
	}

	if (connection && xio_is_connection_online(connection))
		return xio_connection_xmit(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_poll_completions							     */
/*---------------------------------------------------------------------------*/
int xio_poll_completions(struct xio_connection *connection,
			long min_nr, long nr,
			 struct timespec *timeout)
{
	if (connection->conn)
		return xio_conn_poll(connection->conn, min_nr, nr, timeout);
	else
		return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_send_fin_req							     */
/*---------------------------------------------------------------------------*/
static int xio_send_fin_req(struct xio_connection *connection)
{
	struct xio_msg *msg;

	msg = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, msg, pdata);

	msg->type		= XIO_FIN_REQ;
	msg->in.header.iov_len	= 0;
	msg->out.header.iov_len	= 0;
	msg->in.data_iovlen	= 0;
	msg->out.data_iovlen	= 0;


	/* insert to the tail of the queue */
	xio_msg_list_insert_tail(&connection->reqs_msgq, msg, pdata);

	TRACE_LOG("send fin request. session:%p, connection:%p\n",
		  connection->session, connection);

	/* do not xmit until connection is assigned */
	return xio_connection_xmit(connection);
}

/*---------------------------------------------------------------------------*/
/* xio_send_fin_ack							     */
/*---------------------------------------------------------------------------*/
int xio_send_fin_ack(struct xio_connection *connection, struct xio_task *task)
{
	struct xio_msg *msg;

	msg = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, msg, pdata);


	msg->type		= XIO_FIN_RSP;
	msg->request		= &task->imsg;
	msg->in.header.iov_len	= 0;
	msg->out.header.iov_len	= 0;
	msg->in.data_iovlen	= 0;
	msg->out.data_iovlen	= 0;

	/* insert to the tail of the queue */
	xio_msg_list_insert_tail(&connection->rsps_msgq, msg, pdata);

	TRACE_LOG("send fin response. session:%p, connection:%p\n",
		  connection->session, connection);

	/* status is not important - just send */
	return xio_connection_xmit(connection);
}

/*---------------------------------------------------------------------------*/
/* xio_connection_release_fin						     */
/*---------------------------------------------------------------------------*/
int xio_connection_release_fin(struct xio_connection *connection,
			       struct xio_msg *msg)
{
	xio_msg_list_insert_head(&connection->one_way_msg_pool, msg, pdata);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_disconnect_initial_connection					     */
/*---------------------------------------------------------------------------*/
int xio_disconnect_initial_connection(struct xio_connection *connection)
{
	struct xio_msg *msg;
	int		retval;

	msg = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, msg, pdata);

	msg->type		= XIO_FIN_REQ;
	msg->in.header.iov_len	= 0;
	msg->out.header.iov_len	= 0;
	msg->in.data_iovlen	= 0;
	msg->out.data_iovlen	= 0;

	TRACE_LOG("send fin request. session:%p, connection:%p\n",
		  connection->session, connection);

	TRACE_LOG("connection %p state change: current_state:%s, " \
		  "next_state:%s\n",
		  connection,
		  xio_connection_state_str(connection->state),
		  xio_connection_state_str(XIO_CONNECTION_STATE_FIN_WAIT_1));

	connection->state = XIO_CONNECTION_STATE_FIN_WAIT_1;
	/* we don't want to send all queued messages yet - send directly */
	retval = xio_connection_send(connection, msg);
	if (retval == -EAGAIN)
		retval = 0;

	if (!connection->disable_notify)
		xio_session_notify_connection_closed(connection->session,
						     connection);
	return  retval;
}

static void xio_pre_disconnect(void *_connection)
{
	struct xio_connection *connection = _connection;

	/* now we are on the right context, reaffirm that in the mean time, state
	 * was not changed
	 */
	if (connection->state != XIO_CONNECTION_STATE_ONLINE)
		return;

	connection->state = XIO_CONNECTION_STATE_FIN_WAIT_1;
	xio_send_fin_req(connection);

	if (!connection->disable_notify) {
		connection->close_reason = XIO_E_SESSION_DISCONECTED;
		xio_session_notify_connection_closed(connection->session,
						     connection);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_disconnect							     */
/*---------------------------------------------------------------------------*/
int xio_disconnect(struct xio_connection *connection)
{
	int retval;

	/* active close state machine */

	if (!connection || !connection->session) {
		xio_set_error(EINVAL);
		ERROR_LOG("xio_disconnect failed %m\n");
		return -1;
	}
	if (connection->state != XIO_CONNECTION_STATE_ONLINE ||
	    connection->in_close) {
		return 0;
	}
	connection->in_close = 1;
	retval = xio_ctx_add_work(
			connection->ctx,
			connection,
			xio_pre_disconnect,
			&connection->fin_work);
	if (retval != 0) {
		ERROR_LOG("xio_ctx_timer_add failed.\n");
		return retval;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_cancel_request							     */
/*---------------------------------------------------------------------------*/
int xio_cancel_request(struct xio_connection *connection,
		       struct xio_msg *req)
{
	struct xio_msg *pmsg, *tmp_pmsg;
	uint64_t	stag;
	struct xio_session_cancel_hdr hdr;


	/* search the tx */
	xio_msg_list_foreach_safe(pmsg, &connection->reqs_msgq,
				  tmp_pmsg, pdata) {
		if (pmsg->sn == req->sn) {
			ERROR_LOG("[%llu] - message found on reqs_msgq\n",
				  req->sn);
			xio_msg_list_remove(&connection->reqs_msgq,
					    pmsg, pdata);
			xio_session_notify_cancel(
				connection, pmsg, XIO_E_MSG_CANCELED);
			return 0;
		}
	}
	hdr.sn			 = htonll(req->sn);
	hdr.requester_session_id =
		htonl(connection->session->session_id);
	hdr.responder_session_id =
		htonl(connection->session->peer_session_id);
	stag			 =
		uint64_from_ptr(connection->session);

	/* cancel request on tx */
	xio_conn_cancel_req(connection->conn, req, stag, &hdr, sizeof(hdr));

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_send_cancel_response					     */
/*---------------------------------------------------------------------------*/
int xio_connection_send_cancel_response(struct xio_connection *connection,
					struct xio_msg *msg,
					struct xio_task *task,
					enum xio_status result)
{
	struct xio_session_cancel_hdr hdr;

	hdr.sn			= htonll(msg->sn);
	hdr.responder_session_id = htonl(connection->session->session_id);
	hdr.requester_session_id = htonl(connection->session->peer_session_id);

	xio_conn_cancel_rsp(connection->conn, task, result, &hdr, sizeof(hdr));

	return 0;
}

struct xio_task *xio_connection_find_io_task(struct xio_connection *connection,
					     uint64_t msg_sn)
{
	struct xio_task *ptask;

	/* look in the tx_comp */
	list_for_each_entry(ptask, &connection->io_tasks_list,
			    tasks_list_entry) {
		if (ptask->imsg.sn == msg_sn)
			return ptask;
	}

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_cancel								     */
/*---------------------------------------------------------------------------*/
int xio_cancel(struct xio_msg *req, enum xio_status result)
{
	struct xio_task *task;

	if (result != XIO_E_MSG_CANCELED && result != XIO_E_MSG_CANCEL_FAILED) {
		xio_set_error(EINVAL);
		ERROR_LOG("invalid status\n");
		return -1;
	}

	task = container_of(req, struct xio_task, imsg);
	xio_connection_send_cancel_response(task->connection, &task->imsg,
					    task, result);
	/* release the message */
	if (result == XIO_E_MSG_CANCELED) {
		/* the rx task is returned back to pool */
		xio_tasks_pool_put(task);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_modify_connection						     */
/*---------------------------------------------------------------------------*/
int xio_modify_connection(struct xio_connection *connection,
		       struct xio_connection_attr *attr,
		       int attr_mask)
{
	if (!connection || !attr) {
		xio_set_error(EINVAL);
		ERROR_LOG("invalid parameters\n");
		return -1;
	}

	if (attr_mask & XIO_CONNECTION_ATTR_USER_CTX)
		connection->cb_user_context = attr->user_context;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_query_connection							     */
/*---------------------------------------------------------------------------*/
int xio_query_connection(struct xio_connection *connection,
		       struct xio_connection_attr *attr,
		       int attr_mask)
{
	if (!connection || !attr) {
		xio_set_error(EINVAL);
		ERROR_LOG("invalid parameters\n");
		return -1;
	}
	if (attr_mask & XIO_CONNECTION_ATTR_USER_CTX)
		attr->user_context = connection->cb_user_context;

	if (attr_mask & XIO_CONNECTION_ATTR_CTX)
		attr->ctx = connection->ctx;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_send_hello_req					     */
/*---------------------------------------------------------------------------*/
int xio_connection_send_hello_req(struct xio_connection *connection)
{
	struct xio_msg *msg;
	int		retval;

	TRACE_LOG("send hello request. session:%p, connection:%p\n",
		  connection->session, connection);

	msg = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, msg, pdata);

	msg->type		= XIO_CONNECTION_HELLO_REQ;
	msg->in.header.iov_len	= 0;
	msg->out.header.iov_len	= 0;
	msg->in.data_iovlen	= 0;
	msg->out.data_iovlen	= 0;

	/* we don't want to send all queued messages yet - send directly */
	retval = xio_connection_send(connection, msg);
	if (retval == -EAGAIN)
		retval = 0;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_send_hello_rsp					     */
/*---------------------------------------------------------------------------*/
int xio_connection_send_hello_rsp(struct xio_connection *connection,
				  struct xio_task *task)
{
	struct xio_msg	*msg;
	int		retval;

	TRACE_LOG("send hello response. session:%p, connection:%p\n",
		  connection->session, connection);

	msg = xio_msg_list_first(&connection->one_way_msg_pool);
	xio_msg_list_remove(&connection->one_way_msg_pool, msg, pdata);


	msg->type		= XIO_CONNECTION_HELLO_RSP;
	msg->request		= &task->imsg;
	msg->in.header.iov_len	= 0;
	msg->out.header.iov_len	= 0;
	msg->in.data_iovlen	= 0;
	msg->out.data_iovlen	= 0;


	/* we don't want to send all queued messages yet - send directly */
	retval = xio_connection_send(connection, msg);
	if (retval == -EAGAIN)
		retval = 0;

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_release_hello						     */
/*---------------------------------------------------------------------------*/
int xio_connection_release_hello(struct xio_connection *connection,
				 struct xio_msg *msg)
{
	xio_msg_list_insert_head(&connection->one_way_msg_pool, msg, pdata);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_post_destroy						     */
/*---------------------------------------------------------------------------*/
int xio_connection_post_destroy(struct xio_connection *connection)
{
	int			retval;
	int			reason;
	struct xio_session	*session;
	int			destroy_session = 0;
	int			state;
	int			close_reason;

	if (connection == NULL) {
		xio_set_error(EINVAL);
		return -1;
	}
	session = connection->session;
	state = session->state;
	close_reason = connection->close_reason;

	DEBUG_LOG("xio_connection_post_destroy. session:%p, connection:%p " \
		  "conn:%p nr:%d\n",
		  session, connection, connection->conn,
		  session->connections_nr);


	xio_connection_flush_tasks(connection);

	/* remove the connection from the session's connections list */
	if (connection->conn)
		xio_conn_close(connection->conn, &session->observer);

	/* leading connection */
	if (session->lead_connection &&
	    session->lead_connection->conn == connection->conn) {
		retval = xio_connection_close(session->lead_connection);
		session->lead_connection = NULL;
		TRACE_LOG("lead connection is closed\n");
	} else if (session->redir_connection &&
		   session->redir_connection->conn == connection->conn) {
		retval = xio_connection_close(session->redir_connection);
		session->redir_connection = NULL;
		TRACE_LOG("redirected connection is closed\n");
	} else {
		spin_lock(&session->connections_list_lock);
		if (session->connections_nr == 1)  {
			session->state = XIO_SESSION_STATE_CLOSING;
			destroy_session = 1;
		}
		session->connections_nr--;
		list_del(&connection->connections_list_entry);
		spin_unlock(&session->connections_list_lock);
		retval = xio_connection_close(connection);
	}
	if (retval != 0) {
		ERROR_LOG("failed to close connection");
		return -1;
	}
	if (session->disable_teardown)
		return 0;

	if (destroy_session) {
		switch (state) {
		case XIO_SESSION_STATE_REJECTED:
			if (session->type == XIO_SESSION_SERVER)
				xio_session_destroy(session);
			else
				xio_session_notify_rejected(session);
			return 0;
		case XIO_SESSION_STATE_ACCEPTED:
			if (session->type == XIO_SESSION_SERVER)
				reason = XIO_E_SESSION_DISCONECTED;
			else
				reason = XIO_E_SESSION_REFUSED;
			break;
		default:
			reason = close_reason;
			break;
		}
		xio_session_notify_teardown(
			session,
			reason);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_destroy						     */
/*---------------------------------------------------------------------------*/
int xio_connection_destroy(struct xio_connection *connection)
{
	int			retval = 0;
	struct xio_session	*session;

	if (connection == NULL) {
		xio_set_error(EINVAL);
		return -1;
	}
	session = connection->session;

	DEBUG_LOG("xio_connection_destroy. session:%p, connection:%p " \
		  "conn:%p nr:%d\n",
		  session, connection, connection->conn,
		  session->connections_nr);

	if (connection->state == XIO_CONNECTION_STATE_CLOSE_WAIT)  {
		retval = xio_send_fin_req(connection);
		DEBUG_LOG("connection %p state change: current_state:%s, " \
			  "next_state:%s\n",
			  connection,
			  xio_connection_state_str(connection->state),
			  xio_connection_state_str(
				  XIO_CONNECTION_STATE_LAST_ACK));
		connection->state = XIO_CONNECTION_STATE_LAST_ACK;
	} else {
		DEBUG_LOG("connection:%p, state:%s\n", connection,
			  xio_connection_state_str(connection->state));
		retval = xio_connection_post_destroy(connection);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_disconnected						     */
/*---------------------------------------------------------------------------*/
int xio_connection_disconnected(struct xio_connection *connection)
{
	connection->state	 = XIO_CONNECTION_STATE_DISCONNECTED;

	xio_session_notify_connection_disconnected(
			connection->session, connection,
			connection->close_reason);

	if (connection->conn)
		xio_conn_close(connection->conn,
			       &connection->session->observer);

	/* flush all messages from in flight message queue to in queue */
	xio_connection_flush_msgs(connection);

	/* flush all messages back to user */
	xio_connection_notify_msgs_flush(connection);

	xio_session_notify_connection_teardown(connection->session, connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_refused						     */
/*---------------------------------------------------------------------------*/
int xio_connection_refused(struct xio_connection *connection)
{
	connection->state	 = XIO_CONNECTION_STATE_DISCONNECTED;
	connection->close_reason = XIO_E_CONNECT_ERROR;

	xio_session_notify_connection_refused(
			connection->session, connection,
			XIO_E_CONNECT_ERROR);

	/* flush all messages from in flight message queue to in queue */
	xio_connection_flush_msgs(connection);

	/* flush all messages back to user */
	xio_connection_notify_msgs_flush(connection);

	xio_session_notify_connection_teardown(connection->session, connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_error_event						     */
/*---------------------------------------------------------------------------*/
int xio_connection_error_event(struct xio_connection *connection,
			       enum xio_status reason)
{
	connection->state	 = XIO_CONNECTION_STATE_ERROR;
	connection->close_reason = reason;

	xio_session_notify_connection_error(connection->session, connection,
					    reason);

	/* flush all messages from in flight message queue to in queue */
	xio_connection_flush_msgs(connection);

	/* flush all messages back to user */
	xio_connection_notify_msgs_flush(connection);

	xio_session_notify_connection_teardown(connection->session, connection);

	return 0;
}
