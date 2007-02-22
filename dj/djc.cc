#include <inc/labelutil.hh>
#include <dj/djprot.hh>
#include <dj/djops.hh>
#include <dj/gatesender.hh>
#include <dj/djsrpc.hh>
#include <dj/mapcreatex.h>

int
main(int ac, char **av)
{
    if (ac != 4) {
	printf("Usage: %s host-pk call-ct gate-ct.id\n", av[0]);
	exit(-1);
    }

    const char *pk16 = av[1];
    dj_pubkey k;
    k.n = bigint(pk16, 16);
    k.k = 8;

    uint64_t call_ct = atoi(av[2]);

    dj_message_endpoint ep;
    ep.set_type(EP_GATE);
    *ep.gate <<= av[3];

    gate_sender gs;

    dj_delegation_set dset;
    dj_catmap cm;
    dj_message m;

    /* Allocate a new category to taint with.. */
    uint64_t tcat = handle_alloc();
    warn << "allocated category " << tcat << " for tainting\n";

    /* XXX abuse of call_ct -- assumes same node */
    dj_mapreq mapreq;
    mapreq.ct = call_ct;
    mapreq.lcat = tcat;

    m.target.set_type(EP_MAPCREATE);
    m.token = 0;
    m.taint.deflevel = 1;
    m.glabel.deflevel = 3;
    m.gclear.deflevel = 0;

    label xgrant(3);
    xgrant.set(tcat, LB_LEVEL_STAR);

    dj_message replym;
    dj_delivery_code c = dj_rpc_call(&gs, k, 1, dset, cm, m,
				     xdr2str(mapreq), &replym, &xgrant);
    if (c != DELIVERY_DONE)
	fatal << "error talking to mapcreate: code " << c << "\n";

    dj_cat_mapping tcatmap;
    if (!bytes2xdr(tcatmap, replym.msg))
	fatal << "unmarshaling dj_cat_mapping\n";

    warn << "Got a dj_cat_mapping: "
	 << tcatmap.gcat << ", "
	 << tcatmap.lcat << ", "
	 << tcatmap.user_ct << ", "
	 << tcatmap.res_ct << ", "
	 << tcatmap.res_gt << "\n";

    /* Send a real echo request now.. */
    m.target = ep;
    m.msg_ct = call_ct;
    m.token = 0;
    m.taint.deflevel = 1;
    m.glabel.deflevel = 3;
    m.gclear.deflevel = 0;

    c = dj_rpc_call(&gs, k, 1, dset, cm, m, "Hello world", &replym);
    warn << "code = " << c << "\n";
    if (c == DELIVERY_DONE)
	warn << replym;
}
