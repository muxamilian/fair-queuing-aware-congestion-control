/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h> 
#include "cc_common.h"

/* Many congestion control algorithms run a parallel version of new reno in order
 * to provide a lower bound estimate of either the congestion window or the
 * the minimal bandwidth. This implementation of new reno does not directly
 * refer to the connection and path variables (e.g. cwin) but instead sets
 * its entire state in memory.
 */

void picoquic_tonopah_sim_reset(picoquic_tonopah_sim_state_t * nrss)
{
    /* Initialize the state of the congestion control algorithm */
    memset(nrss, 0, sizeof(picoquic_tonopah_sim_state_t));
    nrss->alg_state = picoquic_tonopah_alg_slow_start;
    nrss->ssthresh = UINT64_MAX;
    nrss->cwin = PICOQUIC_CWIN_INITIAL;
}

/* The recovery state last 1 RTT, during which parameters will be frozen
 */
static void picoquic_tonopah_sim_enter_recovery(
    picoquic_tonopah_sim_state_t* nr_state,
    picoquic_cnx_t* cnx,
    picoquic_path_t * path_x,
    picoquic_congestion_notification_t notification,
    uint64_t current_time)
{
    nr_state->ssthresh = nr_state->cwin / 2;
    if (nr_state->ssthresh < PICOQUIC_CWIN_MINIMUM) {
        nr_state->ssthresh = PICOQUIC_CWIN_MINIMUM;
    }

    if (notification == picoquic_congestion_notification_timeout) {
        nr_state->cwin = PICOQUIC_CWIN_MINIMUM;
        nr_state->alg_state = picoquic_tonopah_alg_slow_start;
    }
    else {
        nr_state->cwin = nr_state->ssthresh;
        nr_state->alg_state = picoquic_tonopah_alg_congestion_avoidance;
    }

    nr_state->recovery_start = current_time;
    nr_state->recovery_sequence = picoquic_cc_get_sequence_number(cnx, path_x);
    nr_state->residual_ack = 0;
}

/* Update cwin per signaled bandwidth
 */
static void picoquic_tonopah_sim_seed_cwin(picoquic_tonopah_sim_state_t* nr_state,
    picoquic_path_t* path_x, uint64_t bytes_in_flight)
{
    if (nr_state->alg_state == picoquic_tonopah_alg_slow_start &&
        nr_state->ssthresh == UINT64_MAX) {
        if (bytes_in_flight > nr_state->cwin) {
            nr_state->cwin = bytes_in_flight;
            nr_state->ssthresh = bytes_in_flight;
            nr_state->alg_state = picoquic_tonopah_alg_congestion_avoidance;
        }
    }
}


/* Notification API for new Reno simulations.
 */
