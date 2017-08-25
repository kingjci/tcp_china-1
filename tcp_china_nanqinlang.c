/* -*- indent-tabs-mode: t; tab-width: 8; c-basic-offset: 8; -*- */
/*
 * TCP China congestion control
 * 
 * Modified by @nanqinlang
 *
 *
 * This is based on the congestion detection/avoidance scheme described in
 *   King R., Baraniuk, R., and Riedi, R.
 *   "TCP-Africa: An Adaptive and Fair Rapid Increase Rule for Scalable TCP"
 *   Presented at INFOCOM 2005, and published in:
 *   INFOCOM 2005. 24th Annual Joint Conference of the IEEE Computer and
 *   Communications Societies. Proceedings IEEE
 *   Volume 3, 13-17 March 2005. pp 1838-1848.
 *
 *   The paper is available from:
 *   http://www.spin.rice.edu/PDF/Infocom05web.pdf
 *
 * TCP-Africa is a delay-sensitive congestion avoidance algorithm for TCP that
 * allows for scalable, aggressive behavior in large underutilized links, yet
 * falls back to the more conservative TCP-Reno algorithm once links become
 * well utilized and congestion is imminent.
 *
 * TCP-China is a modification of TCP-Africa that keeps using fast mode all the
 * time, regardless of fairness.
 *
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/version.h>

#include <net/tcp.h>

/* From AIMD tables from RFC 3649 appendix B,
 * with fixed-point MD scaled <<8.
 */
static const struct hstcp_aimd_val {
        unsigned int cwnd;
        unsigned int md;
} hstcp_aimd_vals[] = {
 {     36,  128, /*  0.50 */ },
 {    119,  115, /*  0.45 */ },
 {    218,  107, /*  0.40 */ },
 {    347,  101, /*  0.39 */ },
 {    494,   99, /*  0.38 */ },
 {    495,   97, /*  0.37 */ },
 {    496,   96, /*  0.36 */ },
 {    663,   89, /*  0.35 */ },
 {    851,   87, /*  0.34 */ },
 {   1058,   84, /*  0.33 */ },
 {   1284,   81, /*  0.32 */ },
 {   1529,   78, /*  0.31 */ },
 {   1793,   76, /*  0.30 */ },
 {   2076,   74, /*  0.29 */ },
 {   2239,   72, /*  0.29 */ },
 {   2378,   69, /*  0.28 */ },
 {   2699,   68, /*  0.28 */ },
 {   3039,   67, /*  0.27 */ },
 {   3399,   66, /*  0.27 */ },
 {   3778,   65, /*  0.26 */ },
 {   4177,   64, /*  0.26 */ },
 {   4596,   63, /*  0.25 */ },
 {   5036,   62, /*  0.25 */ },
 {   5497,   61, /*  0.24 */ },
 {   5979,   60, /*  0.24 */ },
 {   6483,   59, /*  0.23 */ },
 {   7009,   58, /*  0.23 */ },
 {   7558,   57, /*  0.22 */ },
 {   8130,   56, /*  0.22 */ },
 {   8726,   56, /*  0.22 */ },
 {   9346,   55, /*  0.21 */ },
 {   9991,   54, /*  0.21 */ },
 {  10661,   54, /*  0.21 */ },
 {  11358,   53, /*  0.20 */ },
 {  12082,   52, /*  0.20 */ },
 {  12834,   52, /*  0.20 */ },
 {  13614,   51, /*  0.19 */ },
 {  14424,   50, /*  0.19 */ },
 {  15265,   50, /*  0.19 */ },
 {  17042,   49, /*  0.18 */ },
 {  17981,   48, /*  0.18 */ },
 {  18955,   48, /*  0.18 */ },
 {  19965,   47, /*  0.17 */ },
 {  21013,   47, /*  0.17 */ },
 {  22101,   46, /*  0.17 */ },
 {  23230,   45, /*  0.17 */ },
 {  24402,   44, /*  0.16 */ },
 {  25618,   44, /*  0.16 */ },
 {  26881,   43, /*  0.16 */ },
 {  28193,   42, /*  0.16 */ },
 {  29557,   41, /*  0.15 */ },
 {  30975,   41, /*  0.15 */ },
 {  32450,   40, /*  0.15 */ },
 {  33986,   39, /*  0.15 */ },
 {  35586,   38, /*  0.14 */ },
 {  37253,   38, /*  0.14 */ },
 {  38992,   37, /*  0.14 */ },
 {  40808,   36, /*  0.14 */ },
 {  42707,   35, /*  0.13 */ },
 {  44694,   35, /*  0.13 */ },
 {  46776,   34, /*  0.13 */ },
 {  48961,   33, /*  0.13 */ },
 {  53677,   32, /*  0.12 */ },
 {  56230,   32, /*  0.12 */ },
 {  58932,   31, /*  0.12 */ },
 {  61799,   30, /*  0.12 */ },
 {  64851,   29, /*  0.11 */ },
 {  68113,   29, /*  0.11 */ },
 {  71617,   28, /*  0.11 */ },
 {  73739,   27, /*  0.11 */ },
 {  75401,   26, /*  0.10 */ },
 {  79517,   26, /*  0.10 */ },
 {  84035,   25, /*  0.10 */ },
 {  89053,   24, /*  0.10 */ },
};

