#include <inc/labelutil.hh>
#include <inc/privstore.hh>
#include <inc/scopeguard.hh>
#include <dj/mapcreate.hh>
#include <dj/internalx.h>
#include <dj/djlabel.hh>
#include <dj/djops.hh>
#include <dj/djrpcx.h>

void
histar_mapcreate::exec(const dj_pubkey &sender, const dj_message &m,
		       const delivery_args &da)
{
    if (m.target.type != EP_MAPCREATE) {
	warn << "histar_mapcreate: not a mapcreate target\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    local_mapcreate_state *lms = (local_mapcreate_state *) da.local_delivery_arg;

    label vl, vc;
    dj_label reply_taint;

    dj_catmap_indexed cmi(m.catmap);
    try {
	if (lms) {
	    vl = *lms->vl;
	    vc = *lms->vc;

	    label taint = vl;
	    taint.transform(label::star_to, taint.get_default());
	    label_to_djlabel(cmi, taint, &reply_taint);
	} else {
	    djlabel_to_label(cmi, m.taint, &vl);
	    vc = vl;
	    reply_taint = m.taint;
	}
    } catch (std::exception &e) {
	warn << "histar_mapcreate: " << e.what() << "\n";
	da.cb(DELIVERY_REMOTE_MAPPING, 0);
	return;
    }

    verify_label_reqctx ctx(vl, vc);
    try {
	cm_->acquire(m.catmap, true);
	cm_->resource_check(&ctx, m.catmap);
    } catch (std::exception &e) {
	warn << "histar_mapcreate: " << e.what() << "\n";
	da.cb(DELIVERY_REMOTE_MAPPING, 0);
	return;
    }

    dj_call_msg callmsg;
    dj_mapreq mapreq;
    if (!bytes2xdr(callmsg, m.msg) || !bytes2xdr(mapreq, callmsg.buf)) {
	warn << "histar_mapcreate: cannot unmarshal\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    if (callmsg.return_ep.type != EP_GATE) {
	warn << "histar_mapcreate: must return to a gate\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    if (!ctx.can_rw(COBJ(mapreq.ct, mapreq.ct))) {
	warn << "histar_mapcreate: cannot write target ct\n";
	da.cb(DELIVERY_REMOTE_ERR, 0);
	return;
    }

    dj_cat_mapping mapent;
    if (mapreq.lcat) {		/* Create a global category */
	dj_gcat gcat;
	gcat.key = p_->pubkey();
	gcat.id = ++counter_;
	gcat.integrity = mapreq.gcat.integrity;

	if (cmi.l2g(mapreq.lcat, 0)) {
	    // Caller already provided an existing mapping for lcat,
	    // so just create a new mapping.
	    mapent = cm_->store(gcat, mapreq.lcat, mapreq.ct);
	} else {
	    // Caller better have granted us the star on gate invocation,
	    // because this is the first mapping for this category.
	    if (!lms) {
		warn << "histar_mapcreate: missing local_delivery_arg\n";
		da.cb(DELIVERY_REMOTE_ERR, 0);
		return;
	    }

	    label gl;
	    obj_get_label(lms->privgate, &gl);
	    if (gl.get(mapreq.lcat) != LB_LEVEL_STAR) {
		warn << "histar_mapcreate: trying to map unknown lcat\n";
		da.cb(DELIVERY_REMOTE_ERR, 0);
		return;
	    }

	    saved_privilege sp(mapreq.lcat, lms->privgate);
	    sp.acquire();
	    scope_guard<void, uint64_t> drop(thread_drop_star, mapreq.lcat);
	    mapent = cm_->store(gcat, mapreq.lcat, mapreq.ct);
	}
    } else {
	int64_t lcat = handle_alloc();
	if (lcat < 0) {
	    warn << "histar_mapcreate: cannot allocate handle\n";
	    da.cb(DELIVERY_REMOTE_ERR, 0);
	    return;
	}

	scope_guard<void, uint64_t> drop(thread_drop_star, lcat);
	mapent = cm_->store(mapreq.gcat, lcat, mapreq.ct);
    }

    dj_message replym;
    replym.target = callmsg.return_ep;
    replym.token = mapent.res_ct;
    replym.taint = reply_taint;
    replym.catmap = callmsg.return_cm;
    replym.dset = callmsg.return_ds;
    replym.msg = xdr2str(mapent);

    p_->send(sender, 0, m.dset, replym, 0, 0);
    da.cb(DELIVERY_DONE, replym.token);
}
