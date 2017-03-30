/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2016-2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/queue.h>

#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>

#include <rte_eventdev.h>
#include "test.h"

#define MAX_PORTS 16
#define MAX_QIDS 16
#define NUM_PACKETS (1<<18)

static int evdev;

struct test {
	struct rte_mempool *mbuf_pool;
	uint8_t port[MAX_PORTS];
	uint8_t qid[MAX_QIDS];
	int nb_qids;
};

static struct rte_event release_ev;

static inline struct rte_mbuf *
rte_gen_arp(int portid, struct rte_mempool *mp)
{
	/*
	 * len = 14 + 46
	 * ARP, Request who-has 10.0.0.1 tell 10.0.0.2, length 46
	 */
	static const uint8_t arp_request[] = {
		/*0x0000:*/ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xec, 0xa8,
		0x6b, 0xfd, 0x02, 0x29, 0x08, 0x06, 0x00, 0x01,
		/*0x0010:*/ 0x08, 0x00, 0x06, 0x04, 0x00, 0x01, 0xec, 0xa8,
		0x6b, 0xfd, 0x02, 0x29, 0x0a, 0x00, 0x00, 0x01,
		/*0x0020:*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/*0x0030:*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	struct rte_mbuf *m;
	int pkt_len = sizeof(arp_request) - 1;

	m = rte_pktmbuf_alloc(mp);
	if (!m)
		return 0;

	memcpy((void *)((uintptr_t)m->buf_addr + m->data_off),
		arp_request, pkt_len);
	rte_pktmbuf_pkt_len(m) = pkt_len;
	rte_pktmbuf_data_len(m) = pkt_len;

	RTE_SET_USED(portid);

	return m;
}

/* initialization and config */
static inline int
init(struct test *t, int nb_queues, int nb_ports)
{
	struct rte_event_dev_config config = {
			.nb_event_queues = nb_queues,
			.nb_event_ports = nb_ports,
			.nb_event_queue_flows = 1024,
			.nb_events_limit = 4096,
			.nb_event_port_dequeue_depth = 128,
			.nb_event_port_enqueue_depth = 128,
	};
	int ret;

	void *temp = t->mbuf_pool; /* save and restore mbuf pool */

	memset(t, 0, sizeof(*t));
	t->mbuf_pool = temp;

	ret = rte_event_dev_configure(evdev, &config);
	if (ret < 0)
		printf("%d: Error configuring device\n", __LINE__);
	return ret;
};

static inline int
create_ports(struct test *t, int num_ports)
{
	int i;
	static const struct rte_event_port_conf conf = {
			.new_event_threshold = 1024,
			.dequeue_depth = 32,
			.enqueue_depth = 64,
	};
	if (num_ports > MAX_PORTS)
		return -1;

	for (i = 0; i < num_ports; i++) {
		if (rte_event_port_setup(evdev, i, &conf) < 0) {
			printf("Error setting up port %d\n", i);
			return -1;
		}
		t->port[i] = i;
	}

	return 0;
}

static inline int
create_lb_qids(struct test *t, int num_qids, uint32_t flags)
{
	int i;

	/* Q creation */
	const struct rte_event_queue_conf conf = {
			.event_queue_cfg = flags,
			.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
			.nb_atomic_flows = 1024,
			.nb_atomic_order_sequences = 1024,
	};

	for (i = t->nb_qids; i < t->nb_qids + num_qids; i++) {
		if (rte_event_queue_setup(evdev, i, &conf) < 0) {
			printf("%d: error creating qid %d\n", __LINE__, i);
			return -1;
		}
		t->qid[i] = i;
	}
	t->nb_qids += num_qids;
	if (t->nb_qids > MAX_QIDS)
		return -1;

	return 0;
}

static inline int
create_atomic_qids(struct test *t, int num_qids)
{
	return create_lb_qids(t, num_qids, RTE_EVENT_QUEUE_CFG_ATOMIC_ONLY);
}

static inline int
create_ordered_qids(struct test *t, int num_qids)
{
	return create_lb_qids(t, num_qids, RTE_EVENT_QUEUE_CFG_ORDERED_ONLY);
}


static inline int
create_unordered_qids(struct test *t, int num_qids)
{
	return create_lb_qids(t, num_qids, RTE_EVENT_QUEUE_CFG_PARALLEL_ONLY);
}

static inline int
create_directed_qids(struct test *t, int num_qids, const uint8_t ports[])
{
	int i;

	/* Q creation */
	static const struct rte_event_queue_conf conf = {
			.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
			.event_queue_cfg = RTE_EVENT_QUEUE_CFG_SINGLE_LINK,
			.nb_atomic_flows = 1024,
			.nb_atomic_order_sequences = 1024,
	};

	for (i = t->nb_qids; i < t->nb_qids + num_qids; i++) {
		if (rte_event_queue_setup(evdev, i, &conf) < 0) {
			printf("%d: error creating qid %d\n", __LINE__, i);
			return -1;
		}
		t->qid[i] = i;

		if (rte_event_port_link(evdev, ports[i - t->nb_qids],
				&t->qid[i], NULL, 1) != 1) {
			printf("%d: error creating link for qid %d\n",
					__LINE__, i);
			return -1;
		}
	}
	t->nb_qids += num_qids;
	if (t->nb_qids > MAX_QIDS)
		return -1;

	return 0;
}

/* destruction */
static inline int
cleanup(struct test *t __rte_unused)
{
	rte_event_dev_stop(evdev);
	rte_event_dev_close(evdev);
	return 0;
};

struct test_event_dev_stats {
	uint64_t rx_pkts;       /**< Total packets received */
	uint64_t rx_dropped;    /**< Total packets dropped (Eg Invalid QID) */
	uint64_t tx_pkts;       /**< Total packets transmitted */

	/** Packets received on this port */
	uint64_t port_rx_pkts[MAX_PORTS];
	/** Packets dropped on this port */
	uint64_t port_rx_dropped[MAX_PORTS];
	/** Packets inflight on this port */
	uint64_t port_inflight[MAX_PORTS];
	/** Packets transmitted on this port */
	uint64_t port_tx_pkts[MAX_PORTS];
	/** Packets received on this qid */
	uint64_t qid_rx_pkts[MAX_QIDS];
	/** Packets dropped on this qid */
	uint64_t qid_rx_dropped[MAX_QIDS];
	/** Packets transmitted on this qid */
	uint64_t qid_tx_pkts[MAX_QIDS];
};

static inline int
test_event_dev_stats_get(int dev_id, struct test_event_dev_stats *stats)
{
	static uint32_t i;
	static uint32_t total_ids[3]; /* rx, tx and drop */
	static uint32_t port_rx_pkts_ids[MAX_PORTS];
	static uint32_t port_rx_dropped_ids[MAX_PORTS];
	static uint32_t port_inflight_ids[MAX_PORTS];
	static uint32_t port_tx_pkts_ids[MAX_PORTS];
	static uint32_t qid_rx_pkts_ids[MAX_QIDS];
	static uint32_t qid_rx_dropped_ids[MAX_QIDS];
	static uint32_t qid_tx_pkts_ids[MAX_QIDS];


	stats->rx_pkts = rte_event_dev_xstats_by_name_get(dev_id,
			"dev_rx", &total_ids[0]);
	stats->rx_dropped = rte_event_dev_xstats_by_name_get(dev_id,
			"dev_drop", &total_ids[1]);
	stats->tx_pkts = rte_event_dev_xstats_by_name_get(dev_id,
			"dev_tx", &total_ids[2]);
	for (i = 0; i < MAX_PORTS; i++) {
		char name[32];
		snprintf(name, sizeof(name), "port_%u_rx", i);
		stats->port_rx_pkts[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &port_rx_pkts_ids[i]);
		snprintf(name, sizeof(name), "port_%u_drop", i);
		stats->port_rx_dropped[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &port_rx_dropped_ids[i]);
		snprintf(name, sizeof(name), "port_%u_inflight", i);
		stats->port_inflight[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &port_inflight_ids[i]);
		snprintf(name, sizeof(name), "port_%u_tx", i);
		stats->port_tx_pkts[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &port_tx_pkts_ids[i]);
	}
	for (i = 0; i < MAX_QIDS; i++) {
		char name[32];
		snprintf(name, sizeof(name), "qid_%u_rx", i);
		stats->qid_rx_pkts[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &qid_rx_pkts_ids[i]);
		snprintf(name, sizeof(name), "qid_%u_drop", i);
		stats->qid_rx_dropped[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &qid_rx_dropped_ids[i]);
		snprintf(name, sizeof(name), "qid_%u_tx", i);
		stats->qid_tx_pkts[i] = rte_event_dev_xstats_by_name_get(
				dev_id, name, &qid_tx_pkts_ids[i]);
	}

	return 0;
}

static int
test_single_directed_packet(struct test *t)
{
	const int rx_enq = 0;
	const int wrk_enq = 2;
	int err;

	/* Create instance with 3 directed QIDs going to 3 ports */
	if (init(t, 3, 3) < 0 ||
			create_ports(t, 3) < 0 ||
			create_directed_qids(t, 3, t->port) < 0)
		return -1;

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/************** FORWARD ****************/
	struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);
	struct rte_event ev = {
			.op = RTE_EVENT_OP_NEW,
			.queue_id = wrk_enq,
			.mbuf = arp,
	};

	if (!arp) {
		printf("%d: gen of pkt failed\n", __LINE__);
		return -1;
	}

	const uint32_t MAGIC_SEQN = 4711;
	arp->seqn = MAGIC_SEQN;

	/* generate pkt and enqueue */
	err = rte_event_enqueue_burst(evdev, rx_enq, &ev, 1);
	if (err < 0) {
		printf("%d: error failed to enqueue\n", __LINE__);
		return -1;
	}

	/* Run schedule() as dir packets may need to be re-ordered */
	rte_event_schedule(evdev);

	struct test_event_dev_stats stats;
	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: error failed to get stats\n", __LINE__);
		return -1;
	}

	if (stats.port_rx_pkts[rx_enq] != 1) {
		printf("%d: error stats incorrect for directed port\n",
				__LINE__);
		return -1;
	}

	uint32_t deq_pkts;
	deq_pkts = rte_event_dequeue_burst(evdev, wrk_enq, &ev, 1, 0);
	if (deq_pkts != 1) {
		printf("%d: error failed to deq\n", __LINE__);
		return -1;
	}

	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_rx_pkts[wrk_enq] != 0 &&
			stats.port_rx_pkts[wrk_enq] != 1) {
		printf("%d: error directed stats post-dequeue\n", __LINE__);
		return -1;
	}

	if (ev.mbuf->seqn != MAGIC_SEQN) {
		printf("%d: error magic sequence number not dequeued\n",
				__LINE__);
		return -1;
	}

	rte_pktmbuf_free(ev.mbuf);
	cleanup(t);
	return 0;
}

