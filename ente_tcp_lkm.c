/*
 * ENTE-TCP: Entropy-Enhanced TCP Congestion Control
 * Linux Kernel Module Implementation
 * 
 * This module implements an entropy-aware TCP congestion control algorithm
 * that distinguishes between random network noise and real congestion.
 * 
 * Core Innovation:
 * - Uses Shannon Entropy to detect if RTT variance is random noise or congestion
 * - High entropy (random RTT patterns) = likely wireless/mobile noise
 * - Low entropy (consistent RTT patterns) = likely real congestion
 * 
 * Algorithm Logic:
 * 1. Track RTT history in a sliding window
 * 2. Calculate Shannon entropy of RTT distribution
 * 3. If entropy is HIGH: be aggressive (noise, not congestion)
 * 4. If entropy is LOW: be conservative (real congestion)
 * 5. Adjust cwnd reduction and growth accordingly
 * 
 * Licensed under GPL v2
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tcp.h>
#include <linux/inet_diag.h>
#include <net/tcp.h>
#include <linux/slab.h>

#define ENTE_TCP_VERSION "1.0"

/* Configuration parameters */
#define ENTROPY_WINDOW_SIZE 16      /* RTT samples for entropy calculation */
#define ENTROPY_CALC_INTERVAL 8     /* Calculate entropy every N packets */
#define HISTOGRAM_BINS 16           /* Number of bins for entropy calculation */

/* Thresholds (scaled by 1000 for integer math) */
#define HIGH_ENTROPY_THRESHOLD 700  /* 0.7 - above this is noise */
#define LOW_ENTROPY_THRESHOLD 400   /* 0.4 - below this is congestion */

/* Aggressiveness factors (scaled by 1000) */
#define NOISE_AGGRESSION 1500       /* 1.5x more aggressive on noise */
#define CONGESTION_CONSERVE 500     /* 0.5x more conservative on congestion */

/* CWND reduction factors */
#define NOISE_REDUCTION_FACTOR 3    /* Reduce to 2/3 on noise */
#define CONGESTION_REDUCTION_FACTOR 2 /* Reduce to 1/2 on congestion */

/* Compact ENTE-TCP private data structure */
struct ente_tcp {
	/* TCP state tracking */
	u32 min_rtt_us;              /* Minimum RTT observed (baseline) */
	u32 prior_cwnd;              /* Previous congestion window */
	u32 ssthresh;                /* Slow start threshold */
	
	/* RTT history for entropy calculation */
	u16 rtt_history[ENTROPY_WINDOW_SIZE]; /* RTT samples in ms */
	u16 history_index;           /* Current position in circular buffer */
	u16 history_count;           /* Number of samples collected */
	
	/* Entropy metrics */
	u16 shannon_entropy;         /* Current entropy (scaled x1000) */
	u16 packets_acked;           /* Counter for periodic entropy calc */
	
	/* RTT variance tracking */
	u32 rtt_variance;            /* RTT variance for quick checks */
	u32 avg_rtt_us;              /* Average RTT */
	
	/* State flags */
	u8 has_entropy_data:1,       /* Have enough samples for entropy */
	   in_slow_start:1,          /* Currently in slow start phase */
	   is_noise:1,               /* High entropy = noise detected */
	   is_congestion:1,          /* Low entropy = congestion detected */
	   loss_event:1,             /* Recent packet loss */
	   reserved:3;               /* Reserved bits */
};

/* Helper: Calculate Shannon entropy from RTT history
 * 
 * Shannon Entropy Formula: H = -Σ(p_i * log2(p_i))
 * where p_i is the probability of value in bin i
 * 
 * High entropy = random/unpredictable (noise)
 * Low entropy = predictable/consistent (congestion)
 */
