/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * (C) 2015 by Argonne National Laboratory.
 *     See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include "cspu.h"

/* Multi-objects lock (MLOCK) component on user processes */

#ifdef CSPU_MLOCK_DEBUG
#define CSPU_MLOCK_DBG_PRINT(str,...) do { \
    fprintf(stdout, "[CSPU-MLOCK][%d]"str, CSP_PROC.wrank, ## __VA_ARGS__); \
    fflush(stdout); \
    } while (0)
static const char *cwp_lock_status_name[CSP_MLOCK_STATUS_MAX] = {
    "unset",
    "suspended_l",
    "suspended_h",
    "acquired"
};
#else
#define CSPU_MLOCK_DBG_PRINT(str,...)
#endif

/* ======================================================================
 * Internal lock related functions.
 * ====================================================================== */

static inline int mlock_sync_status(CSP_mlock_status_t * lock_status)
{
    int mpi_errno = MPI_SUCCESS;
    CSP_cwp_pkt_t pkt;
    CSP_cwp_mlock_status_sync_pkt_t *locksync_pkt = &pkt.u.lock_status_sync;

    mpi_errno = PMPI_Recv((char *) &pkt, sizeof(CSP_cwp_pkt_t),
                          MPI_CHAR, CSP_PROC.user.g_lranks[0], CSP_CWP_TAG, CSP_PROC.local_comm,
                          MPI_STATUS_IGNORE);
    if (mpi_errno != MPI_SUCCESS)
        return mpi_errno;

    CSPU_MLOCK_DBG_PRINT(" \t sync LOCK STAT (%d -> %d[%s])\n", (*lock_status),
                         locksync_pkt->status, cwp_lock_status_name[locksync_pkt->status]);

    (*lock_status) = locksync_pkt->status;
    return mpi_errno;
}

static inline int mlock_issue_acquire_req(int group_id)
{
    CSP_cwp_pkt_t pkt;
    CSP_cwp_mlock_acquire_pkt_t *lockacq_pkt = &pkt.u.lock_acquire;

    pkt.cmd_type = CSP_MLOCK_ACQUIRE;
    lockacq_pkt->group_id = group_id;

    CSPU_MLOCK_DBG_PRINT(" \t send LOCK CMD %d (acquire, %d)\n", pkt.cmd_type, group_id);
    return CSPU_cwp_issue(&pkt);
}

static inline int mlock_issue_discard_req(int group_id)
{
    CSP_cwp_pkt_t pkt;
    CSP_cwp_mlock_discard_pkt_t *lockdcd_pkt = &pkt.u.lock_discard;

    pkt.cmd_type = CSP_MLOCK_DISCARD;
    lockdcd_pkt->group_id = group_id;

    CSPU_MLOCK_DBG_PRINT(" \t send LOCK CMD %d (discard, %d)\n", pkt.cmd_type, group_id);
    return CSPU_cwp_issue(&pkt);
}

static inline int mlock_issue_release_req(int group_id)
{
    CSP_cwp_pkt_t pkt;
    CSP_cwp_mlock_release_pkt_t *lockrls_pkt = &pkt.u.lock_release;

    pkt.cmd_type = CSP_MLOCK_RELEASE;
    lockrls_pkt->group_id = group_id;

    CSPU_MLOCK_DBG_PRINT(" \t send LOCK CMD %d (release, %d)\n", pkt.cmd_type, group_id);
    return CSPU_cwp_issue(&pkt);
}

/* Lock ghost processes for the next function command (blocking collective call).
 * It is collective call in user root communicator, blocking wait till all user
 * root processes have acquired the lock on their local ghost processes.
 * The lock is released automatically after function remotely finished.
 *
 * Note that this routine must be called after all user root arrived in function,
 * otherwise the group id may not be unique (i.e., two groups [0, 1] and [0, 3],
 * 0 and 3 start acquiring lock concurrently). */
