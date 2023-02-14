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

typedef struct st_picoquic_new_tonopah_interval_info_t {
    uint64_t first_seq_num1;
    uint64_t first_seq_num2;
    uint64_t first_ack_time1;
    uint64_t first_ack_time2;
    uint64_t last_ack_time1;
    uint64_t last_ack_time2;
    uint64_t bytes_received1;
    uint64_t bytes_received2;
    double rtt_sum1;
    double rtt_sum2;
    uint64_t num_acks1;
    uint64_t num_acks2;
    uint64_t new_tonopah_dominant_path_id; // can be 1 or 2;
    uint8_t finished1;
    uint8_t finished2;
    uint8_t dont_use;
    
    struct st_picoquic_new_tonopah_interval_info_t* next;
    struct st_picoquic_new_tonopah_interval_info_t* prev;
} picoquic_new_tonopah_interval_info_t;

#define INTERVALS_REQUIRED 1
uint64_t new_tonopah_minimum_interval = 0;
uint64_t new_tonopah_minimum_congestion_avoidance_interval = 50000;
uint64_t new_tonopah_maximum_interval = 1000000;

picoquic_new_tonopah_interval_info_t* new_tonopah_interval_list_first = NULL;
picoquic_new_tonopah_interval_info_t* new_tonopah_interval_list_last = NULL;

uint64_t new_tonopah_updated_path0 = 0;
uint64_t new_tonopah_updated_new_tonopah_path1 = 0;

uint64_t new_tonopah_last_change = 0;

double new_tonopah_ratio = 2./3.;

picoquic_cnx_t * new_tonopah_last_cnx = NULL;
picoquic_path_t* new_tonopah_path1 = NULL;
picoquic_path_t* new_tonopah_path2 = NULL;
picoquic_path_t* new_tonopah_dominant_path = NULL;

size_t new_tonopah_get_interval_info_list_len(picoquic_new_tonopah_interval_info_t* list) {
    picoquic_new_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    while (current_elem != NULL) {
        current_elem = current_elem->next;
        len += 1;
    }
    return len;
}

void new_tonopah_delete_info_list() {
    puts("Tonopah: resetting intervals");
    picoquic_new_tonopah_interval_info_t* current_elem = new_tonopah_interval_list_first;
    while (current_elem != NULL) {
        picoquic_new_tonopah_interval_info_t* prev_current_elem = current_elem;
        current_elem = current_elem->next;
        free(prev_current_elem);
    }
    new_tonopah_interval_list_first = NULL;
    new_tonopah_interval_list_last = NULL;
}

size_t new_tonopah_new_tonopah_get_interval_info_list_len_back(picoquic_new_tonopah_interval_info_t* list) {
    picoquic_new_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    while (current_elem != NULL) {
        current_elem = current_elem->prev;
        len += 1;
    }
    return len;
}

void picoquic_new_tonopah_sim_reset(picoquic_new_tonopah_sim_state_t * nrss)
{
    /* Initialize the state of the congestion control algorithm */
    memset(nrss, 0, sizeof(picoquic_new_tonopah_sim_state_t));
    nrss->alg_state = picoquic_new_tonopah_alg_slow_start;
    nrss->ssthresh = UINT64_MAX;
    nrss->cwin = PICOQUIC_CWIN_INITIAL;
}

/* The recovery state last 1 RTT, during which parameters will be frozen
 */