#define HSTCP_AIMD_MAX	ARRAY_SIZE(hstcp_aimd_vals)

struct china {
	u32	minrtt;		/* the minimum of all RTT measurements seen (in usec) */
	u32	artt;		/* smoothed average of all RTT measurements seen (in usec) */
	u32	ai;		/* used in the HSTCP fast mode */
};

static void tcp_china_reset(struct china *ca)
{
	ca->minrtt = 0;
	ca->artt = 0;
}

static void tcp_china_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct china *ca = inet_csk_ca(sk);

	ca->ai = 0;

	/* Ensure the MD arithmetic works.  This is somewhat pedantic,
	 * since I don't think we will see a cwnd this large. :) */
	tp->snd_cwnd_clamp = min_t(u32, tp->snd_cwnd_clamp, 0xffffffff/128);

	tcp_china_reset(ca);
}

static void tcp_china_rtt_calc(struct sock *sk, u32 num_acked, s32 rtt_us)
{
	struct china *ca = inet_csk_ca(sk);
	u32 rtt, artt, minrtt;

	rtt = rtt_us + 1; /* Never allow zero rtt */

	minrtt = ca->minrtt;
	artt = ca->artt;

	/* Get the smallest RTT seen. This is the assumed propogation delay. */
	if (rtt < minrtt || minrtt == 0)
		minrtt = rtt;

	/* Get the average RTT so far. This is tha assumed propogation delay
	 * + queueing delay.
	 */
	if (artt > 0)
		/* Find the exponentially smoothed average RTT.
		 * Using avg RTT = 7/8 avg RTT + 1/8 new RTT
		 * for speed, like in tcp_rtt_estimator.
		 */
		artt += (rtt >> 3) - (artt >> 3);
	else
		/* We don't have an average yet. */
		artt = rtt;

	ca->artt = artt;
	ca->minrtt = minrtt;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
static void tcp_china_cong_avoid(struct sock *sk, u32 ack, u32 acked,
                                 u32 in_flight)
#else
static void tcp_china_cong_avoid(struct sock *sk, u32 ack, u32 acked)
#endif
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct china *ca = inet_csk_ca(sk);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
	if (!tcp_is_cwnd_limited(sk, in_flight))
#else
	if (!tcp_is_cwnd_limited(sk))
#endif
		return;

	/* Adjust the cwnd */
	if (tp->snd_cwnd <= tp->snd_ssthresh) {
		/* Standard TCP slow start. */
		tcp_slow_start(tp, acked);
	}
	else {
		/* Fast increase (W = W + fast_increase(W)/W)
		 *
		 * Fast increase is done with the HSTCP algorithm.
		 */

		/* Update AIMD parameters.
		 *
		 * We want to guarantee that:
		 *     hstcp_aimd_vals[ca->ai-1].cwnd <
		 *     snd_cwnd <=
		 *     hstcp_aimd_vals[ca->ai].cwnd
		 */
		if (tp->snd_cwnd > hstcp_aimd_vals[ca->ai].cwnd) {
			while (tp->snd_cwnd >
			       hstcp_aimd_vals[ca->ai].cwnd &&
			       ca->ai < HSTCP_AIMD_MAX - 1)
				ca->ai++;
		} else if (ca->ai && tp->snd_cwnd <=
			   hstcp_aimd_vals[ca->ai-1].cwnd) {
			while (ca->ai && tp->snd_cwnd <=
			       hstcp_aimd_vals[ca->ai-1].cwnd)
				ca->ai--;
		}

		/* Do additive increase */
		if (tp->snd_cwnd < tp->snd_cwnd_clamp) {
			/* cwnd = cwnd + a(w) / cwnd */
			tp->snd_cwnd_cnt += ca->ai + 1;
			if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
				tp->snd_cwnd_cnt -= tp->snd_cwnd;
				tp->snd_cwnd++;
			}
		}
	}
}

static u32 tcp_china_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	const struct china *ca = inet_csk_ca(sk);

    /* HSTCP decrease. */
    return max(tp->snd_cwnd -
           ((tp->snd_cwnd * hstcp_aimd_vals[ca->ai].md) >> 8),
           2U);
}

static struct tcp_congestion_ops tcp_china = {
	.init		= tcp_china_init,
	.ssthresh	= tcp_china_ssthresh,
	.cong_avoid	= tcp_china_cong_avoid,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
	.min_cwnd	= tcp_reno_min_cwnd,
#endif
	.pkts_acked	= tcp_china_rtt_calc,

	.owner		= THIS_MODULE,
	.name		= "china",
};

static int __init tcp_china_register(void)
{
	BUILD_BUG_ON(sizeof(struct china) > ICSK_CA_PRIV_SIZE);
	tcp_register_congestion_control(&tcp_china);
	return 0;
}

static void __exit tcp_china_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_china);
}

module_init(tcp_china_register);
module_exit(tcp_china_unregister);

MODULE_AUTHOR("Dana Jansens, madeye, nanqinlang");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tcpchina");
