/*
 * netsniff-ng - the packet sniffing beast
 * By Daniel Borkmann <daniel@netsniff-ng.org>
 * Copyright 2009-2011 Daniel Borkmann.
 * Subject to the GPL.
 */

#include <stdio.h>
#include <string.h>
#include <curses.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>

#include "xmalloc.h"
#include "strlcpy.h"
#include "error_and_die.h"
#include "netdev.h"
#include "system.h"
#include "tty.h"
#include "version.h"
#include "signals.h"
#include "tx_ring.h"

struct counter {
	uint16_t id;
	uint8_t min;
	uint8_t max;
	uint8_t inc;
	uint8_t val;
	off_t off;
};

struct randomizer {
	uint8_t val;
	off_t off;
};

struct packet {
	uint8_t *payload;
	size_t plen;
	struct counter *cnt;
	size_t clen;
	struct randomizer *rnd;
	size_t rlen;
};

struct pktconf {
	unsigned long num;
	unsigned long gap;
	struct packet *pkts;
	size_t len;
};

struct stats {
	unsigned long tx_bytes;
	unsigned long tx_packets;
};

struct mode {
	struct stats stats;
	char *device;
	int cpu;
	/* 0 for automatic, > 0 for manual */
	unsigned int reserve_size;
};

#define CPU_UNKNOWN  -1
#define CPU_NOTOUCH  -2

static sig_atomic_t sigint = 0;

static const char *short_options = "d:c:n:t:vhS:HQb:B:";

