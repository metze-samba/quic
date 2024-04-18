/* SPDX-License-Identifier: GPL-2.0-or-later */
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

enum {
	QUIC_TIMER_AP_LOSS = QUIC_CRYPTO_APP,
	QUIC_TIMER_IN_LOSS = QUIC_CRYPTO_INITIAL,
	QUIC_TIMER_HS_LOSS = QUIC_CRYPTO_HANDSHAKE,
	QUIC_TIMER_SACK,
	QUIC_TIMER_PATH,
	QUIC_TIMER_MAX,
};

void quic_timer_reduce(struct sock *sk, u8 type, u32 timeout);
void quic_timer_reset(struct sock *sk, u8 type, u32 timeout);
void quic_timer_start(struct sock *sk, u8 type, u32 timeout);
void quic_timer_stop(struct sock *sk, u8 type);
void quic_timer_init(struct sock *sk);
void quic_timer_free(struct sock *sk);
