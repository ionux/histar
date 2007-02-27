#include <dj/delegator.hh>
#include <dj/internalx.h>
#include <dj/djops.hh>
#include <dj/djrpcx.h>

void
delegation_create(djprot *p, token_factory *tf, const dj_pubkey &sender,
		  const dj_message &m, const delivery_args &da)
{
    if (m.target.type != EP_DELEGATOR) {
	warn << "delegation_create: not a delegator target\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    dj_call_msg callmsg;
    dj_delegate_req dreq;
    if (!bytes2xdr(callmsg, m.msg) || !bytes2xdr(dreq, callmsg.buf)) {
	warn << "delegation_create: cannot unmarshal\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    if (callmsg.return_ep.type != EP_GATE) {
	warn << "delegation_create: must return to a gate\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    const dj_gcat &gcat = dreq.gcat;
    bool owner = false;
    for (uint32_t i = 0; i < m.glabel.ents.size(); i++)
	if (m.glabel.ents[i].cat == gcat &&
	    m.glabel.ents[i].level == LB_LEVEL_STAR)
	    owner = true;

    if (!owner) {
	warn << "delegation_create: not owner\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    dj_stmt_signed ss;
    ss.stmt.set_type(STMT_DELEGATION);
    ss.stmt.delegation->a.set_type(ENT_PUBKEY);
    *ss.stmt.delegation->a.key = dreq.to;
    ss.stmt.delegation->b.set_type(ENT_GCAT);
    *ss.stmt.delegation->b.gcat = gcat;
    if (gcat.key != p->pubkey())
	*ss.stmt.delegation->via.alloc() = p->pubkey();
    ss.stmt.delegation->from_ts = dreq.from_ts;
    ss.stmt.delegation->until_ts = dreq.until_ts;
    p->sign_statement(&ss);

    dj_message replym;
    replym.target = callmsg.return_ep;
    replym.msg_ct = callmsg.return_ct;
    replym.token = tf->token();
    replym.taint = m.taint;
    replym.glabel.deflevel = 3;
    replym.gclear.deflevel = 0;
    replym.catmap = callmsg.return_cm;
    replym.dset = callmsg.return_ds;
    replym.msg = xdr2str(ss);

    p->send(sender, 0, m.dset, replym, 0, 0);
    da.cb(DELIVERY_DONE, replym.token);
}