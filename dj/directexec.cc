#include <dj/directexec.hh>

void
dj_direct_gatemap::deliver(const dj_message &a,
			   const delivery_args &da)
{
    if (a.target.type != EP_GATE) {
	da.cb(DELIVERY_REMOTE_ERR);
	return;
    }

    dj_msg_sink *s = gatemap_[COBJ(a.target.ep_gate->gate.gate_ct,
				   a.target.ep_gate->gate.gate_id)];
    if (!s) {
	da.cb(DELIVERY_REMOTE_ERR);
	return;
    }

    (*s)(a);
    da.cb(DELIVERY_DONE);
}

dj_slot
dj_direct_gatemap::create_gate(uint64_t ct, dj_msg_sink cb)
{
    dj_slot ep;
    ep.set_type(EP_GATE);
    ep.ep_gate->msg_ct = 0xdeadbeef;
    ep.ep_gate->gate.gate_ct = ct;
    ep.ep_gate->gate.gate_id = ++counter_;
    gatemap_.insert(COBJ(ep.ep_gate->gate.gate_ct, ep.ep_gate->gate.gate_id), cb);
    return ep;
}

void
dj_direct_gatemap::set_ep(const dj_slot &ep, dj_msg_sink cb)
{
    assert(ep.type == EP_GATE);
    gatemap_.insert(COBJ(ep.ep_gate->gate.gate_ct, ep.ep_gate->gate.gate_id), cb);
}

void
dj_direct_gatemap::destroy(const dj_slot &ep)
{
    if (ep.type != EP_GATE) {
	warn << "dj_direct_gatemap::destroy: not a gate\n";
	return;
    }

    cobj_ref cid = COBJ(ep.ep_gate->gate.gate_ct, ep.ep_gate->gate.gate_id);
    gatemap_.remove(cid);
}
