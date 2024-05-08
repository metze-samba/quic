// SPDX-License-Identifier: GPL-2.0-or-later
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Initialization/cleanup for QUIC protocol support.
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include "socket.h"

static void quic_outq_transmit_ctrl(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *tmp;
	struct list_head *head;
	int ret;

	head =  &outq->control_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (!quic_crypto_send_ready(quic_crypto(sk, frame->level)))
			break;
		ret = quic_packet_config(sk, frame->level, frame->path_alt);
		if (ret) { /* filtered out this frame */
			if (ret > 0)
				continue;
			break;
		}
		if (quic_packet_tail(sk, frame, 0))
			continue; /* packed and conintue with the next frame */
		quic_packet_create(sk); /* build and xmit the packed frames */
		tmp = frame; /* go back but still pack the current frame */
	}
}

static void quic_outq_transmit_dgram(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *tmp;
	u8 level = outq->data_level;
	struct list_head *head;
	int ret;

	if (!quic_crypto_send_ready(quic_crypto(sk, level)))
		return;

	head =  &outq->datagram_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (outq->data_inflight + frame->len > outq->window)
			break;
		ret = quic_packet_config(sk, level, frame->path_alt);
		if (ret) {
			if (ret > 0)
				continue;
			break;
		}
		if (quic_packet_tail(sk, frame, 1)) {
			outq->data_inflight += frame->bytes;
			continue;
		}
		quic_packet_create(sk);
		tmp = frame;
	}
}

static int quic_outq_flow_control(struct sock *sk, struct quic_frame *frame)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *nframe = NULL;
	u32 len = frame->bytes;
	struct quic_stream *stream;
	u8 blocked = 0;

	/* congestion control */
	if (outq->data_inflight + len > outq->window)
		blocked = 1;

	/* send flow control */
	stream = frame->stream;
	if (stream->send.bytes + len > stream->send.max_bytes) {
		if (!stream->send.data_blocked &&
		    stream->send.last_max_bytes < stream->send.max_bytes) {
			nframe = quic_frame_create(sk, QUIC_FRAME_STREAM_DATA_BLOCKED, stream);
			if (nframe)
				quic_outq_ctrl_tail(sk, nframe, true);
			stream->send.last_max_bytes = stream->send.max_bytes;
			stream->send.data_blocked = 1;
		}
		blocked = 1;
	}
	if (outq->bytes + len > outq->max_bytes) {
		if (!outq->data_blocked && outq->last_max_bytes < outq->max_bytes) {
			nframe = quic_frame_create(sk, QUIC_FRAME_DATA_BLOCKED, outq);
			if (nframe)
				quic_outq_ctrl_tail(sk, nframe, true);
			outq->last_max_bytes = outq->max_bytes;
			outq->data_blocked = 1;
		}
		blocked = 1;
	}

	if (nframe)
		quic_outq_transmit_ctrl(sk);
	return blocked;
}

static void quic_outq_transmit_stream(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *tmp;
	u8 level = outq->data_level;
	struct list_head *head;
	int ret;

	if (!quic_crypto_send_ready(quic_crypto(sk, level)))
		return;

	head = &outq->stream_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (!level && quic_outq_flow_control(sk, frame))
			break;
		ret = quic_packet_config(sk, level, frame->path_alt);
		if (ret) {
			if (ret > 0)
				continue;
			break;
		}
		if (quic_packet_tail(sk, frame, 0)) {
			frame->stream->send.frags++;
			frame->stream->send.bytes += frame->bytes;
			outq->bytes += frame->bytes;
			outq->data_inflight += frame->bytes;
			continue;
		}
		quic_packet_create(sk);
		tmp = frame;
	}
}

/* pack and transmit frames from outqueue */
int quic_outq_transmit(struct sock *sk)
{
	quic_outq_transmit_ctrl(sk);

	quic_outq_transmit_dgram(sk);

	quic_outq_transmit_stream(sk);

	return quic_packet_flush(sk);
}

void quic_outq_wfree(int len, struct sock *sk)
{
	if (!len)
		return;

	WARN_ON(refcount_sub_and_test(len, &sk->sk_wmem_alloc));
	sk_wmem_queued_add(sk, -len);
	sk_mem_uncharge(sk, len);

	if (sk_stream_wspace(sk) > 0)
		sk->sk_write_space(sk);
}

void quic_outq_set_owner_w(int len, struct sock *sk)
{
	if (!len)
		return;

	refcount_add(len, &sk->sk_wmem_alloc);
	sk_wmem_queued_add(sk, len);
	sk_mem_charge(sk, len);
}

