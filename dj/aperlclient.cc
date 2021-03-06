#include <dj/djprot.hh>
#include <dj/djops.hh>
#include <dj/djarpc.hh>
#include <dj/internalx.h>
#include <dj/miscx.h>
#include <dj/execmux.hh>
#include <dj/stuff.hh>
#include <dj/directexec.hh>
#include <dj/delegator.hh>

enum { dbg = 1 };

struct perl_req {
    djprot *p;
    dj_gate_factory *f;
    dj_pubkey k;
    uint64_t call_ct;
    uint64_t taint_ct;
    dj_gatename srvgate;
    dj_gcat gcat;

    uint64_t start_usec;

    dj_delegation_set dset;
    dj_catmap catmap;
};

static uint64_t
time_usec()
{
    struct timeval tv;
    gettimeofday(&tv, 0);

    uint64_t usec = tv.tv_sec;
    usec = usec * 1000000 + tv.tv_usec;
    return usec;
}

static void
perl_cb(perl_req *pr, ptr<dj_arpc_call> old_call,
	dj_delivery_code c, const dj_message *rm)
{
    uint64_t end_usec = time_usec();
    if (dbg)
	warn << "perl runtime: " << (end_usec - pr->start_usec) << " usec\n";

    if (c != DELIVERY_DONE) {
	warn << "perl delivery code " << c << "\n";
	return;
    }

    warn << "finished: " << time_usec() << "\n";

    perl_run_res pres;
    assert(bytes2xdr(pres, rm->msg));
    if (pres.retval != 0 || pres.output != "Hello world.") {
	warn << "Perl exit code: " << pres.retval << "\n";
	warn << "Perl output: " << pres.output << "\n";
    }
}

static void
ctalloc_cb(perl_req *pr, ptr<dj_arpc_call> old_call,
	   dj_delivery_code c, const dj_message *rm)
{
    if (dbg)
	warn << "ctalloc_cb: " << (time_usec() - pr->start_usec) << " usec\n";

    container_alloc_res ctres;
    if (c != DELIVERY_DONE)
	fatal << "ctalloc_cb: code " << c << "\n";
    assert(bytes2xdr(ctres, rm->msg));
    pr->taint_ct = ctres.ct_id;

    /* Send a real request now.. */
    perl_run_arg parg;
    parg.script = str("print 'Hello world.';");
    parg.input = str("");

    dj_message m;
    m.to = pr->k;
    m.target.set_type(EP_GATE);
    m.target.ep_gate->msg_ct = pr->taint_ct;
    m.target.ep_gate->gate = pr->srvgate;
    m.dset = pr->dset;
    m.catmap = pr->catmap;
    m.taint.ents.push_back(pr->gcat);

    ptr<dj_arpc_call> call = New refcounted<dj_arpc_call>(pr->p, pr->f, 0xdead);
    call->call(5, pr->dset, m, xdr2str(parg),
	       wrap(&perl_cb, pr, call), &pr->catmap, &pr->dset);
}

static void
delegate_cb(perl_req *pr, ptr<dj_arpc_call> old_call,
	    dj_delivery_code c, const dj_message *rm)
{
    if (dbg)
	warn << "delegate_cb: " << (time_usec() - pr->start_usec) << " usec\n";

    dj_delegate_res dres;
    assert(c == DELIVERY_DONE);
    assert(bytes2xdr(dres, rm->msg));
    assert(dres.delegations.size() == 1);

    rpc_bytes<2147483647ul> s;
    xdr2bytes(s, dres.delegations[0]);
    pr->dset.ents.push_back(s);

    /* Create a remote tainted container */
    container_alloc_req ctreq;
    ctreq.parent = pr->call_ct;
    ctreq.quota = CT_QUOTA_INF;
    ctreq.timeout_sec = 5;
    ctreq.label.ents.push_back(pr->gcat);

    dj_message m;
    m.to = pr->k;
    m.target.set_type(EP_GATE);
    m.target.ep_gate->msg_ct = pr->call_ct;
    m.target.ep_gate->gate.gate_ct = 0;
    m.target.ep_gate->gate.gate_id = GSPEC_CTALLOC;
    m.dset = pr->dset;
    m.catmap = pr->catmap;
    m.glabel.ents.push_back(pr->gcat);
    m.gclear.ents.push_back(pr->gcat);

    ptr<dj_arpc_call> call = New refcounted<dj_arpc_call>(pr->p, pr->f, 0xdead);
    call->call(5, pr->dset, m, xdr2str(ctreq),
	       wrap(&ctalloc_cb, pr, call), &pr->catmap, &pr->dset);
}

