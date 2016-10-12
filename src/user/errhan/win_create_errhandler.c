/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *
 *  (C) 2016 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include "cspu.h"

int MPI_Win_create_errhandler(MPI_Win_errhandler_function * win_errhandler_fn,
                              MPI_Errhandler * errhandler)
{
    int mpi_errno = MPI_SUCCESS;

    CSP_CALLMPI(JUMP, PMPI_Win_create_errhandler(win_errhandler_fn, errhandler));

    /* Because no way to get the user function from handler, we store it into hash.
     * It is fetched in comm|win_set_errhandler, and deleted at errhandler_free. */
    CSPU_errhan_cache_fnc(*errhandler, win_errhandler_fn);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
