#pragma once
#include <stdbool.h>

/* Caller MUST serialize commit and cancel with the same short mutex.
 * A successful commit authorizes one frame even if cancellation follows.
 * This gate cannot abort SDK I/O. Zero initialization starts a new scan. */
typedef struct {
    bool cancelled;
} LcTransactionGate;
bool lc_transaction_commit(const LcTransactionGate* gate);
void lc_transaction_cancel(LcTransactionGate* gate);
