#ifndef JOS_DJ_DJAUTORPC_HH
#define JOS_DJ_DJAUTORPC_HH

#include <inc/labelutil.hh>
#include <dj/djsrpc.hh>
#include <dj/djcache.hh>
#include <dj/djlabel.hh>

enum { autorpc_debug = 0 };

class dj_autorpc {
 public:
    dj_autorpc(gate_sender *gs, time_t tmo, const dj_pubkey &pk,
	       dj_global_cache &cache)
	: gs_(gs), tmo_(tmo), pk_(pk), cache_(&cache),
	  home_(cache[gs->hostkey()]), nc_(cache[pk]) {}

    template<class TA, class TR>
    dj_delivery_code call(const dj_slot &ep, const TA &arg, TR &res,
			  label *taint = 0,
			  label *grant = 0,
			  label *gclear = 0,
			  label *xgrant = 0,
			  bool gateret = false)
    {
	dj_delegation_map loc_dmap;
	dj_delegation_map rem_dmap;

	dj_catmap_indexed loc_cm;
	dj_catmap_indexed rem_cm;

	dj_message reqm;
	reqm.to = pk_;
	reqm.target = ep;

	label tl;
	if (!taint) {
	    thread_cur_label(&tl);
	    tl.transform(label::star_to, tl.get_default());
	}

	if (autorpc_debug) {
	    if (taint)
		warn << "autorpc: taint " << taint->to_string() << "\n";
	    if (grant)
		warn << "autorpc: grant " << grant->to_string() << "\n";
	    if (gclear)
		warn << "autorpc: gclear " << gclear->to_string() << "\n";

	    warn << "autorpc: starting label conversions...\n";
	}

	/* Convert labels into global equivalents; fill catmap for local node */
	label_to_djlabel(home_->cmi_, taint  ? *taint  : tl,       &reqm.taint,  label_taint, false, &loc_cm);
	label_to_djlabel(home_->cmi_, grant  ? *grant  : label(3), &reqm.glabel, label_owner, false, &loc_cm);
	label_to_djlabel(home_->cmi_, gclear ? *gclear : label(0), &reqm.gclear, label_clear, false, &loc_cm);

	if (autorpc_debug) {
	    warn << "autorpc: global taint " << reqm.taint
		 << " grant " << reqm.glabel
		 << " gclear " << reqm.gclear << "\n";
	    warn << "autorpc: starting remote-host conversions...\n";
	}

	/* If we are granting some non-globally-named categories,
	 * just to the local exporter, see if we happen to have
	 * some mappings for them, and include them in the local
	 * catmap if so.
	 */
	if (xgrant)
	    label_to_djlabel(home_->cmi_, *xgrant, 0, label_owner, true, &loc_cm);

	/* Populate the catmap for the remote node */
	bool skip_missing = false;
	if (ep.type == EP_MAPCREATE)
	    skip_missing = true;

	djlabel_to_label(nc_->cmi_, reqm.taint,  0, label_taint, skip_missing, &rem_cm);
	djlabel_to_label(nc_->cmi_, reqm.glabel, 0, label_owner, skip_missing, &rem_cm);
	djlabel_to_label(nc_->cmi_, reqm.gclear, 0, label_clear, skip_missing, &rem_cm);

	if (autorpc_debug)
	    warn << "autorpc: done with conversions.\n";

	/*
	 * XXX
	 * Currently we don't choose the right delegations,
	 * and just send all of them..
	 */
	loc_dmap.insert(cache_->dmap_);
	rem_dmap.insert(cache_->dmap_);

	reqm.catmap = rem_cm.to_catmap();
	reqm.dset = rem_dmap.to_delegation_set();

	if (autorpc_debug) {
	    for (uint64_t i = 0; i < reqm.catmap.ents.size(); i++)
		warn << "autorpc_call: catmap entry for "
		     << reqm.catmap.ents[i].gcat << " <-> "
		     << reqm.catmap.ents[i].lcat << "\n";
	}

	str reqstr = xdr2str(arg);
	if (!reqstr) {
	    warn << "dj_autorpc: cannot marshal\n";
	    return DELIVERY_LOCAL_ERR;
	}

	dj_message resm;
	dj_delivery_code code = dj_rpc_call(gs_, tmo_,
					    loc_dmap.to_delegation_set(),
					    loc_cm.to_catmap(),
					    reqm, reqstr, &resm, xgrant,
					    taint, gateret);
	if (code != DELIVERY_DONE)
	    return code;

	if (!bytes2xdr(res, resm.msg)) {
	    warn << "dj_autorpc: cannot unmarshal\n";
	    return DELIVERY_LOCAL_ERR;
	}

	dj_catmap_indexed res_cmi(resm.catmap);
	djlabel_to_label(res_cmi, resm.taint,  taint,  label_taint, false, &home_->cmi_);
	djlabel_to_label(res_cmi, resm.glabel, grant,  label_owner, false, &home_->cmi_);
	djlabel_to_label(res_cmi, resm.gclear, gclear, label_clear, false, &home_->cmi_);

	cache_->dmap_.insert(resm.dset);
	return code;
    }

 private:
    gate_sender *gs_;
    time_t tmo_;
    dj_pubkey pk_;
    dj_global_cache *cache_;
    dj_node_cache *home_;
    dj_node_cache *nc_;
};

#endif