static void picoquic_new_tonopah_sim_enter_recovery(
    picoquic_new_tonopah_sim_state_t* nr_state,
    picoquic_cnx_t* cnx,
    picoquic_path_t * path_x,
    picoquic_congestion_notification_t notification,
    uint64_t current_time)
{
    printf("In recovery, state: %d\n", nr_state->alg_state);
    if (nr_state->alg_state == picoquic_new_tonopah_alg_congestion_avoidance && new_tonopah_get_interval_info_list_len(new_tonopah_interval_list_first) == 0) {
        puts("Tonopah: Packet lost but ignoring it");
        return;
    }
    if (new_tonopah_path1 != NULL && new_tonopah_path2 != NULL) {
        printf("Tonopah: Recovery at %lu: ", picoquic_current_time());
        new_tonopah_delete_info_list();
    }
    nr_state->ssthresh = nr_state->cwin / 2;
    if (nr_state->ssthresh < PICOQUIC_CWIN_MINIMUM) {
        nr_state->ssthresh = PICOQUIC_CWIN_MINIMUM;
    }

    if (notification == picoquic_congestion_notification_timeout) {
        nr_state->cwin = PICOQUIC_CWIN_MINIMUM;
        nr_state->alg_state = picoquic_new_tonopah_alg_slow_start;
    }
    else {
        nr_state->cwin = nr_state->ssthresh;
        nr_state->alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
    }

    nr_state->recovery_start = current_time;
    nr_state->recovery_sequence = picoquic_cc_get_sequence_number(cnx, path_x);
    nr_state->residual_ack = 0;
}

/* Update cwin per signaled bandwidth
 */
static void picoquic_new_tonopah_sim_seed_cwin(picoquic_new_tonopah_sim_state_t* nr_state,
    picoquic_path_t* path_x, uint64_t bytes_in_flight)
{
    if (nr_state->alg_state == picoquic_new_tonopah_alg_slow_start &&
        nr_state->ssthresh == UINT64_MAX) {
        if (bytes_in_flight > nr_state->cwin) {
            nr_state->cwin = bytes_in_flight;
            nr_state->ssthresh = bytes_in_flight;
            nr_state->alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
        }
    }
}


/* Notification API for new Reno simulations.
 */
void picoquic_new_tonopah_sim_notify(
    picoquic_new_tonopah_sim_state_t* nr_state,
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t nb_bytes_acknowledged,
    uint64_t current_time)
{
    uint64_t smoothed_rtt = path_x->smoothed_rtt;
    if (cnx->nb_paths > 1) {
        if (cnx->nb_paths != 2) {
            puts("Oh no, more paths than expected!");
        }
        assert(cnx->nb_paths == 2);
        smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
    }
    switch (notification) {
    case picoquic_congestion_notification_acknowledgement: {
        switch (nr_state->alg_state) {
        case picoquic_new_tonopah_alg_slow_start:
            nr_state->cwin += nb_bytes_acknowledged;
            /* if cnx->cwin exceeds SSTHRESH, exit and go to CA */
            if (nr_state->cwin >= nr_state->ssthresh) {
                nr_state->alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
            }
            break;
        case picoquic_new_tonopah_alg_congestion_avoidance:
        default: {
            uint64_t complete_delta = nb_bytes_acknowledged * path_x->send_mtu + nr_state->residual_ack;
            nr_state->residual_ack = complete_delta % nr_state->cwin;
            uint64_t current_smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
            double ratio = MIN((((double) current_smoothed_rtt) / ((double) new_tonopah_minimum_congestion_avoidance_interval)), 1.0);
            // printf("ratio: %f\n", ratio);
            nr_state->cwin += ratio * (((double) complete_delta) / ((double) nr_state->cwin));
            // nr_state->cwin += complete_delta / nr_state->cwin;
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
                picoquic_new_tonopah_sim_enter_recovery(nr_state, cnx, path_x, notification, current_time);
            }
        }
        else {
            if (current_time - nr_state->recovery_start > smoothed_rtt ||
                nr_state->recovery_start <= picoquic_cc_get_ack_sent_time(cnx, path_x)) {
                picoquic_new_tonopah_sim_enter_recovery(nr_state, cnx, path_x, notification, current_time);
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
                    nr_state->alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
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
                    nr_state->alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
                }
            }
        }
        break;
    case picoquic_congestion_notification_bw_measurement:
        break;
    case picoquic_congestion_notification_reset:
        picoquic_new_tonopah_sim_reset(nr_state);
        break;
    case picoquic_congestion_notification_seed_cwin:
        picoquic_new_tonopah_sim_seed_cwin(nr_state, path_x, nb_bytes_acknowledged);
        break;
    default:
        /* ignore */
        break;
    }
}