void quic_outq_stream_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	struct quic_stream_table *streams = quic_streams(sk);
	struct quic_stream *stream = frame->stream;

	if (stream->send.state == QUIC_STREAM_SEND_STATE_READY)
		stream->send.state = QUIC_STREAM_SEND_STATE_SEND;

	if (frame->type & QUIC_STREAM_BIT_FIN &&
	    stream->send.state == QUIC_STREAM_SEND_STATE_SEND) {
		if (quic_stream_send_active(streams) == stream->id)
			quic_stream_set_send_active(streams, -1);
		stream->send.state = QUIC_STREAM_SEND_STATE_SENT;
	}

	list_add_tail(&frame->list, &quic_outq(sk)->stream_list);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_dgram_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	list_add_tail(&frame->list, &quic_outq(sk)->datagram_list);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_ctrl_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	struct list_head *head = &quic_outq(sk)->control_list;
	struct quic_frame *pos;

	if (frame->level) { /* prioritize handshake frames */
		list_for_each_entry(pos, head, list) {
			if (!pos->level) {
				head = &pos->list;
				break;
			}
		}
	}
	list_add_tail(&frame->list, head);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_transmitted_tail(struct sock *sk, struct quic_frame *frame)
{
	struct list_head *head = &quic_outq(sk)->transmitted_list;
	struct quic_frame *pos;

	if (frame->level) { /* prioritize handshake frames */
		list_for_each_entry(pos, head, list) {
			if (!pos->level) {
				head = &pos->list;
				break;
			}
		}
	}
	list_add_tail(&frame->list, head);
}

void quic_outq_transmit_probe(struct sock *sk)
{
	struct quic_path_dst *d = (struct quic_path_dst *)quic_dst(sk);
	struct quic_pnmap *pnmap = quic_pnmap(sk, QUIC_CRYPTO_APP);
	u8 taglen = quic_packet_taglen(quic_packet(sk));
	struct quic_inqueue *inq = quic_inq(sk);
	struct quic_frame *frame;
	u32 pathmtu;
	s64 number;

	if (!quic_is_established(sk))
		return;

	frame = quic_frame_create(sk, QUIC_FRAME_PING, &d->pl.probe_size);
	if (frame) {
		number = quic_pnmap_next_number(pnmap);
		quic_outq_ctrl_tail(sk, frame, false);

		pathmtu = quic_path_pl_send(quic_dst(sk), number);
		if (pathmtu)
			quic_packet_mss_update(sk, pathmtu + taglen);
	}

	quic_timer_reset(sk, QUIC_TIMER_PATH, quic_inq_probe_timeout(inq));
}

void quic_outq_transmit_close(struct sock *sk, u8 type, u32 errcode, u8 level)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_connection_close close = {};
	struct quic_frame *frame;

	if (!errcode)
		return;

	close.errcode = errcode;
	close.frame = type;
	if (quic_inq_event_recv(sk, QUIC_EVENT_CONNECTION_CLOSE, &close))
		return;

	quic_outq_set_close_errcode(outq, errcode);
	quic_outq_set_close_frame(outq, type);

	frame = quic_frame_create(sk, QUIC_FRAME_CONNECTION_CLOSE, NULL);
	if (frame) {
		frame->level = level;
		quic_outq_ctrl_tail(sk, frame, false);
	}
	quic_set_state(sk, QUIC_SS_CLOSED);
}

void quic_outq_transmit_app_close(struct sock *sk)
{
	u32 errcode = QUIC_TRANSPORT_ERROR_APPLICATION;
	u8 type = QUIC_FRAME_CONNECTION_CLOSE, level;
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame;

	if (quic_is_established(sk)) {
		level = QUIC_CRYPTO_APP;
		type = QUIC_FRAME_CONNECTION_CLOSE_APP;
	} else if (quic_is_establishing(sk)) {
		level = QUIC_CRYPTO_INITIAL;
		quic_outq_set_close_errcode(outq, errcode);
	} else {
		return;
	}

	/* send close frame only when it's NOT idle timeout or closed by peer */
	frame = quic_frame_create(sk, type, NULL);
	if (frame) {
		frame->level = level;
		quic_outq_ctrl_tail(sk, frame, false);
	}
}