static void
map_create_cb(perl_req *pr, ptr<dj_arpc_call> old_call,
	      dj_delivery_code c, const dj_message *rm)
{
    if (dbg)
	warn << "mapcreate_cb: " << (time_usec() - pr->start_usec) << " usec\n";

    dj_mapcreate_res mres;
    assert(c == DELIVERY_DONE);
    assert(bytes2xdr(mres, rm->msg));
    assert(mres.mappings.size() == 1);

    dj_cat_mapping &cme = mres.mappings[0];
    pr->catmap.ents.push_back(cme);

    /* Create a delegation for the remote host */
    dj_delegate_req dreq;
    dreq.gcat = pr->gcat;
    dreq.to = pr->k;
    dreq.from_ts = 0;
    dreq.until_ts = ~0;

    dj_delegate_arg darg;
    darg.reqs.push_back(dreq);

    dj_message m;
    m.to = pr->p->pubkey();
    m.target.set_type(EP_DELEGATOR);
    m.glabel.ents.push_back(pr->gcat);

    ptr<dj_arpc_call> call = New refcounted<dj_arpc_call>(pr->p, pr->f, 0xdead);
    call->call(5, pr->dset, m, xdr2str(darg),
	       wrap(&delegate_cb, pr, call));
}

static void
do_stuff(perl_req *pr)
{
    /* Fabricate a new global category for tainting */
    pr->gcat.key = pr->p->pubkey();
    pr->gcat.id = 0xc0ffee;
    pr->gcat.integrity = 0;

    /* Create a mapping on the remote machine */
    dj_mapreq mapreq;
    mapreq.ct = pr->call_ct;
    mapreq.gcat = pr->gcat;
    mapreq.lcat = 0;

    dj_mapcreate_arg marg;
    marg.reqs.push_back(mapreq);

    dj_message m;
    m.to = pr->k;
    m.target.set_type(EP_MAPCREATE);

    pr->start_usec = time_usec();
    ptr<dj_arpc_call> call = New refcounted<dj_arpc_call>(pr->p, pr->f, 0xdead);
    call->call(5, pr->dset, m, xdr2str(marg),
	       wrap(&map_create_cb, pr, call));
}

static void
start_stuff(perl_req *pr, int count)
{
    warn << "starting: " << time_usec() << "\n";
    for (int i = 0; i < count; i++) {
	perl_req *pr2 = new perl_req(*pr);
	do_stuff(pr2);
    }
}

int
main(int ac, char **av)
{
    if (ac != 4) {
	printf("Usage: %s host-pk call-ct perl-gate\n", av[0]);
	exit(-1);
    }

    ptr<sfspub> sfspub = sfscrypt.alloc(av[1], SFS_VERIFY | SFS_ENCRYPT);
    assert(sfspub);

    perl_req pr;
    pr.k = sfspub2dj(sfspub);
    pr.call_ct = strtoll(av[2], 0, 0);
    pr.srvgate <<= av[3];

    uint16_t port = 5923;
    djprot *djs = djprot::alloc(port);

    exec_mux emux;
    djs->set_delivery_cb(wrap(&emux, &exec_mux::exec));

    dj_direct_gatemap gm;
    emux.set(EP_GATE, wrap(&gm, &dj_direct_gatemap::deliver));
    emux.set(EP_DELEGATOR, wrap(&delegation_create, djs));

    pr.p = djs;
    pr.f = &gm;

    delaycb(5, wrap(&start_stuff, &pr, 1));
    delaycb(6, wrap(&start_stuff, &pr, 1));
    delaycb(7, wrap(&start_stuff, &pr, 10));

    amain();
}