static struct option long_options[] = {
	{"dev", required_argument, 0, 'd'},
	{"conf", required_argument, 0, 'c'},
	{"num", required_argument, 0, 'n'},
	{"gap", required_argument, 0, 't'},
	{"ring-size", required_argument, 0, 'S'},
	{"bind-cpu", required_argument, 0, 'b'},
	{"unbind-cpu", required_argument, 0, 'B'},
	{"prio-norm", no_argument, 0, 'H'},
	{"notouch-irq", no_argument, 0, 'Q'},
	{"version", no_argument, 0, 'v'},
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static inline uint8_t lcrand(uint8_t val)
{
	return (3 * val + 11) && 0xFF;
}

static void signal_handler(int number)
{
	switch (number) {
	case SIGINT:
		sigint = 1;
		break;
	case SIGHUP:
		break;
	default:
		break;
	}
}

static void header(void)
{
	printf("%s%s%s\n", colorize_start(bold), "trafgen "
	       VERSION_STRING, colorize_end());
}

static void help(void)
{
	printf("\ntrafgen %s, network packet generator\n",
	       VERSION_STRING);
	printf("http://www.netsniff-ng.org\n\n");
	printf("Usage: trafgen [options]\n");
	printf("Options:\n");
	printf("  -d|--dev <netdev>      TX Device\n");
	printf("  -c|--conf <file>       Packet configuration txf-file\n");
	printf("  -n|--num <uint>        TX mode\n");
	printf("  `--     0              Loop until interrupt (default)\n");
	printf("   `-     n              Send n packets and done\n");
	printf("  -t|--gap <interval>    Packet interval in msecs, def: 0\n");
	printf("  -S|--ring-size <size>  Manually set ring size to <size>:\n");
	printf("                         mmap space in KB/MB/GB, e.g. \'10MB\'\n");
	printf("  -H|--prio-norm         Do not high priorize process\n");
	printf("  -Q|--notouch-irq       Do not touch IRQ CPU affinity of NIC\n");
	printf("  -b|--bind-cpu <cpu>    Bind to specific CPU or CPU-range\n");
	printf("  -B|--unbind-cpu <cpu>  Forbid to use specific CPU or CPU-range\n");
	printf("  -v|--version           Print version\n");
	printf("  -h|--help              Print this help\n");
	printf("\n");
	printf("Example:\n");
	printf("  See trafgen.txf for configuration file examples.\n");
	printf("  trafgen --dev eth0 --conf trafgen.txf --prio-norm\n");
	printf("  trafgen --dev eth0 --conf trafgen.txf --num 100 --gap 5\n");
	printf("\n");
	printf("Please report bugs to <bugs@netsniff-ng.org>\n");
	printf("Copyright (C) 2011 Daniel Borkmann\n");
	printf("License: GNU GPL version 2\n");
	printf("This is free software: you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n\n");

	die();
}

static void version(void)
{
	printf("\ntrafgen %s, network packet generator\n",
	       VERSION_STRING);
	printf("http://www.netsniff-ng.org\n\n");
	printf("Please report bugs to <bugs@netsniff-ng.org>\n");
	printf("Copyright (C) 2011 Daniel Borkmann\n");
	printf("License: GNU GPL version 2\n");
	printf("This is free software: you are free to change and redistribute it.\n");
	printf("There is NO WARRANTY, to the extent permitted by law.\n\n");

	die();
}

static void tx_fire_or_die(struct mode *mode, struct pktconf *cfg)
{
	int sock, irq, ifindex, mtu;
	unsigned int size;
	size_t l;
	struct ring tx_ring;

	if (!mode || !cfg)
		panic("Panic over invalid args for TX trigger!\n");

	mtu = device_mtu(mode->device);
	for (l = 0; l < cfg->len; ++l)
		if (cfg->pkts[l].plen > mtu)
			panic("Device MTU < than your packet size!\n");

	sock = pf_socket();

	memset(&tx_ring, 0, sizeof(tx_ring));

	ifindex = device_ifindex(mode->device);
	size = ring_size(mode->device, mode->reserve_size);

	setup_tx_ring_layout(sock, &tx_ring, size);
	create_tx_ring(sock, &tx_ring);
	mmap_tx_ring(sock, &tx_ring);
	alloc_tx_ring_frames(&tx_ring);
	bind_tx_ring(sock, &tx_ring, ifindex);

	if (mode->cpu >= 0 && ifindex > 0) {
		irq = device_irq_number(mode->device);
		device_bind_irq_to_cpu(mode->cpu, irq);
		printf("IRQ: %s:%d > CPU%d\n", mode->device, irq, 
		       mode->cpu);
	}

	printf("MD: %s\n\n", !cfg->gap ? "FIRE" : "TX");

	while(likely(sigint == 0)) {
		; /* do stuff */
	}

	destroy_tx_ring(sock, &tx_ring);
	close(sock);
}

static inline char *getuint(char *in, uint32_t *out)
{
	char *pt = in, tmp;
	while (*in && (isdigit(*in) || isxdigit(*in) || *in == 'x'))
		in++;
	if (!*in)
		panic("Syntax error!\n");
	tmp = *in;
	*in = 0;
	*out = strtol(pt, NULL, 0);
	if (errno == EINVAL) {
		*out = strtol(pt, NULL, 16);
		if (errno == EINVAL)
			panic("Syntax error!\n");
	}
	*in = tmp;
	return in;
}

#define TYPE_NUM 0
#define TYPE_CNT 1
#define TYPE_RND 2
#define TYPE_EOL 3

static inline char *getuint_or_obj(char *in, uint32_t *out, int *type)
{
	if (*in == '\n') {
		*type = TYPE_EOL;
	} else if (*in == '$') {
		in++;
		if (!strncmp("II", in, strlen("II"))) {
			in += 2;
			in = getuint(in, out);
			*type = TYPE_CNT;
		} else if (!strncmp("PRB", in, strlen("PRB"))) {
			*type = TYPE_RND;
			in += 3;
		} else
			panic("Syntax error!\n");
	} else {
		in = getuint(in, out);
		*type = TYPE_NUM;
	}

	return in;
}

static inline char *skipchar(char *in, char c)
{
	if (*in != c)
		panic("Syntax error!\n");
	return ++in;
}

static inline char *skipchar_s(char *in, char c)
{
	in = skips(in);
	if (*in == '\n')
		return in;
	in = skipchar(in, c);
	in = skips(in);

	return in;
}

static void dump_conf(struct pktconf *cfg)
{
	size_t i, j;

	info("n %lu, gap %lu ms, pkts %zu\n", cfg->num, cfg->gap, cfg->len);
	if (cfg->len == 0)
		return;

	for (i = 0; i < cfg->len; ++i) {
		info("[%zu] pkt\n", i);
		info(" len %zu cnts %zu rnds %zu\n", cfg->pkts[i].plen,
		     cfg->pkts[i].clen, cfg->pkts[i].rlen);
		info(" payload ");
		for (j = 0; j < cfg->pkts[i].plen; ++j)
			info("%02x ", cfg->pkts[i].payload[j]);
		info("\n");
		for (j = 0; j < cfg->pkts[i].clen; ++j)
			info(" cnt%zu [%u,%u], inc %u, off %zu\n",
			     j, cfg->pkts[i].cnt[j].min,
			     cfg->pkts[i].cnt[j].max,
			     cfg->pkts[i].cnt[j].inc,
			     cfg->pkts[i].cnt[j].off);
		for (j = 0; j < cfg->pkts[i].rlen; ++j)
			info(" rnd%zu off %zu\n",
			     cfg->pkts[i].rnd[j].off);
	}
}

/* Seems to need a rewrite later ;-) */
static void parse_conf_or_die(char *file, struct pktconf *cfg)
{
	int withinpkt = 0;
	unsigned long line = 0;
	char *pb, buff[1024];
	FILE *fp;
	struct counter *cnts = NULL;
	size_t l = 0;
	off_t offset = 0;

	if (!file || !cfg)
		panic("Panic over invalid args for the parser!\n");

	fp = fopen(file, "r");
	if (!fp)
		panic("Cannot open config file!\n");
	memset(buff, 0, sizeof(buff));

	info("CFG:\n");
	srand(time(NULL));

	while (fgets(buff, sizeof(buff), fp) != NULL) {
		line++;
		buff[sizeof(buff) - 1] = 0;
		pb = skips(buff);

		/* A comment or junk. Skip this line */
		if (*pb == '#' || *pb == '\n') {
			memset(buff, 0, sizeof(buff));
			continue;
		}

		if (!withinpkt && *pb == '$') {
			pb++;
			if (!strncmp("II", pb, strlen("II"))) {
				uint32_t id, min = 0, max = 0xFF, inc = 1;
				pb += 2;
				pb = getuint(pb, &id);
				pb = skipchar(pb, ':');
				pb = skips(pb);
				pb = getuint(pb, &min);
				pb = skipchar(pb, ',');
				pb = getuint(pb, &max);
				pb = skipchar(pb, ',');
				pb = getuint(pb, &inc);
				l++;
				cnts = xrealloc(cnts, 1, l * sizeof(*cnts));
				cnts[l - 1].id = 0xFF & id;
				cnts[l - 1].min = 0xFF & min;
				cnts[l - 1].max = 0xFF & max;
				cnts[l - 1].inc = 0xFF & inc;
			} else if (!strncmp("P", pb, strlen("P"))) {
				uint32_t id;
				pb++;
				pb = getuint(pb, &id);
				pb = skips(pb);
				pb = skipchar(pb, '{');
				withinpkt = 1;
				cfg->len++;
				cfg->pkts = xrealloc(cfg->pkts, 1,
						     cfg->len * sizeof(*cfg->pkts));
				memset(&cfg->pkts[cfg->len - 1], 0,
				       sizeof(cfg->pkts[cfg->len - 1]));
				offset = 0;
			} else 
				panic("Unknown instruction! Syntax error "
				      "on line %lu!\n", line);
		} else if (withinpkt && *pb == '}')
				withinpkt = 0;
		else if (withinpkt) {
			int type, i, found;
			uint32_t val;
			while (1) {
				found = 0;
				pb = getuint_or_obj(pb, &val, &type);
				if (type == TYPE_EOL)
					break;
				if (type == TYPE_CNT) {
					size_t z;
					struct counter *new;
					for (i = 0; i < l; ++i) {
						if (val == cnts[i].id) {
							found = 1;
							break;
						}
					}
					if (!found)
						panic("Counter %u not found!\n");

					val = cnts[i].min;
					z = ++(cfg->pkts[cfg->len - 1].clen);
					cfg->pkts[cfg->len - 1].cnt =
						xrealloc(cfg->pkts[cfg->len - 1].cnt,
							 1, z * sizeof(struct counter));
					new = &cfg->pkts[cfg->len - 1].cnt[z - 1];
					new->min = cnts[i].min;
					new->max = cnts[i].max;
					new->inc = cnts[i].inc;
					new->off = offset;
					new->val = val;
				} else if (type == TYPE_RND) {
					size_t z;
					struct randomizer *new;

					val = 0xFF & rand();
					z = ++(cfg->pkts[cfg->len - 1].rlen);
					cfg->pkts[cfg->len - 1].rnd =
						xrealloc(cfg->pkts[cfg->len - 1].rnd,
							 1, z * sizeof(struct randomizer));
					new = &cfg->pkts[cfg->len - 1].rnd[z - 1];
					new->val = val;
					new->off = offset;
				}

				cfg->pkts[cfg->len - 1].plen++;
				cfg->pkts[cfg->len - 1].payload =
					xrealloc(cfg->pkts[cfg->len - 1].payload,
						 1, cfg->pkts[cfg->len - 1].plen);
				cfg->pkts[cfg->len - 1].payload[cfg->pkts[cfg->len - 1].plen - 1] =
					0xFF & val;
				offset++;
				pb = skipchar_s(pb, ',');
			}
		} else
			panic("Syntax error!\n");
		memset(buff, 0, sizeof(buff));
	}

	fclose(fp);
	xfree(cnts);

	dump_conf(cfg);
}

static void cleanup_cfg(struct pktconf *cfg)
{
	size_t l;

	for (l = 0; l < cfg->len; ++l) {
		if (cfg->pkts[l].plen > 0)
			xfree(cfg->pkts[l].payload);
		if (cfg->pkts[l].clen > 0)
			xfree(cfg->pkts[l].cnt);
		if (cfg->pkts[l].rlen > 0)
			xfree(cfg->pkts[l].rnd);
	}

	if (cfg->len > 0)
		xfree(cfg->pkts);
}

static int main_loop(struct mode *mode, char *confname, unsigned long pkts,
		     unsigned long gap)
{
	struct pktconf cfg = {
		.num = pkts,
		.gap = gap,
		.len = 0,
	};

	parse_conf_or_die(confname, &cfg);
	tx_fire_or_die(mode, &cfg);
	cleanup_cfg(&cfg);

	return 0;
}

int main(int argc, char **argv)
{
	int c, opt_index, ret, i, j;
	char *confname = NULL, *ptr;
	unsigned long pkts = 0, gap = 0;
	bool prio_high = true;
	struct mode mode;

	check_for_root_maybe_die();

	memset(&mode, 0, sizeof(mode));
	mode.cpu = CPU_UNKNOWN;

	while ((c = getopt_long(argc, argv, short_options, long_options,
	       &opt_index)) != EOF) {
		switch (c) {
		case 'h':
			help();
			break;
		case 'v':
			version();
			break;
		case 'd':
			mode.device = xstrndup(optarg, IFNAMSIZ);
			break;
		case 'c':
			confname = xstrdup(optarg);
			break;
		case 'n':
			pkts = atol(optarg);
			break;
		case 't':
			gap = atol(optarg);
			break;
		case 'S':
			ptr = optarg;
			mode.reserve_size = 0;

			for (j = i = strlen(optarg); i > 0; --i) {
				if (!isdigit(optarg[j - i]))
					break;
				ptr++;
			}

			if (!strncmp(ptr, "KB", strlen("KB")))
				mode.reserve_size = 1 << 10;
			else if (!strncmp(ptr, "MB", strlen("MB")))
				mode.reserve_size = 1 << 20;
			else if (!strncmp(ptr, "GB", strlen("GB")))
				mode.reserve_size = 1 << 30;
			else
				panic("Syntax error in ring size param!\n");

			*ptr = 0;
			mode.reserve_size *= atoi(optarg);
			break;
		case 'b':
			set_cpu_affinity(optarg, 0);
			/* Take the first CPU for rebinding the IRQ */
			if (mode.cpu != CPU_NOTOUCH)
				mode.cpu = atoi(optarg);
			break;
		case 'B':
			set_cpu_affinity(optarg, 1);
			break;
		case 'H':
			prio_high = false;
			break;
		case 'Q':
			mode.cpu = CPU_NOTOUCH;
			break;
		case '?':
			switch (optopt) {
			case 'd':
			case 'c':
			case 'n':
			case 'S':
			case 'b':
			case 'B':
			case 't':
				error_and_die(EXIT_FAILURE, "Option -%c "
					      "requires an argument!\n",
					      optopt);
			default:
				if (isprint(optopt))
					whine("Unknown option character "
					      "`0x%X\'!\n", optopt);
				die();
			}
		default:
			break;
		}
	}

	if (argc < 5)
		help();
	if (mode.device == NULL)
		error_and_die(EXIT_FAILURE, "No networking device given!\n");
	if (confname == NULL)
		error_and_die(EXIT_FAILURE, "No configuration file given!\n");
	if (device_mtu(mode.device) == 0)
		error_and_die(EXIT_FAILURE, "This is no networking device!\n");

	register_signal(SIGINT, signal_handler);
	register_signal(SIGHUP, signal_handler);
	register_signal(SIGSEGV, muntrace_handler);

	header();

	if (prio_high == true) {
		set_proc_prio(DEFAULT_PROCESS_PRIO);
		set_sched_status(DEFAULT_SCHED_POLICY, DEFAULT_SCHED_PRIO);
	}

	ret = main_loop(&mode, confname, pkts, gap);

	xfree(mode.device);
	xfree(confname);
	return ret;
}