void picoquic_tonopah_sim_notify(
    picoquic_tonopah_sim_state_t* nr_state,
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t nb_bytes_acknowledged,
    uint64_t current_time)
{
    uint64_t smoothed_rtt = path_x->smoothed_rtt;
    if (cnx->nb_paths > 1) {
        assert(cnx->nb_paths == 2);
        smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
    }
    switch (notification) {
    case picoquic_congestion_notification_acknowledgement: {
        switch (nr_state->alg_state) {
        case picoquic_tonopah_alg_slow_start:
            nr_state->cwin += nb_bytes_acknowledged;
            /* if cnx->cwin exceeds SSTHRESH, exit and go to CA */
            if (nr_state->cwin >= nr_state->ssthresh) {
                nr_state->alg_state = picoquic_tonopah_alg_congestion_avoidance;
            }
            break;
        case picoquic_tonopah_alg_congestion_avoidance:
        default: {
            uint64_t complete_delta = nb_bytes_acknowledged * path_x->send_mtu + nr_state->residual_ack;
            nr_state->residual_ack = complete_delta % nr_state->cwin;
            nr_state->cwin += complete_delta / nr_state->cwin;
            break;
        }
        }
        break;
    }
    case picoquic_congestion_notification_ecn_ec:
    case picoquic_congestion_notification_repeat:
    case picoquic_congestion_notification_timeout:
        /* enter recovery */
        if (!cnx->is_multipath_enabled) {
            if (current_time - nr_state->recovery_start > smoothed_rtt ||
                nr_state->recovery_sequence <= picoquic_cc_get_ack_number(cnx, path_x)) {
                picoquic_tonopah_sim_enter_recovery(nr_state, cnx, path_x, notification, current_time);
            }
        }
        else {
            if (current_time - nr_state->recovery_start > smoothed_rtt ||
                nr_state->recovery_start <= picoquic_cc_get_ack_sent_time(cnx, path_x)) {
                picoquic_tonopah_sim_enter_recovery(nr_state, cnx, path_x, notification, current_time);
            }
        }
        break;
    case picoquic_congestion_notification_spurious_repeat:
        if (!cnx->is_multipath_enabled) {
            if (current_time - nr_state->recovery_start < smoothed_rtt &&
                nr_state->recovery_sequence > picoquic_cc_get_ack_number(cnx, path_x)) {
                /* If spurious repeat of initial loss detected,
                 * exit recovery and reset threshold to pre-entry cwin.
                 */
                if (nr_state->ssthresh != UINT64_MAX &&
                    nr_state->cwin < 2 * nr_state->ssthresh) {
                    nr_state->cwin = 2 * nr_state->ssthresh;
                    nr_state->alg_state = picoquic_tonopah_alg_congestion_avoidance;
                }
            }
        }
        else {
            if (current_time - nr_state->recovery_start < smoothed_rtt &&
                nr_state->recovery_start > picoquic_cc_get_ack_sent_time(cnx, path_x)) {
                /* If spurious repeat of initial loss detected,
                 * exit recovery and reset threshold to pre-entry cwin.
                 */
                if (nr_state->ssthresh != UINT64_MAX &&
                    nr_state->cwin < 2 * nr_state->ssthresh) {
                    nr_state->cwin = 2 * nr_state->ssthresh;
                    nr_state->alg_state = picoquic_tonopah_alg_congestion_avoidance;
                }
            }
        }
        break;
    case picoquic_congestion_notification_bw_measurement:
        break;
    case picoquic_congestion_notification_reset:
        picoquic_tonopah_sim_reset(nr_state);
        break;
    case picoquic_congestion_notification_seed_cwin:
        picoquic_tonopah_sim_seed_cwin(nr_state, path_x, nb_bytes_acknowledged);
        break;
    default:
        /* ignore */
        break;
    }
}


/* Actual implementation of New Reno, when used as a stand alone algorithm
 */

typedef struct st_picoquic_tonopah_state_t {
    picoquic_tonopah_sim_state_t nrss;
    picoquic_min_max_rtt_t rtt_filter;
} picoquic_tonopah_state_t;

static void picoquic_tonopah_reset(picoquic_tonopah_state_t* nr_state, picoquic_path_t* path_x)
{
    memset(nr_state, 0, sizeof(picoquic_tonopah_state_t));
    picoquic_tonopah_sim_reset(&nr_state->nrss);
    path_x->cwin = nr_state->nrss.cwin;
}

static void picoquic_tonopah_init(picoquic_path_t* path_x, uint64_t current_time)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_tonopah_state_t* nr_state = (picoquic_tonopah_state_t*)malloc(sizeof(picoquic_tonopah_state_t));
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(current_time);
#endif

    if (nr_state != NULL) {
        picoquic_tonopah_reset(nr_state, path_x);
        path_x->congestion_alg_state = nr_state;
    }
    else {
        path_x->congestion_alg_state = NULL;
    }
}

uint64_t updated_path0 = 0;
uint64_t updated_path1 = 0;

uint64_t last_change = 0;

double ratio = 0.75;

picoquic_path_t* path1 = NULL;
picoquic_path_t* path2 = NULL;
picoquic_path_t* dominant_path = NULL;

typedef struct st_picoquic_tonopah_interval_info_t {
    uint64_t first_seq_num;
    uint64_t last_seq_num;
    uint64_t first_ack_time_dominant;
    uint64_t first_ack_time_submissive;
    uint64_t last_ack_time_dominant;
    uint64_t last_ack_time_submissive;
    uint64_t bytes_received_dominant;
    uint64_t bytes_received_submissive;
    struct st_picoquic_tonopah_interval_info_t* next;
} picoquic_tonopah_interval_info_t;

picoquic_tonopah_interval_info_t* interval_list = NULL;

