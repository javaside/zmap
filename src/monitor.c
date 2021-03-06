/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

// module responsible for printing on-screen updates during the scan process

#include "monitor.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "iterator.h"
#include "recv.h"
#include "state.h"

#include "../lib/logger.h"
#include "../lib/xalloc.h"
#include "../lib/lockfd.h"

#define UPDATE_INTERVAL 1 //seconds
#define NUMBER_STR_LEN 20

// internal monitor status that is used to track deltas
typedef struct internal_scan_status {
	double   last_now;
	uint32_t last_sent;
	uint32_t last_send_failures;
	uint32_t last_recv_net_success;
	uint32_t last_recv_app_success;
	uint32_t last_pcap_drop;

} int_status_t;

// exportable status information that can be printed to screen
typedef struct export_scan_status {
	uint32_t total_sent;
	uint32_t recv_success_unique;
	uint32_t app_recv_success_unique;
	uint32_t complete;
	uint32_t send_threads;
	double percent_complete;

	float hitrate; // network, e.g. SYN-ACK vs RST
	float app_hitrate; // application level, e.g. DNS response versus correct lookup.

	float send_rate;
	char send_rate_str[NUMBER_STR_LEN];
	float send_rate_avg;
	char send_rate_avg_str[NUMBER_STR_LEN];

	float recv_rate;
	char recv_rate_str[NUMBER_STR_LEN];
	float recv_avg;
	char recv_avg_str[NUMBER_STR_LEN];

	float app_success_rate;
	char app_success_rate_str[NUMBER_STR_LEN];
	float app_success_avg;
	char app_success_avg_str[NUMBER_STR_LEN];

	uint32_t pcap_drop;
	uint32_t pcap_ifdrop;
	uint32_t pcap_drop_total;
	char pcap_drop_total_str[NUMBER_STR_LEN];
	float pcap_drop_last;
	char pcap_drop_last_str[NUMBER_STR_LEN];
	float pcap_drop_avg;
	char pcap_drop_avg_str[NUMBER_STR_LEN];

	uint32_t time_remaining;
	char time_remaining_str[NUMBER_STR_LEN];
	uint32_t time_past;
	char time_past_str[NUMBER_STR_LEN];

	uint32_t fail_total;
	float fail_avg;
	float fail_last;

} export_status_t;

// find minimum of an array of doubles
static double min_d(double array[], int n)
{
	double value=INFINITY;
	for (int i=0; i<n; i++) {
		if (array[i] < value) {
			value = array[i];
		}
	}
	return value;
}

// pretty print elapsed (or estimated) number of seconds
static void time_string(uint32_t time, int est, char *buf, size_t len)
{
  	int y = time / 31556736;
  	int d = (time % 31556736) / 86400;
	int h = (time % 86400) / 3600;
	int m = (time % 3600) / 60;
	int s = time % 60;

	if (est) {
		if (y > 0) {
			snprintf(buf, len, "%d years", y);
		} else if (d > 9) {
			snprintf(buf, len, "%dd", d);
		} else if (d > 0) {
			snprintf(buf, len, "%dd%02dh", d, h);
		} else if (h > 9) {
			snprintf(buf, len, "%dh", h);
		} else if (h > 0) {
			snprintf(buf, len, "%dh%02dm", h, m);
		} else if (m > 9) {
			snprintf(buf, len, "%dm", m);
		} else if (m > 0) {
			snprintf(buf, len, "%dm%02ds", m, s);
		} else {
			snprintf(buf, len, "%ds", s);
		}
	} else {
		if (d > 0) {
			snprintf(buf, len, "%dd%d:%02d:%02d", d, h, m, s);
		} else if (h > 0) {
			snprintf(buf, len, "%d:%02d:%02d", h, m, s);
		} else {
			snprintf(buf, len, "%d:%02d", m, s);
		}
	}
}

// pretty print quantities
static void number_string(uint32_t n, char *buf, size_t len)
{
	int figs = 0;
	if (n < 1000) {
		snprintf(buf, len, "%u ", n);
	} else if (n < 1000000) {
		if (n < 10000) {
		figs = 2;
		} else if (n < 100000) {
		figs = 1;
		}
		snprintf(buf, len, "%0.*f K", figs, (float)n/1000.);
	} else {
		if (figs < 10000000) {
		figs = 2;
		} else if (figs < 100000000) {
			figs = 1;
		}
		snprintf(buf, len, "%0.*f M", figs, (float)n/1000000.);
	}
}