/* Actual implementation of New Reno, when used as a stand alone algorithm
 */

typedef struct st_picoquic_new_tonopah_state_t {
    picoquic_new_tonopah_sim_state_t nrss;
    picoquic_min_max_rtt_t rtt_filter;
} picoquic_new_tonopah_state_t;

static void picoquic_new_tonopah_reset(picoquic_new_tonopah_state_t* nr_state, picoquic_path_t* path_x)
{
    memset(nr_state, 0, sizeof(picoquic_new_tonopah_state_t));
    picoquic_new_tonopah_sim_reset(&nr_state->nrss);
    path_x->cwin = nr_state->nrss.cwin;
}

static void picoquic_new_tonopah_init(picoquic_path_t* path_x, uint64_t current_time)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_new_tonopah_state_t* nr_state = (picoquic_new_tonopah_state_t*)malloc(sizeof(picoquic_new_tonopah_state_t));
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(current_time);
#endif

    puts("Initializing new_tonopah");
    if (nr_state != NULL) {
        picoquic_new_tonopah_reset(nr_state, path_x);
        path_x->congestion_alg_state = nr_state;
    }
    else {
        path_x->congestion_alg_state = NULL;
    }
}

int new_tonopah_aggregate_intervals(picoquic_new_tonopah_interval_info_t* list) {
    picoquic_new_tonopah_interval_info_t* current_elem = list;
    size_t len = 0;
    uint64_t acc_time_diffs_dominant = 0;
    uint64_t acc_time_diffs_submissive = 0;
    uint64_t acc_bytes_dominant = 0;
    uint64_t acc_bytes_submissive = 0;
    uint64_t sent_bytes_dominant = 0;
    uint64_t sent_bytes_submissive = 0;
    double acc_rtt_sum_dominant = 0;
    double acc_rtt_sum_submissive = 0;
    uint64_t acc_num_acks_dominant = 0;
    uint64_t acc_num_acks_submissive = 0;

    while (current_elem != NULL) {
        if (current_elem->finished1 && current_elem->finished2) {
            len += 1;
            assert(current_elem->new_tonopah_dominant_path_id == 1 || current_elem->new_tonopah_dominant_path_id == 2);
            if (current_elem->new_tonopah_dominant_path_id == 1) {
                acc_time_diffs_dominant += (current_elem->last_ack_time1 - current_elem->first_ack_time1);
                acc_time_diffs_submissive += (current_elem->last_ack_time2 - current_elem->first_ack_time2);
                acc_bytes_dominant += current_elem->bytes_received1;
                acc_bytes_submissive += current_elem->bytes_received2;
                sent_bytes_dominant += current_elem->next->first_seq_num1 - current_elem->first_seq_num1;
                sent_bytes_submissive += current_elem->next->first_seq_num2 - current_elem->first_seq_num2;
                acc_rtt_sum_dominant += current_elem->rtt_sum1;
                acc_rtt_sum_submissive += current_elem->rtt_sum2;
                acc_num_acks_dominant += current_elem->num_acks1;
                acc_num_acks_submissive += current_elem->num_acks2;
            } else {
                acc_time_diffs_dominant += (current_elem->last_ack_time2 - current_elem->first_ack_time2);
                acc_time_diffs_submissive += (current_elem->last_ack_time1 - current_elem->first_ack_time1);
                acc_bytes_dominant += current_elem->bytes_received2;
                acc_bytes_submissive += current_elem->bytes_received1;
                sent_bytes_dominant += current_elem->next->first_seq_num2 - current_elem->first_seq_num2;
                sent_bytes_submissive += current_elem->next->first_seq_num1 - current_elem->first_seq_num1;
                acc_rtt_sum_dominant += current_elem->rtt_sum2;
                acc_rtt_sum_submissive += current_elem->rtt_sum1;
                acc_num_acks_dominant += current_elem->num_acks2;
                acc_num_acks_submissive += current_elem->num_acks1;
            }
        } else {
            acc_time_diffs_dominant = 0;
            acc_time_diffs_submissive = 0;
            acc_bytes_dominant = 0;
            acc_bytes_submissive = 0;
            sent_bytes_dominant = 0;
            sent_bytes_submissive = 0;
            acc_rtt_sum_dominant = 0;
            acc_rtt_sum_submissive = 0;
            acc_num_acks_dominant = 0;
            acc_num_acks_submissive = 0;
            len = 0;
        }
        if (len == INTERVALS_REQUIRED) {
            double mean_rtt_dominant = ((double) (acc_rtt_sum_dominant)) / acc_num_acks_dominant;
            double mean_rtt_submissive = ((double) (acc_rtt_sum_submissive)) / acc_num_acks_submissive;
            double observed_new_tonopah_diff = mean_rtt_dominant - mean_rtt_submissive;

            int detected_fq = observed_new_tonopah_diff > 5000;

            // printf("detected_fq: %d, mean_rtt_dominant: %.2f, mean_rtt_submissive: %.2f, observed_new_tonopah_diff: %.2f\n", detected_fq, mean_rtt_dominant, mean_rtt_submissive, observed_new_tonopah_diff);  
            return detected_fq;               
        }
        current_elem = current_elem->prev;
    }
    return 0;
}

