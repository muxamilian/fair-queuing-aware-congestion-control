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

typedef struct st_picoquic_tonopah_interval_info_t {
    uint64_t first_seq_num1;
    uint64_t first_seq_num2;
    uint64_t first_ack_time1;
    uint64_t first_ack_time2;
    uint64_t last_ack_time1;
    uint64_t last_ack_time2;
    uint64_t bytes_received1;
    uint64_t bytes_received2;
    uint64_t dominant_path_id; // can be 1 or 2;
    uint8_t finished1;
    uint8_t finished2;
    
    struct st_picoquic_tonopah_interval_info_t* next;
    struct st_picoquic_tonopah_interval_info_t* prev;
} picoquic_tonopah_interval_info_t;

#define INTERVALS_REQUIRED 10

picoquic_tonopah_interval_info_t* interval_list_first = NULL;
picoquic_tonopah_interval_info_t* interval_list_last = NULL;

size_t get_interval_info_list_len(picoquic_tonopah_interval_info_t* list) {
    picoquic_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    while (current_elem != NULL) {
        current_elem = current_elem->next;
        len += 1;
    }
    return len;
}

void delete_info_list() {
    puts("Tonopah: resetting intervals");
    picoquic_tonopah_interval_info_t* current_elem = interval_list_first;
    while (current_elem != NULL) {
        picoquic_tonopah_interval_info_t* prev_current_elem = current_elem;
        current_elem = current_elem->next;
        free(prev_current_elem);
    }
    interval_list_first = NULL;
    interval_list_last = NULL;
}

size_t get_interval_info_list_len_back(picoquic_tonopah_interval_info_t* list) {
    picoquic_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    while (current_elem != NULL) {
        current_elem = current_elem->prev;
        len += 1;
    }
    return len;
}

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
    delete_info_list();
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

double ratio = 0.625;

picoquic_path_t* path1 = NULL;
picoquic_path_t* path2 = NULL;
picoquic_path_t* dominant_path = NULL;

int aggregate_intervals(picoquic_tonopah_interval_info_t* list) {
    picoquic_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    uint64_t acc_time_diffs_dominant = 0;
    uint64_t acc_time_diffs_submissive = 0;
    uint64_t acc_bytes_dominant = 0;
    uint64_t acc_bytes_submissive = 0;
    while (current_elem != NULL) {
        if (current_elem->finished1 && current_elem->finished2) {
            len += 1;
            assert(current_elem->dominant_path_id == 1 || current_elem->dominant_path_id == 2);
            if (current_elem->dominant_path_id == 1) {
                acc_time_diffs_dominant += (current_elem->last_ack_time1 - current_elem->first_ack_time1);
                acc_time_diffs_submissive += (current_elem->last_ack_time2 - current_elem->first_ack_time2);
                acc_bytes_dominant += current_elem->bytes_received1;
                acc_bytes_submissive += current_elem->bytes_received2;
            } else {
                acc_time_diffs_dominant += (current_elem->last_ack_time2 - current_elem->first_ack_time2);
                acc_time_diffs_submissive += (current_elem->last_ack_time1 - current_elem->first_ack_time1);
                acc_bytes_dominant += current_elem->bytes_received2;
                acc_bytes_submissive += current_elem->bytes_received1;
            }
        } else {
            acc_time_diffs_dominant = 0;
            acc_time_diffs_submissive = 0;
            acc_bytes_dominant = 0;
            acc_bytes_submissive = 0;
            len = 0;
        }
        if (len == INTERVALS_REQUIRED) {
            double bw_dominant = ((double) (acc_bytes_dominant*8)) / acc_time_diffs_dominant * 1000000. / 1000000;
            double bw_submissive = ((double) (acc_bytes_submissive*8)) / acc_time_diffs_submissive * 1000000. / 1000000;
            double observed_ratio = bw_dominant / (bw_dominant + bw_submissive);
            int detected_fq = observed_ratio < ((0.5 + ratio)/2.);
            printf("detected_fq: %d, bw_dominant: %.2f, bw_submissive: %.2f, ratio: %.3f\n", detected_fq, bw_dominant, bw_submissive, observed_ratio);  
            return detected_fq;               
        }
        current_elem = current_elem->prev;
    }
    return 0;
}