// estimate time remaining time based on config and state
double compute_remaining_time(double age, uint64_t sent)
{
	if (!zsend.complete) {
		double remaining[] = {INFINITY, INFINITY, INFINITY};
		if (zsend.targets) {
			double done = (double) sent/zsend.targets;
			remaining[0] = (1. - done)*(age/done) + zconf.cooldown_secs;
		}
		if (zconf.max_runtime) {
			remaining[1] = (zconf.max_runtime - age)+zconf.cooldown_secs;
		}
		if (zconf.max_results) {
			double done = (double)zrecv.success_unique/zconf.max_results;
			remaining[2] = (1. - done)*(age/done);
		}
		return min_d(remaining, sizeof(remaining)/sizeof(double));
	} else {
		return zconf.cooldown_secs - (now() - zsend.finish);
	}
}


static void update_stats(int_status_t *intrnl, iterator_t *it)
{
	intrnl->last_now = now();
	intrnl->last_sent = iterator_get_sent(it);
	intrnl->last_recv_net_success = zrecv.success_unique;
	intrnl->last_recv_app_success = zrecv.app_success_unique;
	intrnl->last_pcap_drop = zrecv.pcap_drop + zrecv.pcap_ifdrop;
	intrnl->last_send_failures = zsend.sendto_failures;
}	

static void update_pcap_stats(pthread_mutex_t *recv_ready_mutex)
{
	// ask pcap for fresh values
	pthread_mutex_lock(recv_ready_mutex);
	recv_update_pcap_stats();
	pthread_mutex_unlock(recv_ready_mutex);	
}

static void export_stats(int_status_t *intrnl, export_status_t *exp, iterator_t *it)
{
	uint32_t total_sent = iterator_get_sent(it);
	double age = now() - zsend.start; // time of entire scan
	double delta = now() - intrnl->last_now; // time since the last time we updated
	double remaining_secs = compute_remaining_time(age, total_sent);

	// export amount of time the scan has been running
	if (age < 5) {
		exp->time_remaining_str[0] = '\0';
	} else {
		char buf[20];
		time_string(ceil(remaining_secs), 1, buf, sizeof(buf));
		snprintf(exp->time_remaining_str, NUMBER_STR_LEN, " (%s left)", buf);
	}
	exp->time_past = age;
	exp->time_remaining = remaining_secs;
	time_string((int)age, 0, exp->time_past_str, NUMBER_STR_LEN);

	// export recv statistics
	exp->recv_rate = (zrecv.success_unique - intrnl->last_recv_net_success)/delta;
	number_string(exp->recv_rate, exp->recv_rate_str, NUMBER_STR_LEN);
	exp->recv_avg = zrecv.success_unique/age;
	number_string(exp->recv_avg, exp->recv_avg_str, NUMBER_STR_LEN);

	// application level statistics
	if (zconf.fsconf.app_success_index >= 0) {
		exp->app_success_rate = (zrecv.app_success_unique - intrnl->last_recv_app_success)/delta;
		number_string(exp->app_success_rate, exp->app_success_rate_str, NUMBER_STR_LEN);
		exp->app_success_avg = (zrecv.app_success_unique/age);
		number_string(exp->app_success_avg, exp->app_success_avg_str, NUMBER_STR_LEN);
	}

	if (!total_sent) {
		exp->hitrate = 0;
		exp->app_hitrate = 0;
	} else {
		exp->hitrate = zrecv.success_unique*100./total_sent;
		exp->app_hitrate = zrecv.app_success_unique*100./total_sent;
	}

	if (!zsend.complete) {
		exp->send_rate = (total_sent - intrnl->last_sent)/delta;
		number_string(exp->send_rate, exp->send_rate_str, NUMBER_STR_LEN);
		exp->send_rate_avg = total_sent/age;
		number_string(exp->send_rate_avg, exp->send_rate_avg_str, NUMBER_STR_LEN);
	} else {
		exp->send_rate_avg = total_sent/(zsend.finish - zsend.start);
		number_string(exp->send_rate_avg, exp->send_rate_avg_str, NUMBER_STR_LEN);
	}
	// export other pre-calculated values
	exp->total_sent = total_sent;
	exp->percent_complete = 100.*age/(age + remaining_secs);
	exp->recv_success_unique = zrecv.success_unique;
	exp->app_recv_success_unique = zrecv.app_success_unique;
	exp->complete = zsend.complete;

	// pcap dropped packets
	exp->pcap_drop = zrecv.pcap_drop;
	exp->pcap_ifdrop = zrecv.pcap_ifdrop;
	exp->pcap_drop_total = zrecv.pcap_drop + zrecv.pcap_ifdrop;
	exp->pcap_drop_last = (zrecv.pcap_drop + zrecv.pcap_ifdrop - intrnl->last_pcap_drop)/delta;
	exp->pcap_drop_avg = (zrecv.pcap_drop + zrecv.pcap_ifdrop)/age;
	number_string(exp->pcap_drop_total, exp->pcap_drop_total_str, NUMBER_STR_LEN);
	number_string(exp->pcap_drop_last, exp->pcap_drop_last_str, NUMBER_STR_LEN);
	number_string(exp->pcap_drop_avg, exp->pcap_drop_avg_str, NUMBER_STR_LEN);

	exp->fail_total = zsend.sendto_failures;
	exp->fail_last = (zsend.sendto_failures - intrnl->last_send_failures) / delta;
	exp->fail_avg = zsend.sendto_failures/age;

	// misc
	exp->send_threads = iterator_get_curr_send_threads(it);
}

