extern "C" {
#include <inc/gateparam.h>
}

#include <crypt.h>
#include <inc/scopeguard.hh>
#include <inc/gateclnt.hh>
#include <inc/labelutil.hh>
#include <dj/gatesender.hh>
#include <dj/djgate.h>

dj_delivery_code
gate_sender::send(const dj_pubkey &node, time_t timeout,
		  const dj_delegation_set &dset, const dj_catmap &cm,
		  const dj_message &msg, uint64_t *tokenp)
{
    dj_incoming_gate_req req;
    req.node = node;
    req.timeout = timeout;
    req.dset = dset;
    req.catmap = cm;
    req.m = msg;

    gate_call gc(g_, 0, 0, 0);

    label reqlabel(1);
    reqlabel.set(gc.call_grant(), 0);
    reqlabel.set(gc.call_taint(), 3);

    str reqstr = xdr2str(req);
    gate_call_data gcd;

    void *data_map = 0;
    error_check(segment_alloc(gc.call_ct(), reqstr.len(), &gcd.param_obj,
			      &data_map, reqlabel.to_ulabel(), "gate_sender req"));
    scope_guard2<int, void*, int> unmap(segment_unmap_delayed, data_map, 1);
    memcpy(data_map, reqstr.cstr(), reqstr.len());

    label vl, vc;
    thread_cur_label(&vl);
    thread_cur_clearance(&vc);
    gc.call(&gcd, &vl, &vc);

    dj_incoming_gate_res res;
    str inline_buf(&gcd.param_buf[0], sizeof(gcd.param_buf));
    if (!str2xdr(res, inline_buf))
	throw basic_exception("cannot unmarshal response");

    if (res.stat == DELIVERY_DONE)
	*tokenp = *res.token;
    return res.stat;
}
