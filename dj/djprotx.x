/*
 * Distributed HiStar protocol
 */

%#include <bigint.h>
%#include <inc/label.h>		/* for LB_LEVEL_STAR */

typedef unsigned dj_timestamp;	/* UNIX seconds */

struct dj_pubkey {
    bigint n;
    unsigned k;
};

struct dj_gcat {		/* Global category name */
    dj_pubkey key;
    unsigned hyper id;
};

struct dj_address {
    unsigned ip;		/* network byte order */
    unsigned port;		/* network byte order */
};

/*
 * Labels.
 */

struct dj_label_entry {
    dj_gcat cat;
    unsigned level;		/* LB_LEVEL_STAR from <inc/label.h> */
};

struct dj_label {
    dj_label_entry ents<>;
    unsigned deflevel;
};

struct dj_cat_mapping {
    dj_gcat gcat;
    unsigned hyper lcat;
    unsigned hyper res_ct;	/* container storing the mapping resources */
    unsigned hyper res_id;	/* resource object ID in that container */
};

struct dj_catmap {
    dj_cat_mapping ents<>;
};

/*
 * Delegations.
 */

enum dj_entity_type {
    ENT_PUBKEY = 1,
    ENT_GCAT,
    ENT_ADDRESS
};

union dj_entity switch (dj_entity_type type) {
 case ENT_PUBKEY:
    dj_pubkey key;
 case ENT_GCAT:
    dj_gcat gcat;
 case ENT_ADDRESS:
    dj_address addr;
};

struct dj_delegation {		/* a speaks-for b, within time window */
    dj_entity a;
    dj_entity b;
    dj_timestamp from_ts;
    dj_timestamp until_ts;
};

struct dj_delegation_set {
    opaque ents<>;		/* really XDR-encoded dj_signed_stmt */
};

/*
 * Message transfer.
 */

enum dj_endpoint_type {
    EP_GATE = 1
};

struct dj_gatename {
    unsigned hyper gate_ct;
    unsigned hyper gate_id;
};

union dj_message_endpoint switch (dj_endpoint_type type) {
 case EP_GATE:
    dj_gatename gate;
};

struct dj_message {
    dj_message_endpoint target;	/* gate or segment to call on delivery */
    unsigned hyper msg_ct;	/* container ID for message segment */
    unsigned hyper token;	/* token returned on sending (0=none) */
    dj_label taint;		/* taint of message */
    dj_label glabel;		/* grant label on gate invocation */
    dj_label gclear;		/* grant clearance on gate invocation */
    dj_catmap catmap;		/* target node category mappings */
    dj_delegation_set dels;	/* supporting delegations */
    opaque msg<>;
};

enum dj_delivery_code {
    DELIVERY_DONE = 1,
    DELIVERY_TIMEOUT,
    DELIVERY_NO_ADDRESS,
    DELIVERY_LOCAL_DELEGATION,
    DELIVERY_REMOTE_DELEGATION,
    DELIVERY_LOCAL_MAPPING,
    DELIVERY_REMOTE_MAPPING,
    DELIVERY_LOCAL_ERR,
    DELIVERY_REMOTE_ERR
};

union dj_message_status switch (dj_delivery_code code) {
 case DELIVERY_DONE:
    unsigned hyper token;	/* 0=none, token issued on delivery */
 default:
    void;
};

enum dj_msg_op {
    MSG_REQUEST = 1,
    MSG_STATUS
};

union dj_msg_u switch (dj_msg_op op) {
 case MSG_REQUEST:
    dj_message req;
 case MSG_STATUS:
    dj_message_status stat;
};

struct dj_msg_xfer {
    dj_pubkey from;
    dj_pubkey to;
    unsigned hyper xid;
    dj_msg_u u;
};

/*
 * Signed statements that can be made by entities.  Every network
 * message is a statement.
 *
 * Fully self-describing statements ensure that one statement
 * cannot be mistaken for another in a different context.
 */

enum dj_stmt_type {
    STMT_DELEGATION = 1,
    STMT_MSG_XFER
};

union dj_stmt switch (dj_stmt_type type) {
 case STMT_DELEGATION:
    dj_delegation delegation;
 case STMT_MSG_XFER:
    dj_msg_xfer msgx;
};

struct dj_stmt_signed {
    dj_stmt stmt;
    bigint sign;
};

