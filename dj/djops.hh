#include <dj/dj.h>

inline bool
operator<(const dj_esign_pubkey &a, const dj_esign_pubkey &b)
{
    return a.n < b.n || (a.n == b.n && a.k < b.k);
}

inline const strbuf &
strbuf_cat(const strbuf &sb, const dj_esign_pubkey &pk)
{
    sb << "{" << pk.n << "," << pk.k << "}";
    return sb;
}

inline const strbuf &
strbuf_cat(const strbuf &sb, const dj_gcat &gc)
{
    sb << "<" << gc.key << "." << gc.id << ">";
    return sb;
}

inline const strbuf &
strbuf_cat(const strbuf &sb, const dj_address &a)
{
    in_addr ia;
    ia.s_addr = a.ip;
    sb << inet_ntoa(ia) << ":" << ntohs(a.port);
    return sb;
}

inline const strbuf &
strbuf_cat(const strbuf &sb, const dj_entity &dje)
{
    switch (dje.type) {
    case ENT_PUBKEY:
	sb << *dje.key;
	break;

    case ENT_GCAT:
	sb << *dje.gcat;
	break;

    case ENT_ADDRESS:
	sb << *dje.addr;
	break;
    }

    return sb;
}