static int
burst_packets(struct test *t)
{
	/************** CONFIG ****************/
	uint32_t i;
	int err;
	int ret;

	/* Create instance with 2 ports and 2 queues */
	if (init(t, 2, 2) < 0 ||
			create_ports(t, 2) < 0 ||
			create_atomic_qids(t, 2) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/* CQ mapping to QID */
	ret = rte_event_port_link(evdev, t->port[0], &t->qid[0], NULL, 1);
	if (ret != 1) {
		printf("%d: error mapping lb qid0\n", __LINE__);
		return -1;
	}
	ret = rte_event_port_link(evdev, t->port[1], &t->qid[1], NULL, 1);
	if (ret != 1) {
		printf("%d: error mapping lb qid1\n", __LINE__);
		return -1;
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/************** FORWARD ****************/
	const uint32_t rx_port = 0;
	const uint32_t NUM_PKTS = 2;

	for (i = 0; i < NUM_PKTS; i++) {
		struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);
		if (!arp) {
			printf("%d: error generating pkt\n", __LINE__);
			return -1;
		}

		struct rte_event ev = {
				.op = RTE_EVENT_OP_NEW,
				.queue_id = i % 2,
				.flow_id = i % 3,
				.mbuf = arp,
		};
		/* generate pkt and enqueue */
		err = rte_event_enqueue_burst(evdev, t->port[rx_port], &ev, 1);
		if (err < 0) {
			printf("%d: Failed to enqueue\n", __LINE__);
			return -1;
		}
	}
	rte_event_schedule(evdev);

	/* Check stats for all NUM_PKTS arrived to sched core */
	struct test_event_dev_stats stats;

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		return -1;
	}
	if (stats.rx_pkts != NUM_PKTS || stats.tx_pkts != NUM_PKTS) {
		printf("%d: Sched core didn't receive all %d pkts\n",
				__LINE__, NUM_PKTS);
		rte_event_dev_dump(evdev, stdout);
		return -1;
	}

	uint32_t deq_pkts;
	int p;

	deq_pkts = 0;
	/******** DEQ QID 1 *******/
	do {
		struct rte_event ev;
		p = rte_event_dequeue_burst(evdev, t->port[0], &ev, 1, 0);
		deq_pkts += p;
		rte_pktmbuf_free(ev.mbuf);
	} while (p);

	if (deq_pkts != NUM_PKTS/2) {
		printf("%d: Half of NUM_PKTS didn't arrive at port 1\n",
				__LINE__);
		return -1;
	}

	/******** DEQ QID 2 *******/
	deq_pkts = 0;
	do {
		struct rte_event ev;
		p = rte_event_dequeue_burst(evdev, t->port[1], &ev, 1, 0);
		deq_pkts += p;
		rte_pktmbuf_free(ev.mbuf);
	} while (p);
	if (deq_pkts != NUM_PKTS/2) {
		printf("%d: Half of NUM_PKTS didn't arrive at port 2\n",
				__LINE__);
		return -1;
	}

	cleanup(t);
	return 0;
}