static u32 calculate_entropy(struct ente_tcp *ca)
{
	u32 histogram[HISTOGRAM_BINS] = {0};
	u32 i, count;
	u16 min_val, max_val, range, bin;
	u64 entropy = 0;
	u32 total;
	
	/* Need minimum samples for reliable entropy */
	if (ca->history_count < 8)
		return 0;
	
	count = min_t(u32, ca->history_count, ENTROPY_WINDOW_SIZE);
	
	/* Find min and max RTT for binning */
	min_val = max_val = ca->rtt_history[0];
	for (i = 0; i < count; i++) {
		if (ca->rtt_history[i] < min_val)
			min_val = ca->rtt_history[i];
		if (ca->rtt_history[i] > max_val)
			max_val = ca->rtt_history[i];
	}
	
	range = max_val - min_val;
	
	/* No variance = no entropy (all values same) */
	if (range == 0)
		return 0;
	
	/* Build histogram: distribute RTT values into bins */
	for (i = 0; i < count; i++) {
		/* Map RTT value to bin number (0 to HISTOGRAM_BINS-1) */
		bin = ((u32)(ca->rtt_history[i] - min_val) * (HISTOGRAM_BINS - 1)) / range;
		bin = min_t(u16, bin, HISTOGRAM_BINS - 1);
		histogram[bin]++;
	}
	
	/* Calculate Shannon entropy: H = -Σ(p * log2(p)) */
	total = count;
	for (i = 0; i < HISTOGRAM_BINS; i++) {
		if (histogram[i] > 0) {
			/* Calculate probability (scaled to avoid float) */
			u64 p = (histogram[i] * 1000000ULL) / total;
			
			/* Approximate log2(p) using bit operations */
			u32 log_p = 0;
			if (p > 0) {
				/* Count leading zeros gives us log2 approximation */
				log_p = 32 - __builtin_clz((u32)(p / 1000));
				log_p *= 1000; /* Scale back */
			}
			
			/* Accumulate: -p * log2(p) */
			entropy += (p * log_p) / 1000000;
		}
	}
	
	/* Normalize entropy to 0-1000 range */
	/* Theoretical max entropy for 16 bins is 4 bits */
	return (u32)min_t(u64, entropy / 4, 1000);
}

/* Helper: Calculate RTT variance for additional confirmation */
static void update_rtt_stats(struct ente_tcp *ca)
{
	u32 i, count, sum = 0;
	u64 variance_sum = 0;
	u32 avg;
	
	if (ca->history_count < 4)
		return;
	
	count = min_t(u32, ca->history_count, ENTROPY_WINDOW_SIZE);
	
	/* Calculate average */
	for (i = 0; i < count; i++) {
		sum += ca->rtt_history[i];
	}
	avg = sum / count;
	ca->avg_rtt_us = avg * 1000; /* Convert ms to us */
	
	/* Calculate variance */
	for (i = 0; i < count; i++) {
		s32 diff = (s32)ca->rtt_history[i] - (s32)avg;
		variance_sum += (u64)(diff * diff);
	}
	ca->rtt_variance = (u32)(variance_sum / count);
}

/* Initialize ENTE-TCP on new connection */
static void ente_tcp_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ente_tcp *ca = inet_csk_ca(sk);
	
	/* Initialize state */
	ca->min_rtt_us = U32_MAX;
	ca->ssthresh = tp->snd_ssthresh;
	ca->prior_cwnd = tp->snd_cwnd;
	ca->history_index = 0;
	ca->history_count = 0;
	ca->shannon_entropy = 0;
	ca->packets_acked = 0;
	ca->rtt_variance = 0;
	ca->avg_rtt_us = 0;
	
	/* Clear flags */
	ca->has_entropy_data = 0;
	ca->in_slow_start = 1;
	ca->is_noise = 0;
	ca->is_congestion = 0;
	ca->loss_event = 0;
	ca->reserved = 0;
	
	/* Clear RTT history */
	memset(ca->rtt_history, 0, sizeof(ca->rtt_history));
	
	/* Start with infinite ssthresh (standard behavior) */
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
}