static void new_tonopah_set_path(picoquic_cnx_t* cnx, picoquic_new_tonopah_sim_state_t* nr_state, uint64_t cwin) {
    
    if (cnx->nb_paths == 2 && new_tonopah_path1 != NULL && new_tonopah_path2 != NULL) {
        uint64_t current_time = picoquic_current_time();

        uint64_t current_smoothed_rtt = (cnx->path[0]->smoothed_rtt + cnx->path[1]->smoothed_rtt)/2;
        if (new_tonopah_last_change + MIN(MAX(new_tonopah_minimum_interval, current_smoothed_rtt), new_tonopah_maximum_interval) < current_time) {
            // if (new_tonopah_dominant_path == new_tonopah_path1) {
            //     new_tonopah_dominant_path = new_tonopah_path2;
            // } else {
            //     new_tonopah_dominant_path = new_tonopah_path1;
            // }
            int detected_fq = new_tonopah_aggregate_intervals(new_tonopah_interval_list_last);
            if (detected_fq && nr_state->alg_state == picoquic_new_tonopah_alg_congestion_avoidance) {
                nr_state->ssthresh = (uint64_t) (((double) nr_state->cwin) * (7./8.));
                nr_state->cwin = nr_state->ssthresh;
                printf("Tonopah: FQ detected at %lu: ", picoquic_current_time());
                new_tonopah_delete_info_list();
            }
            if (nr_state->alg_state != picoquic_new_tonopah_alg_congestion_avoidance) {
                printf("Not in congestion avoidance: %d; ", nr_state->alg_state);
                new_tonopah_delete_info_list();
            }
            picoquic_new_tonopah_interval_info_t* new_interval = (picoquic_new_tonopah_interval_info_t*) malloc(sizeof(picoquic_new_tonopah_interval_info_t));
            memset(new_interval, 0, sizeof(*new_interval));
            new_interval->next = NULL; // Probably unnecessary since it's already set to 0
            new_interval->new_tonopah_dominant_path_id = new_tonopah_dominant_path == new_tonopah_path1 ? 1 : 2;
            new_interval->first_seq_num1 = picoquic_cc_get_sequence_number(cnx, new_tonopah_path1);
            new_interval->first_seq_num2 = picoquic_cc_get_sequence_number(cnx, new_tonopah_path2);
            picoquic_new_tonopah_interval_info_t* prev_element = new_tonopah_interval_list_last;
            new_interval->prev = prev_element;
            if (new_tonopah_interval_list_first == NULL) {
                new_tonopah_interval_list_first = new_interval;
                new_interval->dont_use = 1;
            }
            new_tonopah_interval_list_last = new_interval;
            if (prev_element != NULL) {
                prev_element->next = new_interval;
            }
            size_t interval_len = new_tonopah_get_interval_info_list_len(new_tonopah_interval_list_first);
            assert(interval_len <= 4*INTERVALS_REQUIRED+1);
            if (interval_len > 4*INTERVALS_REQUIRED) {
                picoquic_new_tonopah_interval_info_t* to_delete = new_tonopah_interval_list_first;
                new_tonopah_interval_list_first = new_tonopah_interval_list_first->next;
                new_tonopah_interval_list_first->prev = NULL;
                free(to_delete);
            }
            // interval_len = new_tonopah_get_interval_info_list_len(new_tonopah_interval_list_first);
            // printf("New interval length: %lu\n", interval_len);
            new_tonopah_last_change = current_time;
        }
        uint64_t dominant_cwin = MAX(cwin * new_tonopah_ratio, PICOQUIC_CWIN_MINIMUM);
        uint64_t submissive_cwin = MAX(cwin * (1-new_tonopah_ratio), PICOQUIC_CWIN_MINIMUM);

        if (new_tonopah_dominant_path == new_tonopah_path1) {
            // puts("First one dominant");
            cnx->path[0]->cwin = dominant_cwin;
            cnx->path[1]->cwin = submissive_cwin;
        } else if (new_tonopah_dominant_path == new_tonopah_path2) {
            // puts("Second one domimant");
            cnx->path[1]->cwin = dominant_cwin;
            cnx->path[0]->cwin = submissive_cwin;
        }
    }
}