static int
abuse_inflights(struct test *t)
{
	const int rx_enq = 0;
	const int wrk_enq = 2;
	int err;

	/* Create instance with 4 ports */
	if (init(t, 1, 4) < 0 ||
			create_ports(t, 4) < 0 ||
			create_atomic_qids(t, 1) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/* CQ mapping to QID */
	err = rte_event_port_link(evdev, t->port[wrk_enq], NULL, NULL, 0);
	if (err != 1) {
		printf("%d: error mapping lb qid\n", __LINE__);
		cleanup(t);
		return -1;
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/* Enqueue op only */
	err = rte_event_enqueue_burst(evdev, t->port[rx_enq], &release_ev, 1);
	if (err < 0) {
		printf("%d: Failed to enqueue\n", __LINE__);
		return -1;
	}

	/* schedule */
	rte_event_schedule(evdev);

	struct test_event_dev_stats stats;

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		return -1;
	}

	if (stats.rx_pkts != 0 ||
			stats.tx_pkts != 0 ||
			stats.port_inflight[wrk_enq] != 0) {
		printf("%d: Sched core didn't handle pkt as expected\n",
				__LINE__);
		return -1;
	}

	cleanup(t);
	return 0;
}

static int
port_reconfig_credits(struct test *t)
{
	if (init(t, 1, 1) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	uint32_t i;
	const uint32_t NUM_ITERS = 32;
	for (i = 0; i < NUM_ITERS; i++) {
		const struct rte_event_queue_conf conf = {
			.event_queue_cfg = RTE_EVENT_QUEUE_CFG_ATOMIC_ONLY,
			.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
			.nb_atomic_flows = 1024,
			.nb_atomic_order_sequences = 1024,
		};
		if (rte_event_queue_setup(evdev, 0, &conf) < 0) {
			printf("%d: error creating qid\n", __LINE__);
			return -1;
		}
		t->qid[0] = 0;

		static const struct rte_event_port_conf port_conf = {
				.new_event_threshold = 128,
				.dequeue_depth = 32,
				.enqueue_depth = 64,
		};
		if (rte_event_port_setup(evdev, 0, &port_conf) < 0) {
			printf("%d Error setting up port\n", __LINE__);
			return -1;
		}

		int links = rte_event_port_link(evdev, 0, NULL, NULL, 0);
		if (links != 1) {
			printf("%d: error mapping lb qid\n", __LINE__);
			goto fail;
		}

		if (rte_event_dev_start(evdev) < 0) {
			printf("%d: Error with start call\n", __LINE__);
			goto fail;
		}

		const uint32_t NPKTS = 1;
		uint32_t j;
		for (j = 0; j < NPKTS; j++) {
			struct rte_event ev;
			struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);
			if (!arp) {
				printf("%d: gen of pkt failed\n", __LINE__);
				goto fail;
			}
			ev.queue_id = t->qid[0];
			ev.op = RTE_EVENT_OP_NEW;
			ev.mbuf = arp;
			int err = rte_event_enqueue_burst(evdev, 0, &ev, 1);
			if (err != 1) {
				printf("%d: Failed to enqueue\n", __LINE__);
				rte_event_dev_dump(0, stdout);
				goto fail;
			}
		}

		rte_event_schedule(evdev);

		struct rte_event ev[NPKTS];
		int deq = rte_event_dequeue_burst(evdev, t->port[0], ev,
							NPKTS, 0);
		if (deq != 1)
			printf("%d error; no packet dequeued\n", __LINE__);

		/* let cleanup below stop the device on last iter */
		if (i != NUM_ITERS-1)
			rte_event_dev_stop(evdev);
	}

	cleanup(t);
	return 0;
fail:
	cleanup(t);
	return -1;
}

static int
port_single_lb_reconfig(struct test *t)
{
	if (init(t, 2, 2) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		goto fail;
	}

	static const struct rte_event_queue_conf conf_lb_atomic = {
		.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
		.event_queue_cfg = RTE_EVENT_QUEUE_CFG_ATOMIC_ONLY,
		.nb_atomic_flows = 1024,
		.nb_atomic_order_sequences = 1024,
	};
	if (rte_event_queue_setup(evdev, 0, &conf_lb_atomic) < 0) {
		printf("%d: error creating qid\n", __LINE__);
		goto fail;
	}

	static const struct rte_event_queue_conf conf_single_link = {
		.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
		.event_queue_cfg = RTE_EVENT_QUEUE_CFG_SINGLE_LINK,
		.nb_atomic_flows = 1024,
		.nb_atomic_order_sequences = 1024,
	};
	if (rte_event_queue_setup(evdev, 1, &conf_single_link) < 0) {
		printf("%d: error creating qid\n", __LINE__);
		goto fail;
	}

	struct rte_event_port_conf port_conf = {
		.new_event_threshold = 128,
		.dequeue_depth = 32,
		.enqueue_depth = 64,
	};
	if (rte_event_port_setup(evdev, 0, &port_conf) < 0) {
		printf("%d Error setting up port\n", __LINE__);
		goto fail;
	}
	if (rte_event_port_setup(evdev, 1, &port_conf) < 0) {
		printf("%d Error setting up port\n", __LINE__);
		goto fail;
	}

	/* link port to lb queue */
	uint8_t queue_id = 0;
	if (rte_event_port_link(evdev, 0, &queue_id, NULL, 1) != 1) {
		printf("%d: error creating link for qid\n", __LINE__);
		goto fail;
	}

	int ret = rte_event_port_unlink(evdev, 0, &queue_id, 1);
	if (ret != 1) {
		printf("%d: Error unlinking lb port\n", __LINE__);
		goto fail;
	}

	queue_id = 1;
	if (rte_event_port_link(evdev, 0, &queue_id, NULL, 1) != 1) {
		printf("%d: error creating link for qid\n", __LINE__);
		goto fail;
	}

	queue_id = 0;
	int err = rte_event_port_link(evdev, 1, &queue_id, NULL, 1);
	if (err != 1) {
		printf("%d: error mapping lb qid\n", __LINE__);
		goto fail;
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		goto fail;
	}

	cleanup(t);
	return 0;
fail:
	cleanup(t);
	return -1;
}

static int
ordered_reconfigure(struct test *t)
{
	if (init(t, 1, 1) < 0 ||
			create_ports(t, 1) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	const struct rte_event_queue_conf conf = {
			.event_queue_cfg = RTE_EVENT_QUEUE_CFG_ORDERED_ONLY,
			.priority = RTE_EVENT_DEV_PRIORITY_NORMAL,
			.nb_atomic_flows = 1024,
			.nb_atomic_order_sequences = 1024,
	};

	if (rte_event_queue_setup(evdev, 0, &conf) < 0) {
		printf("%d: error creating qid\n", __LINE__);
		goto failed;
	}

	if (rte_event_queue_setup(evdev, 0, &conf) < 0) {
		printf("%d: error creating qid, for 2nd time\n", __LINE__);
		goto failed;
	}

	rte_event_port_link(evdev, t->port[0], NULL, NULL, 0);
	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	cleanup(t);
	return 0;
failed:
	cleanup(t);
	return -1;
}

static int
invalid_qid(struct test *t)
{
	struct test_event_dev_stats stats;
	const int rx_enq = 0;
	int err;
	uint32_t i;

	if (init(t, 1, 4) < 0 ||
			create_ports(t, 4) < 0 ||
			create_atomic_qids(t, 1) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/* CQ mapping to QID */
	for (i = 0; i < 4; i++) {
		err = rte_event_port_link(evdev, t->port[i], &t->qid[0],
				NULL, 1);
		if (err != 1) {
			printf("%d: error mapping port 1 qid\n", __LINE__);
			return -1;
		}
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/*
	 * Send in a packet with an invalid qid to the scheduler.
	 * We should see the packed enqueued OK, but the inflights for
	 * that packet should not be incremented, and the rx_dropped
	 * should be incremented.
	 */
	static uint32_t flows1[] = {20};

	for (i = 0; i < RTE_DIM(flows1); i++) {
		struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);
		if (!arp) {
			printf("%d: gen of pkt failed\n", __LINE__);
			return -1;
		}

		struct rte_event ev = {
				.op = RTE_EVENT_OP_NEW,
				.queue_id = t->qid[0] + flows1[i],
				.flow_id = i,
				.mbuf = arp,
		};
		/* generate pkt and enqueue */
		err = rte_event_enqueue_burst(evdev, t->port[rx_enq], &ev, 1);
		if (err < 0) {
			printf("%d: Failed to enqueue\n", __LINE__);
			return -1;
		}
	}

	/* call the scheduler */
	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		return -1;
	}

	/*
	 * Now check the resulting inflights on the port, and the rx_dropped.
	 */
	if (stats.port_inflight[0] != 0) {
		printf("%d:%s: port 1 inflight count not correct\n", __LINE__,
				__func__);
		rte_event_dev_dump(evdev, stdout);
		return -1;
	}
	if (stats.port_rx_dropped[0] != 1) {
		printf("%d:%s: port 1 drops\n", __LINE__, __func__);
		rte_event_dev_dump(evdev, stdout);
		return -1;
	}
	/* each packet drop should only be counted in one place - port or dev */
	if (stats.rx_dropped != 0) {
		printf("%d:%s: port 1 dropped count not correct\n", __LINE__,
				__func__);
		rte_event_dev_dump(evdev, stdout);
		return -1;
	}

	cleanup(t);
	return 0;
}

static int
single_packet(struct test *t)
{
	const uint32_t MAGIC_SEQN = 7321;
	struct rte_event ev;
	struct test_event_dev_stats stats;
	const int rx_enq = 0;
	const int wrk_enq = 2;
	int err;

	/* Create instance with 4 ports */
	if (init(t, 1, 4) < 0 ||
			create_ports(t, 4) < 0 ||
			create_atomic_qids(t, 1) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/* CQ mapping to QID */
	err = rte_event_port_link(evdev, t->port[wrk_enq], NULL, NULL, 0);
	if (err != 1) {
		printf("%d: error mapping lb qid\n", __LINE__);
		cleanup(t);
		return -1;
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/************** Gen pkt and enqueue ****************/
	struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);
	if (!arp) {
		printf("%d: gen of pkt failed\n", __LINE__);
		return -1;
	}

	ev.op = RTE_EVENT_OP_NEW;
	ev.priority = RTE_EVENT_DEV_PRIORITY_NORMAL;
	ev.mbuf = arp;
	ev.queue_id = 0;
	ev.flow_id = 3;
	arp->seqn = MAGIC_SEQN;

	err = rte_event_enqueue_burst(evdev, t->port[rx_enq], &ev, 1);
	if (err < 0) {
		printf("%d: Failed to enqueue\n", __LINE__);
		return -1;
	}

	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		return -1;
	}

	if (stats.rx_pkts != 1 ||
			stats.tx_pkts != 1 ||
			stats.port_inflight[wrk_enq] != 1) {
		printf("%d: Sched core didn't handle pkt as expected\n",
				__LINE__);
		rte_event_dev_dump(evdev, stdout);
		return -1;
	}

	uint32_t deq_pkts;

	deq_pkts = rte_event_dequeue_burst(evdev, t->port[wrk_enq], &ev, 1, 0);
	if (deq_pkts < 1) {
		printf("%d: Failed to deq\n", __LINE__);
		return -1;
	}

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		return -1;
	}

	err = test_event_dev_stats_get(evdev, &stats);
	if (ev.mbuf->seqn != MAGIC_SEQN) {
		printf("%d: magic sequence number not dequeued\n", __LINE__);
		return -1;
	}

	rte_pktmbuf_free(ev.mbuf);
	err = rte_event_enqueue_burst(evdev, t->port[wrk_enq], &release_ev, 1);
	if (err < 0) {
		printf("%d: Failed to enqueue\n", __LINE__);
		return -1;
	}
	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_inflight[wrk_enq] != 0) {
		printf("%d: port inflight not correct\n", __LINE__);
		return -1;
	}

	cleanup(t);
	return 0;
}