static uint64_t set_path(picoquic_cnx_t * cnx, picoquic_path_t* actual_path, uint64_t cwin) {
    if (cnx->nb_paths == 2) {
        uint64_t current_smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
        uint64_t current_time = picoquic_current_time();

        if (last_change + current_smoothed_rtt < current_time) {
            if (dominant_path == path1) {
                // printf("Changing dominant path to path1, last_change: %lu, current_time: %lu, srtt: %lu, sum: %lu\n", last_change, current_time, current_smoothed_rtt, last_change + current_smoothed_rtt);
                dominant_path = path2;
            } else {
                // printf("Changing dominant path to path2, last_change: %lu, current_time: %lu, srtt: %lu, sum: %lu\n", last_change, current_time, current_smoothed_rtt, last_change + current_smoothed_rtt);
                dominant_path = path1;
            }
            last_change = current_time;
        }
        if (actual_path == dominant_path) {
            cwin = MAX(cwin * ratio, PICOQUIC_CWIN_MINIMUM);
            updated_path0 += 1;
            // printf("Updating path0 %lu\n", cwin);
        } else {
            cwin = MAX(cwin * (1-ratio), PICOQUIC_CWIN_MINIMUM);
            updated_path1 += 1;
            // printf("Updating path1 %lu\n", cwin);
        } 
        // else {
        //     puts("Two paths but path I got is neither of the first two");
        //     abort();
        // }
    }
    return cwin;
}

/*
 * Properly implementing New Reno requires managing a number of
 * signals, such as packet losses or acknowledgements. We attempt
 * to condensate all that in a single API, which could be shared
 * by many different congestion control algorithms.
 */
static void picoquic_tonopah_notify(
    picoquic_cnx_t * cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t rtt_measurement,
    uint64_t one_way_delay,
    uint64_t nb_bytes_acknowledged,
    uint64_t lost_packet_number,
    uint64_t current_time)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(lost_packet_number);