picoquic_new_tonopah_interval_info_t* new_tonopah_find_right_interval(picoquic_cnx_t* cnx, picoquic_path_t* path) {
    uint64_t ack_num = picoquic_cc_get_ack_number(cnx, path);
    int path_id = path == new_tonopah_path1 ? 1 : 2;
    assert(path == new_tonopah_path1 || path == new_tonopah_path2);
    picoquic_new_tonopah_interval_info_t* current_elem = new_tonopah_interval_list_last;
    while (current_elem != NULL) {
        if ((path_id == 1 && ack_num >= current_elem->first_seq_num1) || 
             (path_id == 2 && ack_num >= current_elem->first_seq_num2)) {
            if (current_elem->prev != NULL) {
                if (path_id == 1 && !current_elem->prev->finished1) {
                    current_elem->prev->finished1 = 1;
                    // if ((current_elem->prev->first_ack_time1 == 0)
                    // || (current_elem->prev->last_ack_time1 == 0)
                    // || (current_elem->prev->bytes_received1 == 0)
                    // || (current_elem->prev->first_seq_num1 == 0)
                    // ) {
                    //     current_elem->prev->finished1 = 0;
                    // }
                } else if (path_id == 2 && !current_elem->prev->finished2) {
                    current_elem->prev->finished2 = 1;
                    // if ((current_elem->prev->first_ack_time2 == 0)
                    // || (current_elem->prev->last_ack_time2 == 0)
                    // || (current_elem->prev->bytes_received2 == 0)
                    // || (current_elem->prev->first_seq_num2 == 0)
                    // ) {
                    //     current_elem->prev->finished2 = 0;
                    // }
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
static void picoquic_new_tonopah_notify(
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

    new_tonopah_last_cnx = cnx;

    assert(cnx->nb_paths <= 2);

    if (new_tonopah_path1 == NULL) {
        new_tonopah_path1 = path_x;
    } 
    else if (path_x != new_tonopah_path1 && new_tonopah_path2 == NULL) {
        new_tonopah_path2 = path_x;
    }
    if (!(path_x == new_tonopah_path1 || path_x == new_tonopah_path2)) {
        printf("unknown path: %lu\n", (uint64_t) path_x);
        abort();
    }
    assert(path_x == new_tonopah_path1 || path_x == new_tonopah_path2);
    
    picoquic_path_t* actual_path = path_x;
    path_x = cnx->path[0];
    picoquic_new_tonopah_state_t* nr_state = (picoquic_new_tonopah_state_t*)path_x->congestion_alg_state;
    uint64_t t = picoquic_current_time();

    if (new_tonopah_dominant_path == NULL) {
        new_tonopah_dominant_path = path_x;
        new_tonopah_last_change = t;
    }

    actual_path->is_cc_data_updated = 1;

    if (nr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_acknowledgement:
            if (actual_path->last_time_acked_data_frame_sent > actual_path->last_sender_limited_time) {
                picoquic_new_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
                int current_path = actual_path == new_tonopah_path1 ? 1 : 2;
                picoquic_new_tonopah_interval_info_t* right_interval = new_tonopah_find_right_interval(cnx, actual_path);
                if (right_interval != NULL) {
                    // printf("rtt_measurement: %lu\n", rtt_measurement);
                    if (current_path == 1) {
                        right_interval->bytes_received1 += nb_bytes_acknowledged;
                        right_interval->rtt_sum1 += actual_path->smoothed_rtt;
                        right_interval->num_acks1 += 1;
                        if (right_interval->first_ack_time1 == 0) right_interval->first_ack_time1 = t;
                        right_interval->last_ack_time1 = t;
                    } else {
                        right_interval->bytes_received2 += nb_bytes_acknowledged;
                        right_interval->rtt_sum2 += actual_path->smoothed_rtt;
                        right_interval->num_acks2 += 1;
                        if (right_interval->first_ack_time2 == 0) right_interval->first_ack_time2 = t;
                        right_interval->last_ack_time2 = t;
                    }
                }
                new_tonopah_set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
            }
            break;
        case picoquic_congestion_notification_seed_cwin:
        case picoquic_congestion_notification_ecn_ec:
            if (notification == picoquic_congestion_notification_ecn_ec) {
                if (new_tonopah_dominant_path != actual_path) {
                    // puts("ce on submissive path; acting");
                } else {
                    // puts("ce on dominant path; ignoring");
                    break;
                }
            }
        case picoquic_congestion_notification_repeat:
            // if (notification == picoquic_congestion_notification_repeat) {
            //     if (new_tonopah_dominant_path != actual_path) {
            //         // puts("ce on submissive path; acting");
            //     } else {
            //         // puts("ce on dominant path; ignoring");
            //         break;
            //     }
            // }
        case picoquic_congestion_notification_timeout:
            picoquic_new_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            new_tonopah_set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
            break;
        case picoquic_congestion_notification_spurious_repeat:
            picoquic_new_tonopah_sim_notify(&nr_state->nrss, cnx, path_x, notification, nb_bytes_acknowledged, current_time);
            new_tonopah_set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
            break;
        case picoquic_congestion_notification_rtt_measurement:
            /* Using RTT increases as signal to get out of initial slow start */
            if (nr_state->nrss.alg_state == picoquic_new_tonopah_alg_slow_start &&
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
                        new_tonopah_set_path(cnx, &(nr_state->nrss), min_win);
                    }
                }

                if (picoquic_hystart_test(&nr_state->rtt_filter, (cnx->is_time_stamp_enabled) ? one_way_delay : rtt_measurement,
                    cnx->path[0]->pacing_packet_time_microsec, current_time,
                    cnx->is_time_stamp_enabled)) {
                    /* RTT increased too much, get out of slow start! */
                    nr_state->nrss.ssthresh = nr_state->nrss.cwin;
                    nr_state->nrss.alg_state = picoquic_new_tonopah_alg_congestion_avoidance;
                    new_tonopah_set_path(cnx, &(nr_state->nrss), nr_state->nrss.cwin);
                    path_x->is_ssthresh_initialized = 1;
                }
            }
            break;
        case picoquic_congestion_notification_cwin_blocked:
            break;
        case picoquic_congestion_notification_bw_measurement:
            if (nr_state->nrss.alg_state == picoquic_new_tonopah_alg_slow_start &&
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
                    new_tonopah_set_path(cnx, &(nr_state->nrss), min_win);
                }
            }
            break;
        case picoquic_congestion_notification_reset:
            picoquic_new_tonopah_reset(nr_state, actual_path);
            break;
        default:
            /* ignore */
            break;
        }

        // /* Compute pacing data */
        // picoquic_update_pacing_data(cnx, path_x, nr_state->nrss.alg_state == picoquic_new_tonopah_alg_slow_start &&
        //     nr_state->nrss.ssthresh == UINT64_MAX);
        /* Compute pacing data */
        picoquic_update_pacing_data(cnx, actual_path, nr_state->nrss.alg_state == picoquic_new_tonopah_alg_slow_start &&
            nr_state->nrss.ssthresh == UINT64_MAX);
    }
}

uint8_t deleted_paths = 0;

/* Release the state of the congestion control algorithm */
static void picoquic_new_tonopah_delete(picoquic_path_t* path_x)
{
    if (new_tonopah_last_cnx != NULL) {
        in_port_t s_port = ((struct sockaddr_in*) &(new_tonopah_last_cnx->path[0]->local_addr))->sin_port;
        printf("Tonopah: Ending at %lu\n", picoquic_current_time());
        if (new_tonopah_last_cnx->nb_paths > 1) {
            printf("src_port: %hu, selected1: %d; congested1: %d, paced1: %d, selected2: %d, congested2: %d, paced2: %d\n", s_port, 
                new_tonopah_last_cnx->path[0]->selected, new_tonopah_last_cnx->path[0]->congested, new_tonopah_last_cnx->path[0]->paced, 
                new_tonopah_last_cnx->path[1]->selected, new_tonopah_last_cnx->path[1]->congested, new_tonopah_last_cnx->path[1]->paced);
        } else {
            // puts("Ending but couldn't output debug metrics as other flow already is gone.");
        }
    }
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
        deleted_paths += 1;
    }
    if (deleted_paths >= 2) {
        puts("Tonopah doesn't support several consecutive connections at the moment, exiting");
        exit(0);
    }
}

/* Observe the state of congestion control */

void picoquic_new_tonopah_observe(picoquic_path_t* path_x, uint64_t* cc_state, uint64_t* cc_param)
{
    picoquic_new_tonopah_state_t* nr_state = (picoquic_new_tonopah_state_t*)path_x->congestion_alg_state;
    *cc_state = (uint64_t)nr_state->nrss.alg_state;
    *cc_param = (nr_state->nrss.ssthresh == UINT64_MAX) ? 0 : nr_state->nrss.ssthresh;
}

/* Definition record for the New Reno algorithm */

#define PICOQUIC_new_tonopah_ID "new_tonopah" /* NR88 */

picoquic_congestion_algorithm_t picoquic_new_tonopah_algorithm_struct = {
    PICOQUIC_new_tonopah_ID, PICOQUIC_CC_ALGO_NUMBER_NEW_TONOPAH,
    picoquic_new_tonopah_init,
    picoquic_new_tonopah_notify,
    picoquic_new_tonopah_delete,
    picoquic_new_tonopah_observe
};

picoquic_congestion_algorithm_t* picoquic_new_tonopah_algorithm = &picoquic_new_tonopah_algorithm_struct;