static void log_drop_warnings(export_status_t *exp)
{
	if (exp->pcap_drop_last/exp->recv_rate > 0.05) {
		log_warn("monitor", "Dropped %d packets in the last second, (%d total dropped (pcap: %d + iface: %d))",
				 exp->pcap_drop_last, exp->pcap_drop_total, exp->pcap_drop, exp->pcap_ifdrop);
	}
	if (exp->fail_last/exp->send_rate > 0.01) {
		log_warn("monitor", "Failed to send %d packets/sec (%d total failures)",
				 exp->fail_last, exp->fail_total);
	}
}

static void onscreen_appsuccess(export_status_t *exp)
{
	// this when probe module handles application-level success rates
	if (!exp->complete) {
		fprintf(stderr,
				"%5s %0.0f%%%s; sent: %u %sp/s (%sp/s avg); "
				"recv: %u %sp/s (%sp/s avg); "
				"app success: %u %sp/s (%sp/s avg); "
				"drops: %sp/s (%sp/s avg); "
				"hitrate: %0.2f%% "
				"app hitrate: %0.2f%%\n",
				exp->time_past_str,
				exp->percent_complete,
				exp->time_remaining_str,
				exp->total_sent,
				exp->send_rate_str,
				exp->send_rate_avg_str,
				exp->recv_success_unique,
				exp->recv_rate_str,
				exp->recv_avg_str,
				exp->app_recv_success_unique,
				exp->app_success_rate_str,
				exp->app_success_avg_str,
				exp->pcap_drop_last_str,
				exp->pcap_drop_avg_str,
				exp->hitrate,
				exp->app_hitrate);
	} else {
		fprintf(stderr,
				"%5s %0.0f%%%s; sent: %u done (%sp/s avg); "
				"recv: %u %sp/s (%sp/s avg); "
				"app success: %u %sp/s (%sp/s avg); "
				"drops: %sp/s (%sp/s avg); "
				"hitrate: %0.2f%% "
				"app hitrate: %0.2f%%\n",
				exp->time_past_str,
				exp->percent_complete,
				exp->time_remaining_str,
				exp->total_sent,
				exp->send_rate_avg_str,
				exp->recv_success_unique,
				exp->recv_rate_str,
				exp->recv_avg_str,
				exp->app_recv_success_unique,
				exp->app_success_rate_str,
				exp->app_success_avg_str,
				exp->pcap_drop_last_str,
				exp->pcap_drop_avg_str,
				exp->hitrate,
				exp->app_hitrate);
	}
}