/* Main congestion control logic - called on each ACK */
static void ente_tcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ente_tcp *ca = inet_csk_ca(sk);
	u32 rtt_us, rtt_ms;
	
	if (!acked)
		return;
	
	/* Update packet counter */
	ca->packets_acked += acked;
	
	/* Get current smoothed RTT */
	rtt_us = tp->srtt_us >> 3; /* srtt_us is scaled by 8 */
	if (rtt_us == 0)
		rtt_us = 1;
	
	/* Track minimum RTT (baseline for comparison) */
	if (rtt_us < ca->min_rtt_us)
		ca->min_rtt_us = rtt_us;
	
	/* Convert to milliseconds for storage (saves memory) */
	rtt_ms = min_t(u32, rtt_us / 1000, 65535);
	if (rtt_ms == 0)
		rtt_ms = 1;
	
	/* Store RTT in circular history buffer */
	ca->rtt_history[ca->history_index] = (u16)rtt_ms;
	ca->history_index = (ca->history_index + 1) % ENTROPY_WINDOW_SIZE;
	if (ca->history_count < ENTROPY_WINDOW_SIZE)
		ca->history_count++;
	
	/* Calculate entropy periodically (not every packet for efficiency) */
	if (ca->packets_acked >= ENTROPY_CALC_INTERVAL) {
		/* Calculate Shannon entropy from RTT distribution */
		ca->shannon_entropy = (u16)calculate_entropy(ca);
		
		/* Update RTT statistics */
		update_rtt_stats(ca);
		
		/* Reset packet counter */
		ca->packets_acked = 0;
		ca->has_entropy_data = 1;
		
		/* Classify network condition based on entropy */
		if (ca->shannon_entropy > HIGH_ENTROPY_THRESHOLD) {
			/* High entropy = random RTT variation = likely noise
			 * Examples: WiFi interference, mobile handoff, wireless jitter
			 */
			ca->is_noise = 1;
			ca->is_congestion = 0;
		} else if (ca->shannon_entropy < LOW_ENTROPY_THRESHOLD) {
			/* Low entropy = consistent RTT increase = likely congestion
			 * Examples: Queue buildup, bandwidth saturation
			 */
			ca->is_noise = 0;
			ca->is_congestion = 1;
		} else {
			/* Medium entropy = unclear, be neutral */
			ca->is_noise = 0;
			ca->is_congestion = 0;
		}
		
		/* Clear loss flag after analysis */
		ca->loss_event = 0;
	}
	
	/* Check if in slow start phase */
	ca->in_slow_start = (tp->snd_cwnd < ca->ssthresh);
	
	/* ===== CONGESTION WINDOW CONTROL LOGIC ===== */
	
	if (ca->in_slow_start) {
		/* SLOW START PHASE: Exponential growth */
		
		if (ca->has_entropy_data && ca->is_congestion) {
			/* Detected real congestion: slow down growth
			 * Grow slower to avoid overshooting
			 */
			tcp_slow_start(tp, acked / 2);
			
		} else if (ca->has_entropy_data && ca->is_noise) {
			/* Detected noise: maintain aggressive growth
			 * This is just random variation, not congestion
			 */
			tcp_slow_start(tp, acked);
			
		} else {
			/* Normal slow start (not enough data yet) */
			tcp_slow_start(tp, acked);
		}
		
	} else {
		/* CONGESTION AVOIDANCE PHASE: Linear growth */
		
		if (ca->has_entropy_data && ca->is_congestion) {
			/* Real congestion detected: be conservative
			 * Grow slowly: cwnd += 0.5 * acked / cwnd
			 */
			u32 delta = max(1U, (acked * CONGESTION_CONSERVE) / 
			                (tp->snd_cwnd * 1000));
			tcp_cong_avoid_ai(tp, tp->snd_cwnd, delta);
			
		} else if (ca->has_entropy_data && ca->is_noise) {
			/* Noise detected: be aggressive
			 * Grow faster: cwnd += 1.5 * acked / cwnd
			 */
			u32 delta = max(1U, (acked * NOISE_AGGRESSION) / 
			                (tp->snd_cwnd * 1000));
			tcp_cong_avoid_ai(tp, tp->snd_cwnd, delta);
			
		} else {
			/* Not enough entropy data: use standard Reno behavior
			 * cwnd += acked / cwnd
			 */
			tcp_reno_cong_avoid(sk, ack, acked);
		}
	}
}

/* Handle packet loss events - set slow start threshold */
static u32 ente_tcp_ssthresh(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ente_tcp *ca = inet_csk_ca(sk);
	u32 reduction_factor;
	
	/* Mark loss event */
	ca->loss_event = 1;
	
	/* Determine how much to reduce cwnd based on entropy */
	if (ca->has_entropy_data) {
		if (ca->is_noise) {
			/* High entropy = likely noise (spurious loss)
			 * Reduce less aggressively: cwnd * 2/3
			 */
			reduction_factor = NOISE_REDUCTION_FACTOR;
			
		} else if (ca->is_congestion) {
			/* Low entropy = real congestion
			 * Standard reduction: cwnd / 2
			 */
			reduction_factor = CONGESTION_REDUCTION_FACTOR;
			
		} else {
			/* Medium entropy: standard reduction */
			reduction_factor = CONGESTION_REDUCTION_FACTOR;
		}
	} else {
		/* Not enough data: use standard reduction */
		reduction_factor = CONGESTION_REDUCTION_FACTOR;
	}
	
	/* Calculate new ssthresh */
	ca->ssthresh = max(tp->snd_cwnd / reduction_factor, 2U);
	ca->prior_cwnd = tp->snd_cwnd;
	
	return ca->ssthresh;
}