int CSPU_mlock_acquire(MPI_Comm user_root_comm)
{
    int mpi_errno = MPI_SUCCESS;
    int group_id = 0, first_user_root_rank = 0;
    MPI_Group user_root_group = MPI_GROUP_NULL;
    int all_lock_acquired = 0;
    int user_root_rank = -1;

    CSP_mlock_status_t lock_status = CSP_MLOCK_STATUS_UNSET;
    PMPI_Comm_rank(user_root_comm, &user_root_rank);

    /* Get group id (the first root's world rank). */
    PMPI_Comm_group(user_root_comm, &user_root_group);
    mpi_errno = PMPI_Group_translate_ranks(user_root_group, 1, &first_user_root_rank,
                                           CSP_PROC.wgroup, &group_id);

    /* Wait till all user roots have acquired lock. */
    do {
        int min_lock_stats[2];
        int my_lock_stats[2];

        if (lock_status == CSP_MLOCK_STATUS_UNSET) {
            /* only initial, discarded, released locks need issue lock request. */
            mpi_errno = mlock_issue_acquire_req(group_id);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        if (lock_status != CSP_MLOCK_STATUS_ACQUIRED) {
            /* new state can be acquired, suspended_l, suspended_h. */
            mpi_errno = mlock_sync_status(&lock_status);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        my_lock_stats[0] = lock_status; /* suspended_l < suspended_h < acquired */
        my_lock_stats[1] = user_root_rank;
        mpi_errno = PMPI_Allreduce(my_lock_stats, min_lock_stats, 1, MPI_2INT, MPI_MINLOC,
                                   user_root_comm);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;
        CSPU_MLOCK_DBG_PRINT(" \t all-reduced LOCK status min(stat %d [%s], root %d)\n",
                             min_lock_stats[0], cwp_lock_status_name[min_lock_stats[0]],
                             min_lock_stats[1]);

        /* when the smallest group_id is equal to group_id + np, all other roots got its lock. */
        all_lock_acquired = (min_lock_stats[0] == CSP_MLOCK_STATUS_ACQUIRED) ? 1 : 0;
        if (!all_lock_acquired) {
            char bc_byte;
            int lowest_priority = min_lock_stats[0];
            int lowest_p_root = min_lock_stats[1];

            if (lowest_priority == CSP_MLOCK_STATUS_SUSPENDED_L) {
                /* only one of the lowest priority root wait for a lock */
                if (user_root_rank == lowest_p_root) {
                    /* my lock is suspended with low priority now,
                     * wait till all higher priority groups finished work. */
                    do {
                        mpi_errno = mlock_sync_status(&lock_status);
                        if (mpi_errno != MPI_SUCCESS)
                            goto fn_fail;
                    } while (lock_status != CSP_MLOCK_STATUS_ACQUIRED);
                }
                /* all other roots give up its lock and wait for the first root */
                else {
                    if (lock_status == CSP_MLOCK_STATUS_ACQUIRED) {
                        mpi_errno = mlock_issue_release_req(group_id);
                    }
                    else {
                        mpi_errno = mlock_issue_discard_req(group_id);
                    }
                    if (mpi_errno != MPI_SUCCESS)
                        goto fn_fail;

                    /* drop any SYNC received before the discarded ACK (none). */
                    do {
                        mpi_errno = mlock_sync_status(&lock_status);
                        if (mpi_errno != MPI_SUCCESS)
                            goto fn_fail;
                    } while (lock_status != CSP_MLOCK_STATUS_UNSET);
                }

                /* wait till the first root got lock */
                mpi_errno = PMPI_Bcast(&bc_byte, 1, MPI_CHAR, lowest_p_root, user_root_comm);
                if (mpi_errno != MPI_SUCCESS)
                    goto fn_fail;
                CSPU_MLOCK_DBG_PRINT(" \t root %d bcast from %d in user root group\n",
                                     user_root_rank, lowest_p_root);
            }
        }
    } while (!all_lock_acquired);

  fn_exit:
    if (user_root_group != MPI_GROUP_NULL)
        PMPI_Group_free(&user_root_group);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}