#endif

    assert(cnx->nb_paths <= 2);

    if (path1 == NULL) {
        path1 = path_x;
        // printf("assigning path1: %lu\n", (uint64_t) path1);
    } 
    else if (path_x != path1 && path2 == NULL) {
        path2 = path_x;
        // printf("assigning path2: %lu\n", (uint64_t) path2);
        last_change = picoquic_current_time();
    }
    if (!(path_x == path1 || path_x == path2)) {
        printf("unknown path: %lu\n", (uint64_t) path_x);
        abort();
    }
    assert(path_x == path1 || path_x == path2);
    int current_path_id = path_x == path1 ? 0 : 1;
    
    picoquic_path_t* actual_path = path_x;
    path_x = cnx->path[0];
    picoquic_tonopah_state_t* nr_state = (picoquic_tonopah_state_t*)path_x->congestion_alg_state;

    if (dominant_path == NULL) {
        dominant_path = path_x;
        uint64_t last_change = picoquic_current_time();
    }

    // TODO: Insert code that checks which interval a packet is from and update the interval_info
    picoquic_packet_context_t* pkt_ctx = &cnx->pkt_ctx[picoquic_packet_context_application];

    actual_path->is_cc_data_updated = 1;

    if (nr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_acknowledgement:
            if (path_x->last_time_acked_data_frame_sent > path_x->last_sender_limited_time) {
                // printf("current_path: %d, seq_num: %lu, ack_num: %lu\n", current_path_id, picoquic_cc_get_sequence_number(cnx, actual_path), picoquic_cc_get_ack_number(cnx, actual_path));
                // interval_list = (picoquic_tonopah_interval_info_t*) malloc(sizeof(picoquic_tonopah_interval_info_t));
                // memset(interval_list, 0, sizeof(interval_list));
                // interval_list->first_seq_num = picoquic_cc_get_sequence_number(cnx, actual_path);
                picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
                actual_path->cwin = set_path(cnx, actual_path, nr_state->nrss.cwin);
            }
            break;
        case picoquic_congestion_notification_seed_cwin:
        case picoquic_congestion_notification_ecn_ec:
        case picoquic_congestion_notification_repeat:
        case picoquic_congestion_notification_timeout:
            picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            actual_path->cwin = set_path(cnx, actual_path, nr_state->nrss.cwin);
            break;
        case picoquic_congestion_notification_spurious_repeat:
            picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            actual_path->cwin = set_path(cnx, actual_path, nr_state->nrss.cwin);
            break;
        case picoquic_congestion_notification_rtt_measurement:
            /* Using RTT increases as signal to get out of initial slow start */
            if (nr_state->nrss.alg_state == picoquic_tonopah_alg_slow_start &&
                nr_state->nrss.ssthresh == UINT64_MAX){

                if (path_x->rtt_min > PICOQUIC_TARGET_RENO_RTT) {
                    uint64_t min_win;

                    if (path_x->rtt_min > PICOQUIC_TARGET_SATELLITE_RTT) {
                        min_win = (uint64_t)((double)PICOQUIC_CWIN_INITIAL * (double)PICOQUIC_TARGET_SATELLITE_RTT / (double)PICOQUIC_TARGET_RENO_RTT);
                    }
                    else {
                        /* Increase initial CWIN for long delay links. */
                        min_win = (uint64_t)((double)PICOQUIC_CWIN_INITIAL * (double)path_x->rtt_min / (double)PICOQUIC_TARGET_RENO_RTT);
                    }
                    if (min_win > nr_state->nrss.cwin) {
                        nr_state->nrss.cwin = min_win;
                        actual_path->cwin = set_path(cnx, actual_path, min_win);
                    }
                }

                if (picoquic_hystart_test(&nr_state->rtt_filter, (cnx->is_time_stamp_enabled) ? one_way_delay : rtt_measurement,
                    cnx->path[0]->pacing_packet_time_microsec, current_time,
                    cnx->is_time_stamp_enabled)) {
                    /* RTT increased too much, get out of slow start! */
                    nr_state->nrss.ssthresh = nr_state->nrss.cwin;
                    nr_state->nrss.alg_state = picoquic_tonopah_alg_congestion_avoidance;
                    actual_path->cwin = set_path(cnx, actual_path, nr_state->nrss.cwin);
                    path_x->is_ssthresh_initialized = 1;
                }
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            break;
        case picoquic_congestion_notification_bw_measurement:
            if (nr_state->nrss.alg_state == picoquic_tonopah_alg_slow_start &&
                nr_state->nrss.ssthresh == UINT64_MAX) {

                uint64_t smoothed_rtt = path_x->smoothed_rtt;
                if (cnx->nb_paths > 1) {
                    assert(cnx->nb_paths == 2);
                    smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
                }

                /* RTT measurements will happen after the bandwidth is estimated */
                uint64_t max_win = path_x->max_bandwidth_estimate * smoothed_rtt / 1000000;
                uint64_t min_win = max_win /= 2;
                if (nr_state->nrss.cwin < min_win) {
                    nr_state->nrss.cwin = min_win;
                    actual_path->cwin = set_path(cnx, actual_path, min_win);
                }
            }
            break;
        case picoquic_congestion_notification_reset:
            picoquic_tonopah_reset(nr_state, actual_path);
            break;
        default:
            /* ignore */
            break;
        }

        // /* Compute pacing data */
        // picoquic_update_pacing_data(cnx, path_x, nr_state->nrss.alg_state == picoquic_tonopah_alg_slow_start &&
        //     nr_state->nrss.ssthresh == UINT64_MAX);
        /* Compute pacing data */
        picoquic_update_pacing_data(cnx, actual_path, nr_state->nrss.alg_state == picoquic_tonopah_alg_slow_start &&
            nr_state->nrss.ssthresh == UINT64_MAX);
    }
}

/* Release the state of the congestion control algorithm */
static void picoquic_tonopah_delete(picoquic_path_t* path_x)
{
    // printf("updated_path0: %lu; updated_path1: %lu\n", updated_path0, updated_path1);
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Observe the state of congestion control */

void picoquic_tonopah_observe(picoquic_path_t* path_x, uint64_t* cc_state, uint64_t* cc_param)
{
    picoquic_tonopah_state_t* nr_state = (picoquic_tonopah_state_t*)path_x->congestion_alg_state;
    *cc_state = (uint64_t)nr_state->nrss.alg_state;
    *cc_param = (nr_state->nrss.ssthresh == UINT64_MAX) ? 0 : nr_state->nrss.ssthresh;
}

/* Definition record for the New Reno algorithm */

#define PICOQUIC_tonopah_ID "tonopah" /* NR88 */

picoquic_congestion_algorithm_t picoquic_tonopah_algorithm_struct = {
    PICOQUIC_tonopah_ID, PICOQUIC_CC_ALGO_NUMBER_TONOPAH,
    picoquic_tonopah_init,
    picoquic_tonopah_notify,
    picoquic_tonopah_delete,
    picoquic_tonopah_observe
};

picoquic_congestion_algorithm_t* picoquic_tonopah_algorithm = &picoquic_tonopah_algorithm_struct;
