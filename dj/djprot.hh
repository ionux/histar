#ifndef JOS_DJ_DJPROT_HH
#define JOS_DJ_DJPROT_HH

#include <async.h>
#include <sfscrypt.h>
#include <dj/djprotx.h>

typedef callback<void, dj_delivery_code>::ptr delivery_status_cb;

struct delivery_args {
    delivery_status_cb cb;
    void *local_delivery_arg;
};

class message_sender {
 public:
    virtual ~message_sender() {}
    virtual const dj_pubkey &pubkey() const = 0;
    virtual void send(const dj_message &msg,
		      const dj_delegation_set &dset,
		      delivery_status_cb cb,
		      void *delivery_arg) = 0;
};

class djprot : public message_sender {
 public:
    typedef callback<void, const dj_message&,
			   const delivery_args&>::ptr local_delivery_cb;

    virtual ptr<sfspriv> privkey() = 0;
    virtual void set_delivery_cb(local_delivery_cb cb) = 0;
    virtual void set_hostinfo(const dj_hostinfo&) = 0;
    virtual void sign_statement(dj_stmt_signed*) = 0;

    static djprot *alloc(uint16_t port);
};

#endif