/* Undo cwnd reduction if loss was spurious (false alarm) */
static u32 ente_tcp_undo_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ente_tcp *ca = inet_csk_ca(sk);
	
	/* Restore previous cwnd (loss was spurious) */
	tp->snd_cwnd = max(tp->snd_cwnd, ca->prior_cwnd);
	ca->in_slow_start = (tp->snd_cwnd < ca->ssthresh);
	
	return max(tp->snd_cwnd, ca->prior_cwnd);
}

/* Handle congestion window events */
static void ente_tcp_cwnd_event(struct sock *sk, enum tcp_ca_event ev)
{
	struct ente_tcp *ca = inet_csk_ca(sk);
	
	switch (ev) {
	case CA_EVENT_LOSS:
		/* Packet loss detected */
		ca->loss_event = 1;
		break;
		
	case CA_EVENT_CWND_RESTART:
		/* Connection restart after idle - reset state */
		ca->history_count = 0;
		ca->has_entropy_data = 0;
		break;
		
	default:
		break;
	}
}

/* Provide diagnostic information */
static size_t ente_tcp_get_info(struct sock *sk, u32 ext, int *attr,
			         union tcp_cc_info *info)
{
	const struct ente_tcp *ca = inet_csk_ca(sk);
	
	if (ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		/* Reuse Vegas info structure for our metrics */
		info->vegas.tcpv_enabled = 1;
		info->vegas.tcpv_rttcnt = ca->history_count;
		info->vegas.tcpv_rtt = ca->avg_rtt_us / 1000;
		info->vegas.tcpv_minrtt = ca->shannon_entropy;
		*attr = INET_DIAG_VEGASINFO;
		return sizeof(struct tcpvegas_info);
	}
	return 0;
}

/* Set TCP state for congestion control */
static void ente_tcp_set_state(struct sock *sk, u8 new_state)
{
	struct ente_tcp *ca = inet_csk_ca(sk);
	
	if (new_state == TCP_CA_Loss) {
		/* Entering loss state */
		ca->loss_event = 1;
	}
}

/* TCP congestion control operations structure */
static struct tcp_congestion_ops ente_tcp_ops __read_mostly = {
	.init		= ente_tcp_init,
	.ssthresh	= ente_tcp_ssthresh,
	.cong_avoid	= ente_tcp_cong_avoid,
	.undo_cwnd	= ente_tcp_undo_cwnd,
	.cwnd_event	= ente_tcp_cwnd_event,
	.get_info	= ente_tcp_get_info,
	.set_state	= ente_tcp_set_state,
	.owner		= THIS_MODULE,
	.name		= "ente_tcp",
};

/* Module initialization */
static int __init ente_tcp_register(void)
{
	int ret;
	
	/* Verify structure fits in kernel's allocated space */
	BUILD_BUG_ON(sizeof(struct ente_tcp) > ICSK_CA_PRIV_SIZE);
	
	ret = tcp_register_congestion_control(&ente_tcp_ops);
	if (ret)
		return ret;
	
	pr_info("ENTE-TCP v%s: Entropy-Enhanced TCP Congestion Control registered\n",
		ENTE_TCP_VERSION);
	pr_info("ENTE-TCP: Distinguishes network noise from real congestion using entropy\n");
	pr_info("ENTE-TCP: Structure size = %zu bytes (limit = %d bytes)\n",
		sizeof(struct ente_tcp), ICSK_CA_PRIV_SIZE);
	
	return 0;
}

/* Module cleanup */
static void __exit ente_tcp_unregister(void)
{
	tcp_unregister_congestion_control(&ente_tcp_ops);
	pr_info("ENTE-TCP: Unregistered from kernel\n");
}

module_init(ente_tcp_register);
module_exit(ente_tcp_unregister);

MODULE_AUTHOR("ENTE-TCP Development Team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Entropy-Enhanced TCP Congestion Control");
MODULE_VERSION(ENTE_TCP_VERSION);