static void set_path(picoquic_cnx_t* cnx, picoquic_tonopah_sim_state_t* nr_state, uint64_t cwin) {
    
    double ratio_used = ratio;
    // if (((struct sockaddr_in*) &(cnx->path[0]->local_addr))->sin_port != 4433) {
    //     ratio_used = 0.5;
    // }

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
            // // printf("current_path: %d, seq_num: %lu, ack_num: %lu\n", current_path_id, picoquic_cc_get_sequence_number(cnx, actual_path), picoquic_cc_get_ack_number(cnx, actual_path));
            // interval_list = (picoquic_tonopah_interval_info_t*) malloc(sizeof(picoquic_tonopah_interval_info_t));
            // memset(interval_list, 0, sizeof(interval_list));
            // interval_list->first_seq_num = picoquic_cc_get_sequence_number(cnx, actual_path);
            int detected_fq = aggregate_intervals(interval_list_last);
            if (detected_fq) {
                // puts("Detected fq, lowering cw");
                nr_state->ssthresh = (uint64_t) (((double) nr_state->cwin) * (7./8.));
                nr_state->cwin = nr_state->ssthresh;
                delete_info_list();
            }
            picoquic_tonopah_interval_info_t* new_interval = (picoquic_tonopah_interval_info_t*) malloc(sizeof(picoquic_tonopah_interval_info_t));
            memset(new_interval, 0, sizeof(*new_interval));
            new_interval->next = NULL; // Probably unnecessary since it's already set to 0
            new_interval->dominant_path_id = dominant_path == path1 ? 1 : 2;
            new_interval->first_seq_num1 = picoquic_cc_get_sequence_number(cnx, path1);
            new_interval->first_seq_num2 = picoquic_cc_get_sequence_number(cnx, path2);
            picoquic_tonopah_interval_info_t* prev_element = interval_list_last;
            new_interval->prev = prev_element;
            if (interval_list_first == NULL) {
                interval_list_first = new_interval;
            }
            interval_list_last = new_interval;
            if (prev_element != NULL) {
                prev_element->next = new_interval;
            }
            size_t interval_len = get_interval_info_list_len(interval_list_first);
            assert(interval_len <= 2*INTERVALS_REQUIRED+1);
            if (interval_len > 2*INTERVALS_REQUIRED) {
                picoquic_tonopah_interval_info_t* to_delete = interval_list_first;
                interval_list_first = interval_list_first->next;
                interval_list_first->prev = NULL;
                free(to_delete);
            }
            interval_len = get_interval_info_list_len(interval_list_first);
            // size_t interval_len_from_back = get_interval_info_list_len_back(interval_list_last);
            // printf("Intervals: %lu, %lu\n", interval_len, interval_len_from_back);
            last_change = current_time;
        }
        uint64_t dominant_cwin = MAX(cwin * ratio_used, PICOQUIC_CWIN_MINIMUM);
        uint64_t submissive_cwin = MAX(cwin * (1-ratio_used), PICOQUIC_CWIN_MINIMUM);

        if (dominant_path == path1) {
            cnx->path[0]->cwin = dominant_cwin;
            cnx->path[1]->cwin = submissive_cwin;
        } else if (dominant_path == path2) {
            cnx->path[1]->cwin = dominant_cwin;
            cnx->path[0]->cwin = submissive_cwin;
        }

        // else {
        //     puts("Two paths but path I got is neither of the first two");
        //     abort();
        // }
    }
}