static int
inflight_counts(struct test *t)
{
	struct rte_event ev;
	struct test_event_dev_stats stats;
	const int rx_enq = 0;
	const int p1 = 1;
	const int p2 = 2;
	int err;
	int i;

	/* Create instance with 4 ports */
	if (init(t, 2, 3) < 0 ||
			create_ports(t, 3) < 0 ||
			create_atomic_qids(t, 2) < 0) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/* CQ mapping to QID */
	err = rte_event_port_link(evdev, t->port[p1], &t->qid[0], NULL, 1);
	if (err != 1) {
		printf("%d: error mapping lb qid\n", __LINE__);
		cleanup(t);
		return -1;
	}
	err = rte_event_port_link(evdev, t->port[p2], &t->qid[1], NULL, 1);
	if (err != 1) {
		printf("%d: error mapping lb qid\n", __LINE__);
		cleanup(t);
		return -1;
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/************** FORWARD ****************/
#define QID1_NUM 5
	for (i = 0; i < QID1_NUM; i++) {
		struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);

		if (!arp) {
			printf("%d: gen of pkt failed\n", __LINE__);
			goto err;
		}

		ev.queue_id =  t->qid[0];
		ev.op = RTE_EVENT_OP_NEW;
		ev.mbuf = arp;
		err = rte_event_enqueue_burst(evdev, t->port[rx_enq], &ev, 1);
		if (err != 1) {
			printf("%d: Failed to enqueue\n", __LINE__);
			goto err;
		}
	}
#define QID2_NUM 3
	for (i = 0; i < QID2_NUM; i++) {
		struct rte_mbuf *arp = rte_gen_arp(0, t->mbuf_pool);

		if (!arp) {
			printf("%d: gen of pkt failed\n", __LINE__);
			goto err;
		}
		ev.queue_id =  t->qid[1];
		ev.op = RTE_EVENT_OP_NEW;
		ev.mbuf = arp;
		err = rte_event_enqueue_burst(evdev, t->port[rx_enq], &ev, 1);
		if (err != 1) {
			printf("%d: Failed to enqueue\n", __LINE__);
			goto err;
		}
	}

	/* schedule */
	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (err) {
		printf("%d: failed to get stats\n", __LINE__);
		goto err;
	}

	if (stats.rx_pkts != QID1_NUM + QID2_NUM ||
			stats.tx_pkts != QID1_NUM + QID2_NUM) {
		printf("%d: Sched core didn't handle pkt as expected\n",
				__LINE__);
		goto err;
	}

	if (stats.port_inflight[p1] != QID1_NUM) {
		printf("%d: %s port 1 inflight not correct\n", __LINE__,
				__func__);
		goto err;
	}
	if (stats.port_inflight[p2] != QID2_NUM) {
		printf("%d: %s port 2 inflight not correct\n", __LINE__,
				__func__);
		goto err;
	}

	/************** DEQUEUE INFLIGHT COUNT CHECKS  ****************/
	/* port 1 */
	struct rte_event events[QID1_NUM + QID2_NUM];
	uint32_t deq_pkts = rte_event_dequeue_burst(evdev, t->port[p1], events,
			RTE_DIM(events), 0);

	if (deq_pkts != QID1_NUM) {
		printf("%d: Port 1: DEQUEUE inflight failed\n", __LINE__);
		goto err;
	}
	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_inflight[p1] != QID1_NUM) {
		printf("%d: port 1 inflight decrement after DEQ != 0\n",
				__LINE__);
		goto err;
	}
	for (i = 0; i < QID1_NUM; i++) {
		err = rte_event_enqueue_burst(evdev, t->port[p1], &release_ev,
				1);
		if (err != 1) {
			printf("%d: %s rte enqueue of inf release failed\n",
				__LINE__, __func__);
			goto err;
		}
	}

	/*
	 * As the scheduler core decrements inflights, it needs to run to
	 * process packets to act on the drop messages
	 */
	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_inflight[p1] != 0) {
		printf("%d: port 1 inflight NON NULL after DROP\n", __LINE__);
		goto err;
	}

	/* port2 */
	deq_pkts = rte_event_dequeue_burst(evdev, t->port[p2], events,
			RTE_DIM(events), 0);
	if (deq_pkts != QID2_NUM) {
		printf("%d: Port 2: DEQUEUE inflight failed\n", __LINE__);
		goto err;
	}
	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_inflight[p2] != QID2_NUM) {
		printf("%d: port 1 inflight decrement after DEQ != 0\n",
				__LINE__);
		goto err;
	}
	for (i = 0; i < QID2_NUM; i++) {
		err = rte_event_enqueue_burst(evdev, t->port[p2], &release_ev,
				1);
		if (err != 1) {
			printf("%d: %s rte enqueue of inf release failed\n",
				__LINE__, __func__);
			goto err;
		}
	}

	/*
	 * As the scheduler core decrements inflights, it needs to run to
	 * process packets to act on the drop messages
	 */
	rte_event_schedule(evdev);

	err = test_event_dev_stats_get(evdev, &stats);
	if (stats.port_inflight[p2] != 0) {
		printf("%d: port 2 inflight NON NULL after DROP\n", __LINE__);
		goto err;
	}
	cleanup(t);
	return 0;