int quic_outq_transmitted_sack(struct sock *sk, u8 level, s64 largest, s64 smallest,
			       s64 ack_largest, u32 ack_delay)
{
	u32 pathmtu, acked_bytes = 0, transmit_ts = 0, rto, taglen;
	struct quic_path_addr *path = quic_dst(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_inqueue *inq = quic_inq(sk);
	struct quic_cong *cong = quic_cong(sk);
	struct quic_stream_update update;
	struct quic_frame *frame, *tmp;
	struct quic_stream *stream;
	bool raise_timer, complete;
	struct quic_crypto *crypto;
	struct quic_pnmap *pnmap;
	struct list_head *head;
	s64 acked_number = 0;

	pr_debug("[QUIC] %s largest: %llu, smallest: %llu\n", __func__, largest, smallest);
	if (quic_path_pl_confirm(path, largest, smallest)) {
		pathmtu = quic_path_pl_recv(path, &raise_timer, &complete);
		if (pathmtu) {
			taglen = quic_packet_taglen(quic_packet(sk));
			quic_packet_mss_update(sk, pathmtu + taglen);
		}
		if (!complete)
			quic_outq_transmit_probe(sk);
		if (raise_timer) /* reuse probe timer as raise timer */
			quic_timer_reset(sk, QUIC_TIMER_PATH, quic_inq_probe_timeout(inq) * 30);
	}

	head = &outq->transmitted_list;
	list_for_each_entry_safe_reverse(frame, tmp, head, list) {
		if (level != frame->level)
			continue;
		if (frame->number > largest)
			continue;
		if (frame->number < smallest)
			break;
		pnmap = quic_pnmap(sk, level);
		if (frame->number == ack_largest) {
			quic_cong_rtt_update(cong, frame->transmit_ts, ack_delay);
			rto = quic_cong_rto(cong);
			crypto = quic_crypto(sk, level);
			quic_pnmap_set_max_record_ts(pnmap, rto * 2);
			quic_crypto_set_key_update_ts(crypto, rto * 2);
		}
		if (!acked_number) {
			acked_number = frame->number;
			transmit_ts = frame->transmit_ts;
		}

		if (frame->ecn)
			quic_set_sk_ecn(sk, INET_ECN_ECT_0);

		stream = frame->stream;
		if (frame->bytes) {
			if (!stream)
				goto unlink;
			stream->send.frags--;
			if (stream->send.frags || stream->send.state != QUIC_STREAM_SEND_STATE_SENT)
				goto unlink;
			update.id = stream->id;
			update.state = QUIC_STREAM_SEND_STATE_RECVD;
			if (quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update)) {
				stream->send.frags++;
				continue;
			}
			stream->send.state = update.state;
		} else if (frame->type == QUIC_FRAME_RESET_STREAM) {
			update.id = stream->id;
			update.state = QUIC_STREAM_SEND_STATE_RESET_RECVD;
			update.errcode = stream->send.errcode;
			if (quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update))
				continue;
			stream->send.state = update.state;
		} else if (frame->type == QUIC_FRAME_STREAM_DATA_BLOCKED) {
			stream->send.data_blocked = 0;
		} else if (frame->type == QUIC_FRAME_DATA_BLOCKED) {
			outq->data_blocked = 0;
		}
unlink:
		quic_pnmap_set_max_pn_acked(pnmap, frame->number);
		acked_bytes += frame->bytes;

		quic_pnmap_dec_inflight(pnmap, frame->len);
		outq->data_inflight -= frame->bytes;
		outq->inflight -= frame->len;
		list_del(&frame->list);
		quic_frame_free(frame);
	}

	outq->rtx_count = 0;
	if (acked_bytes) {
		quic_cong_cwnd_update_after_sack(cong, acked_number, transmit_ts,
						 acked_bytes, outq->data_inflight);
		quic_outq_set_window(outq, quic_cong_window(cong));
	}
	return acked_bytes;
}

void quic_outq_update_loss_timer(struct sock *sk, u8 level)
{
	struct quic_pnmap *pnmap = quic_pnmap(sk, level);
	u32 timeout, now = jiffies_to_usecs(jiffies);

	timeout = quic_pnmap_loss_ts(pnmap);
	if (timeout)
		goto out;

	if (!quic_pnmap_inflight(pnmap))
		return quic_timer_stop(sk, level);

	timeout = quic_cong_duration(quic_cong(sk));
	timeout *= (1 + quic_outq(sk)->rtx_count);
	timeout += quic_pnmap_last_sent_ts(pnmap);
out:
	if (timeout < now)
		timeout = now + 1;
	quic_timer_reduce(sk, level, timeout - now);
}