static void onscreen_generic(export_status_t *exp)
{
	if (!exp->complete) {
		fprintf(stderr,
				"%5s %0.0f%%%s; send: %u %sp/s (%sp/s avg); "
				"recv: %u %sp/s (%sp/s avg); "
				"drops: %sp/s (%sp/s avg); "
				"hitrate: %0.2f%%\n",
				exp->time_past_str,
				exp->percent_complete,
				exp->time_remaining_str,
				exp->total_sent,
				exp->send_rate_str,
				exp->send_rate_avg_str,
				exp->recv_success_unique,
				exp->recv_rate_str,
				exp->recv_avg_str,
				exp->pcap_drop_last_str,
				exp->pcap_drop_avg_str,
				exp->hitrate);
	} else {
		fprintf(stderr,
				"%5s %0.0f%%%s; send: %u done (%sp/s avg); "
				"recv: %u %sp/s (%sp/s avg); "
				"drops: %sp/s (%sp/s avg); "
				"hitrate: %0.2f%%\n",
				exp->time_past_str,
				exp->percent_complete,
				exp->time_remaining_str,
				exp->total_sent,
				exp->send_rate_avg_str,
				exp->recv_success_unique,
				exp->recv_rate_str,
				exp->recv_avg_str,
				exp->pcap_drop_last_str,
				exp->pcap_drop_avg_str,
				exp->hitrate);
	}
	fflush(stderr);
}

static FILE* init_status_update_file(char *path)
{
		FILE *f = fopen(path, "wb");
		if (!f) {
			log_fatal("csv", "could not open output file (%s)",
					zconf.status_updates_file);
		}
		log_trace("monitor", "status updates CSV will be saved to %s",
				zconf.status_updates_file);
		fprintf(f, 
				"real-time,time-elapsed,time-remaining,"
				"percent-complete,active-send-threads,"
				"sent-total,sent-last-one-sec,sent-avg-per-sec," 
				"recv-success-total,recv-success-last-one-sec,recv-success-avg-per-sec," 
				"pcap-drop-total,drop-last-one-sec,drop-avg-per-sec,"
				"sendto-fail-total,sendto-fail-last-one-sec,sendto-fail-avg-per-sec\n"
			);
		fflush(f);
		return f;
}

static void update_status_updates_file(export_status_t *exp, FILE *f)
{
	struct timeval now;
	char timestamp[256];
	gettimeofday(&now, NULL);
	time_t sec = now.tv_sec;
	struct tm* ptm = localtime(&sec);
	strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", ptm);
	
	fprintf(f,
			"%s,%u,%u,%f,%u,%u,%f,%f,%u,%f,%f,%u,%f,%f,%u,%f,%f\n",
			timestamp, exp->time_past, exp->time_remaining,
			exp->percent_complete, exp->send_threads,
			exp->total_sent, exp->send_rate, exp->send_rate_avg,
			exp->recv_success_unique, exp->recv_rate, exp->recv_avg,
			exp->pcap_drop_total, exp->pcap_drop_last, exp->pcap_drop_avg,
			exp->fail_total, exp->fail_last, exp->fail_avg);
	fflush(f);
}


void monitor_run(iterator_t *it, pthread_mutex_t *lock)
{
	int_status_t *internal_status = xmalloc(sizeof(int_status_t));
	export_status_t *export_status = xmalloc(sizeof(export_status_t));

	FILE *f = NULL;
	if (zconf.status_updates_file) {
		f = init_status_update_file(zconf.status_updates_file);
	}

	while (!(zsend.complete && zrecv.complete)) {
		update_pcap_stats(lock);
		export_stats(internal_status, export_status, it);
		log_drop_warnings(export_status);
		if (!zconf.quiet) {
			lock_file(stderr);
			if (zconf.fsconf.app_success_index >= 0) {
				onscreen_appsuccess(export_status);
			} else {
				onscreen_generic(export_status);
			}
			unlock_file(stderr);
		}
		if (f) {
			update_status_updates_file(export_status, f);
		}
		update_stats(internal_status, it);
		sleep(UPDATE_INTERVAL);
	}
	if (!zconf.quiet) {
		fflush(stderr);
	}
	if (f) {
		fflush(f);
		fclose(f);
	}
}

