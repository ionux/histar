#include <async.h>
#include <crypt.h>
#include <wmstr.h>
#include <dj/dis.hh>
#include <dj/djops.hh>

static void
stuffcb(ptr<djprot> p, str node_pk, dj_reply_status stat, const djcall_args &args)
{
    warn << "stuffcb: status " << stat << "\n";
    if (stat == REPLY_DONE)
	warn << "stuffcb: data " << args.data << "\n";
}

static void
dostuff(ptr<djprot> p, str node_pk)
{
    dj_gatename gate;
    djcall_args args;
    args.data = "Hello world";

    p->call(node_pk, gate, args, wrap(&stuffcb, p, node_pk));

    delaycb(5, wrap(&dostuff, p, node_pk));
}

int
main(int ac, char **av)
{
    uint16_t port = 5923;
    warn << "instantiating a djprot, port " << port << "...\n";
    ptr<djprot> djs = djprot::alloc(port);
    djs->set_callexec(wrap(dj_dummy_exec));

    if (ac == 2) {
	str n(av[1]);
	dj_esign_pubkey k;
	k.n = bigint(n, 16);
	k.k = 8;
	dostuff(djs, xdr2str(k));
    }

    amain();
}