err:
	rte_event_dev_dump(evdev, stdout);
	cleanup(t);
	return -1;
}

static int
parallel_basic(struct test *t, int check_order)
{
	const uint8_t rx_port = 0;
	const uint8_t w1_port = 1;
	const uint8_t w3_port = 3;
	const uint8_t tx_port = 4;
	int err;
	int i;
	uint32_t deq_pkts, j;
	struct rte_mbuf *mbufs[3];
	struct rte_mbuf *mbufs_out[3];
	const uint32_t MAGIC_SEQN = 1234;

	/* Create instance with 4 ports */
	if (init(t, 2, tx_port + 1) < 0 ||
			create_ports(t, tx_port + 1) < 0 ||
			(check_order ?  create_ordered_qids(t, 1) :
				create_unordered_qids(t, 1)) < 0 ||
			create_directed_qids(t, 1, &tx_port)) {
		printf("%d: Error initializing device\n", __LINE__);
		return -1;
	}

	/*
	 * CQ mapping to QID
	 * We need three ports, all mapped to the same ordered qid0. Then we'll
	 * take a packet out to each port, re-enqueue in reverse order,
	 * then make sure the reordering has taken place properly when we
	 * dequeue from the tx_port.
	 *
	 * Simplified test setup diagram:
	 *
	 * rx_port        w1_port
	 *        \     /         \
	 *         qid0 - w2_port - qid1
	 *              \         /     \
	 *                w3_port        tx_port
	 */
	/* CQ mapping to QID for LB ports (directed mapped on create) */
	for (i = w1_port; i <= w3_port; i++) {
		err = rte_event_port_link(evdev, t->port[i], &t->qid[0], NULL,
				1);
		if (err != 1) {
			printf("%d: error mapping lb qid\n", __LINE__);
			cleanup(t);
			return -1;
		}
	}

	if (rte_event_dev_start(evdev) < 0) {
		printf("%d: Error with start call\n", __LINE__);
		return -1;
	}

	/* Enqueue 3 packets to the rx port */
	for (i = 0; i < 3; i++) {
		struct rte_event ev;
		mbufs[i] = rte_gen_arp(0, t->mbuf_pool);
		if (!mbufs[i]) {
			printf("%d: gen of pkt failed\n", __LINE__);
			return -1;
		}

		ev.queue_id = t->qid[0];
		ev.op = RTE_EVENT_OP_NEW;
		ev.mbuf = mbufs[i];
		mbufs[i]->seqn = MAGIC_SEQN + i;

		/* generate pkt and enqueue */
		err = rte_event_enqueue_burst(evdev, t->port[rx_port], &ev, 1);
		if (err != 1) {
			printf("%d: Failed to enqueue pkt %u, retval = %u\n",
					__LINE__, i, err);
			return -1;
		}
	}

	rte_event_schedule(evdev);

	/* use extra slot to make logic in loops easier */
	struct rte_event deq_ev[w3_port + 1];

	/* Dequeue the 3 packets, one from each worker port */
	for (i = w1_port; i <= w3_port; i++) {
		deq_pkts = rte_event_dequeue_burst(evdev, t->port[i],
				&deq_ev[i], 1, 0);
		if (deq_pkts != 1) {
			printf("%d: Failed to deq\n", __LINE__);
			rte_event_dev_dump(evdev, stdout);
			return -1;
		}
	}

	/* Enqueue each packet in reverse order, flushing after each one */
	for (i = w3_port; i >= w1_port; i--) {

		deq_ev[i].op = RTE_EVENT_OP_FORWARD;
		deq_ev[i].queue_id = t->qid[1];
		err = rte_event_enqueue_burst(evdev, t->port[i], &deq_ev[i], 1);
		if (err != 1) {
			printf("%d: Failed to enqueue\n", __LINE__);
			return -1;
		}
	}
	rte_event_schedule(evdev);

	/* dequeue from the tx ports, we should get 3 packets */
	deq_pkts = rte_event_dequeue_burst(evdev, t->port[tx_port], deq_ev,
			3, 0);

	/* Check to see if we've got all 3 packets */
	if (deq_pkts != 3) {
		printf("%d: expected 3 pkts at tx port got %d from port %d\n",
			__LINE__, deq_pkts, tx_port);
		rte_event_dev_dump(evdev, stdout);
		return 1;
	}

	/* Check to see if the sequence numbers are in expected order */
	if (check_order) {
		for (j = 0 ; j < deq_pkts ; j++) {
			if (deq_ev[j].mbuf->seqn != MAGIC_SEQN + j) {
				printf(
					"%d: Incorrect sequence number(%d) from port %d\n",
					__LINE__, mbufs_out[j]->seqn, tx_port);
				return -1;
			}
		}
	}

	/* Destroy the instance */
	cleanup(t);
	return 0;
}