/* put the timeout frame back to the corresponding outqueue */
static void quic_outq_retransmit_one(struct sock *sk, struct quic_frame *frame)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *pos, *tmp;
	struct list_head *head;

	head = &outq->control_list;
	if (frame->bytes) {
		head = &outq->stream_list;
		frame->stream->send.frags--;
		frame->stream->send.bytes -= frame->bytes;
		outq->bytes -= frame->bytes;
	}

	list_for_each_entry_safe(pos, tmp, head, list) {
		if (frame->level < pos->level)
			continue;
		if (frame->level > pos->level) {
			head = &pos->list;
			break;
		}
		if (!pos->offset || frame->offset < pos->offset) {
			head = &pos->list;
			break;
		}
	}
	list_add_tail(&frame->list, head);
}

int quic_outq_retransmit_mark(struct sock *sk, u8 level, u8 immediate)
{
	struct quic_pnmap *pnmap = quic_pnmap(sk, level);
	u32 transmit_ts, now, rto, count = 0, bytes = 0;
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_cong *cong = quic_cong(sk);
	struct quic_frame *frame, *tmp;
	struct list_head *head;
	s64 number, last;

	quic_pnmap_set_loss_ts(pnmap, 0);
	last = quic_pnmap_next_number(pnmap) - 1;
	now = jiffies_to_usecs(jiffies);
	head = &outq->transmitted_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (level != frame->level)
			continue;
		transmit_ts = frame->transmit_ts;
		number = frame->number;
		rto = quic_cong_rto(cong);
		if (!immediate && transmit_ts + rto > now && number + 6 > pnmap->max_pn_acked) {
			quic_pnmap_set_loss_ts(pnmap, transmit_ts + rto);
			break;
		}
		quic_pnmap_dec_inflight(pnmap, frame->len);
		outq->data_inflight -= frame->bytes;
		outq->inflight -= frame->len;
		list_del(&frame->list);
		if (quic_frame_is_dgram(frame->type)) { /* no need to retransmit dgram */
			bytes += frame->bytes;
			quic_frame_free(frame);
		} else {
			quic_outq_retransmit_one(sk, frame); /* mark as loss */
			count++;
		}

		if (frame->bytes) {
			quic_cong_cwnd_update_after_timeout(cong, number, transmit_ts, last);
			quic_outq_set_window(outq, quic_cong_window(cong));
		}
	}
	quic_outq_wfree(bytes, sk);
	quic_outq_update_loss_timer(sk, level);
	return count;
}

void quic_outq_retransmit_list(struct sock *sk, struct list_head *head)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *tmp;
	int bytes = 0;

	list_for_each_entry_safe(frame, tmp, head, list) {
		list_del(&frame->list);
		outq->data_inflight -= frame->bytes;
		if (quic_frame_is_dgram(frame->type)) {
			bytes += frame->bytes;
			quic_frame_free(frame);
			continue;
		}
		quic_outq_retransmit_one(sk, frame);
	}
	quic_outq_wfree(bytes, sk);
}

void quic_outq_transmit_one(struct sock *sk, u8 level)
{
	struct quic_outqueue *outq = quic_outq(sk);
	u32 probe_size = QUIC_MIN_UDP_PAYLOAD;
	struct quic_frame *frame;

	quic_packet_set_filter(sk, level, 1);
	if (quic_outq_transmit(sk))
		goto out;

	if (quic_outq_retransmit_mark(sk, level, 0)) {
		quic_packet_set_filter(sk, level, 1);
		if (quic_outq_transmit(sk))
			goto out;
	}

	frame = quic_frame_create(sk, QUIC_FRAME_PING, &probe_size);
	if (frame) {
		frame->level = level;
		quic_outq_ctrl_tail(sk, frame, false);
	}
out:
	outq->rtx_count++;
	quic_outq_update_loss_timer(sk, level);
}

void quic_outq_validate_path(struct sock *sk, struct quic_frame *frame,
			     struct quic_path_addr *path)
{
	u8 local = quic_path_udp_bind(path), path_alt = QUIC_PATH_ALT_DST;
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_inqueue *inq = quic_inq(sk);
	struct quic_frame *pos;
	struct list_head *head;

	if (quic_inq_event_recv(sk, QUIC_EVENT_CONNECTION_MIGRATION, &local))
		return;

	if (local) {
		quic_path_swap_active(path);
		path_alt = QUIC_PATH_ALT_SRC;
	}
	quic_path_addr_free(sk, path, 1);
	quic_set_sk_addr(sk, quic_path_addr(path, 0), local);
	quic_path_set_sent_cnt(path, 0);
	quic_timer_stop(sk, QUIC_TIMER_PATH);
	quic_timer_reset(sk, QUIC_TIMER_PATH, quic_inq_probe_timeout(inq));

	head = &outq->control_list;
	list_for_each_entry(pos, head, list)
		pos->path_alt &= ~path_alt;

