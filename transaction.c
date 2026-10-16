#include "transaction.h"

bool lc_transaction_commit(const LcTransactionGate* gate) {
    return !gate->cancelled;
}

void lc_transaction_cancel(LcTransactionGate* gate) {
    gate->cancelled = true;
}