static int
ordered_basic(struct test *t)
{
	return parallel_basic(t, 1);
}

static int
unordered_basic(struct test *t)
{
	return parallel_basic(t, 0);
}

static struct rte_mempool *eventdev_func_mempool;

static int
test_sw_eventdev(void)
{
	struct test *t = malloc(sizeof(struct test));
	int ret;

	/* manually initialize the op, older gcc's complain on static
	 * initialization of struct elements that are a bitfield.
	 */
	release_ev.op = RTE_EVENT_OP_RELEASE;

	const char *eventdev_name = "event_sw0";
	evdev = rte_event_dev_get_dev_id(eventdev_name);
	if (evdev < 0) {
		printf("%d: Eventdev %s not found - creating.\n",
				__LINE__, eventdev_name);
		if (rte_eal_vdev_init(eventdev_name, NULL) < 0) {
			printf("Error creating eventdev\n");
			return -1;
		}
		evdev = rte_event_dev_get_dev_id(eventdev_name);
		if (evdev < 0) {
			printf("Error finding newly created eventdev\n");
			return -1;
		}
	}

	/* Only create mbuf pool once, reuse for each test run */
	if (!eventdev_func_mempool) {
		eventdev_func_mempool = rte_pktmbuf_pool_create(
				"EVENTDEV_SW_SA_MBUF_POOL",
				(1<<12), /* 4k buffers */
				32 /*MBUF_CACHE_SIZE*/,
				0,
				512, /* use very small mbufs */
				rte_socket_id());
		if (!eventdev_func_mempool) {
			printf("ERROR creating mempool\n");
			return -1;
		}
	}
	t->mbuf_pool = eventdev_func_mempool;

	printf("*** Running Single Directed Packet test...\n");
	ret = test_single_directed_packet(t);
	if (ret != 0) {
		printf("ERROR - Single Directed Packet test FAILED.\n");
		return ret;
	}
	printf("*** Running Single Load Balanced Packet test...\n");
	ret = single_packet(t);
	if (ret != 0) {
		printf("ERROR - Single Packet test FAILED.\n");
		return ret;
	}
	printf("*** Running Unordered Basic test...\n");
	ret = unordered_basic(t);
	if (ret != 0) {
		printf("ERROR -  Unordered Basic test FAILED.\n");
		return ret;
	}
	printf("*** Running Ordered Basic test...\n");
	ret = ordered_basic(t);
	if (ret != 0) {
		printf("ERROR -  Ordered Basic test FAILED.\n");
		return ret;
	}
	printf("*** Running Burst Packets test...\n");
	ret = burst_packets(t);
	if (ret != 0) {
		printf("ERROR - Burst Packets test FAILED.\n");
		return ret;
	}
	printf("*** Running Invalid QID test...\n");
	ret = invalid_qid(t);
	if (ret != 0) {
		printf("ERROR - Invalid QID test FAILED.\n");
		return ret;
	}
	printf("*** Running Inflight Count test...\n");
	ret = inflight_counts(t);
	if (ret != 0) {
		printf("ERROR - Inflight Count test FAILED.\n");
		return ret;
	}
	printf("*** Running Abuse Inflights test...\n");
	ret = abuse_inflights(t);
	if (ret != 0) {
		printf("ERROR - Abuse Inflights test FAILED.\n");
		return ret;
	}
	printf("*** Running Ordered Reconfigure test...\n");
	ret = ordered_reconfigure(t);
	if (ret != 0) {
		printf("ERROR - Ordered Reconfigure test FAILED.\n");
		return ret;
	}
	printf("*** Running Port LB Single Reconfig test...\n");
	ret = port_single_lb_reconfig(t);
	if (ret != 0) {
		printf("ERROR - Port LB Single Reconfig test FAILED.\n");
		return ret;
	}
	printf("*** Running Port Reconfig Credits test...\n");
	ret = port_reconfig_credits(t);
	if (ret != 0) {
		printf("ERROR - Port Reconfig Credits Reset test FAILED.\n");
		return ret;
	}
	/*
	 * Free test instance, leaving mempool initialized, and a pointer to it
	 * in static eventdev_func_mempool, as it is re-used on re-runs
	 */
	free(t);

	return 0;
}

REGISTER_TEST_COMMAND(eventdev_sw_autotest, test_sw_eventdev);