	head = &outq->transmitted_list;
	list_for_each_entry(pos, head, list)
		pos->path_alt &= ~path_alt;

	frame->path_alt &= ~path_alt;
	quic_packet_set_ecn_probes(quic_packet(sk), 0);
}

void quic_outq_stream_purge(struct sock *sk, struct quic_stream *stream)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *tmp;
	struct quic_pnmap *pnmap;
	struct list_head *head;
	int bytes = 0;

	head = &outq->transmitted_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (frame->stream != stream)
			continue;
		pnmap = quic_pnmap(sk, frame->level);
		quic_pnmap_dec_inflight(pnmap, frame->len);
		outq->data_inflight -= frame->bytes;
		outq->inflight -= frame->len;
		list_del(&frame->list);
		bytes += frame->bytes;
		quic_frame_free(frame);
	}

	head = &outq->stream_list;
	list_for_each_entry_safe(frame, tmp, head, list) {
		if (frame->stream != stream)
			continue;
		list_del(&frame->list);
		bytes += frame->bytes;
		quic_frame_free(frame);
	}
	quic_outq_wfree(bytes, sk);
}

void quic_outq_list_purge(struct sock *sk, struct list_head *head)
{
	struct quic_frame *frame, *next;
	int bytes = 0;

	list_for_each_entry_safe(frame, next, head, list) {
		list_del(&frame->list);
		bytes += frame->bytes;
		quic_frame_free(frame);
	}
	quic_outq_wfree(bytes, sk);
}

static void quic_outq_encrypted_work(struct work_struct *work)
{
	struct quic_sock *qs = container_of(work, struct quic_sock, outq.work);
	struct sock *sk = &qs->inet.sk;
	struct sk_buff_head *head;
	struct sk_buff *skb;

	lock_sock(sk);
	head = &sk->sk_write_queue;
	if (sock_flag(sk, SOCK_DEAD)) {
		skb_queue_purge(head);
		goto out;
	}

	skb = skb_dequeue(head);
	while (skb) {
		struct quic_crypto_cb *cb = QUIC_CRYPTO_CB(skb);

		quic_packet_config(sk, cb->level, cb->path_alt);
		/* the skb here is ready to send */
		quic_packet_xmit(sk, skb, 1);
		skb = skb_dequeue(head);
	}
	quic_packet_flush(sk);
out:
	release_sock(sk);
	sock_put(sk);
}

void quic_outq_encrypted_tail(struct sock *sk, struct sk_buff *skb)
{
	struct quic_outqueue *outq = quic_outq(sk);

	sock_hold(sk);
	skb_queue_tail(&sk->sk_write_queue, skb);

	if (!schedule_work(&outq->work))
		sock_put(sk);
}

void quic_outq_set_param(struct sock *sk, struct quic_transport_param *p)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_inqueue *inq = quic_inq(sk);
	u32 remote_idle, local_idle;

	outq->max_datagram_frame_size = p->max_datagram_frame_size;
	outq->max_udp_payload_size = p->max_udp_payload_size;
	outq->ack_delay_exponent = p->ack_delay_exponent;
	outq->max_idle_timeout = p->max_idle_timeout;
	outq->max_ack_delay = p->max_ack_delay;
	outq->grease_quic_bit = p->grease_quic_bit;
	outq->disable_1rtt_encryption = p->disable_1rtt_encryption;

	outq->max_bytes = p->max_data;
	sk->sk_sndbuf = 2 * p->max_data;

	remote_idle = outq->max_idle_timeout;
	local_idle = quic_inq_max_idle_timeout(inq);
	if (remote_idle && (!local_idle || remote_idle < local_idle))
		quic_inq_set_max_idle_timeout(inq, remote_idle);

	if (quic_inq_disable_1rtt_encryption(inq) && outq->disable_1rtt_encryption)
		quic_packet_set_taglen(quic_packet(sk), 0);
}

void quic_outq_init(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);

	INIT_LIST_HEAD(&outq->stream_list);
	INIT_LIST_HEAD(&outq->control_list);
	INIT_LIST_HEAD(&outq->datagram_list);
	INIT_LIST_HEAD(&outq->transmitted_list);
	skb_queue_head_init(&sk->sk_write_queue);
	INIT_WORK(&outq->work, quic_outq_encrypted_work);
}

void quic_outq_free(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);

	quic_outq_list_purge(sk, &outq->transmitted_list);
	quic_outq_list_purge(sk, &outq->datagram_list);
	quic_outq_list_purge(sk, &outq->control_list);
	quic_outq_list_purge(sk, &outq->stream_list);
	kfree(outq->close_phrase);
}
