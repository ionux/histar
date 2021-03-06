/*
 * Message format for incoming requests into djd's local gate.
 */

%#include <dj/djprotx.h>

struct dj_incoming_gate_req {
    dj_delegation_set dset;
    dj_catmap catmap;
    dj_message m;

    uint64_t res_ct;
    uint64_t res_seg;
};

struct dj_outgoing_gate_msg {
    dj_gatename djd_gate;
    dj_message m;
};