picoquic_tonopah_interval_info_t* find_right_interval(picoquic_cnx_t* cnx, picoquic_path_t* path) {
    uint64_t ack_num = picoquic_cc_get_ack_number(cnx, path);
    int path_id = path == path1 ? 1 : 2;
    assert(path == path1 || path == path2);
    picoquic_tonopah_interval_info_t* current_elem = interval_list_last;
    while (current_elem != NULL) {
        if ((path_id == 1 && ack_num >= current_elem->first_seq_num1) || 
             (path_id == 2 && ack_num >= current_elem->first_seq_num2)) {
            if (current_elem->prev != NULL) {
                if (path_id == 1 && !current_elem->prev->finished1) {
                    current_elem->prev->finished1 = 1;
                    assert(current_elem->prev->first_ack_time1 != 0);
                    assert(current_elem->prev->last_ack_time1 != 0);
                    assert(current_elem->prev->bytes_received1 != 0);
                    assert(current_elem->prev->first_seq_num1 != 0);
                    // double bw = ((double) (current_elem->prev->bytes_received1*8)) / (current_elem->prev->last_ack_time1 - current_elem->prev->first_ack_time1) * 1000000.;
                    // printf("Finished interval1, bw: %f\n", bw);                    
                } else if (path_id == 2 && !current_elem->prev->finished2) {
                    current_elem->prev->finished2 = 1;
                    assert(current_elem->prev->first_ack_time2 != 0);
                    assert(current_elem->prev->last_ack_time2 != 0);
                    assert(current_elem->prev->bytes_received2 != 0);
                    assert(current_elem->prev->first_seq_num2 != 0);
                    // double bw = ((double) (current_elem->prev->bytes_received2*8)) / (current_elem->prev->last_ack_time2 - current_elem->prev->first_ack_time2) * 1000000.;
                    // printf("Finished interval2, bw: %f\n", bw);
                }
            }
            return current_elem;
        }
        current_elem = current_elem->prev;
    }
    return NULL;
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
        // last_change = picoquic_current_time();
    }
    if (!(path_x == path1 || path_x == path2)) {
        printf("unknown path: %lu\n", (uint64_t) path_x);
        abort();
    }
    assert(path_x == path1 || path_x == path2);
    
    picoquic_path_t* actual_path = path_x;
    path_x = cnx->path[0];
    picoquic_tonopah_state_t* nr_state = (picoquic_tonopah_state_t*)path_x->congestion_alg_state;
    uint64_t t = picoquic_current_time();

    if (dominant_path == NULL) {
        dominant_path = path_x;
        last_change = t;
        // last_change = 0;
    }

    actual_path->is_cc_data_updated = 1;

    if (nr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_acknowledgement:
            if (actual_path->last_time_acked_data_frame_sent > actual_path->last_sender_limited_time) {
                picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
                int current_path = actual_path == path1 ? 1 : 2;
                picoquic_tonopah_interval_info_t* right_interval = find_right_interval(cnx, actual_path);
                // assert(right_interval != NULL);
                if (right_interval != NULL) {
                    if (current_path == 1) {
                        right_interval->bytes_received1 += nb_bytes_acknowledged;
                        if (right_interval->first_ack_time1 == 0) right_interval->first_ack_time1 = t;
                        right_interval->last_ack_time1 = t;
                    } else {
                        right_interval->bytes_received2 += nb_bytes_acknowledged;
                        if (right_interval->first_ack_time2 == 0) right_interval->first_ack_time2 = t;
                        right_interval->last_ack_time2 = t;
                    }
                }
                set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
            }
            break;
        case picoquic_congestion_notification_seed_cwin:
        case picoquic_congestion_notification_ecn_ec:
        case picoquic_congestion_notification_repeat:
        case picoquic_congestion_notification_timeout:
            picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
            break;
        case picoquic_congestion_notification_spurious_repeat:
            picoquic_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
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
                        set_path(cnx, &(nr_state->nrss), min_win);
                    }
                }

                if (picoquic_hystart_test(&nr_state->rtt_filter, (cnx->is_time_stamp_enabled) ? one_way_delay : rtt_measurement,
                    cnx->path[0]->pacing_packet_time_microsec, current_time,
                    cnx->is_time_stamp_enabled)) {
                    /* RTT increased too much, get out of slow start! */
                    nr_state->nrss.ssthresh = nr_state->nrss.cwin;
                    nr_state->nrss.alg_state = picoquic_tonopah_alg_congestion_avoidance;
                    set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
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
                    set_path(cnx, &(nr_state->nrss), min_win);
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
