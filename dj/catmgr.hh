#ifndef JOS_DJ_CATMGR_HH
#define JOS_DJ_CATMGR_HH

#include <dj/djprotx.h>
#include <dj/reqcontext.hh>

class catmgr {
 public:
    virtual ~catmgr() {}
    virtual void acquire(const dj_catmap&, bool droplater = false) = 0;
    virtual void resource_check(request_context*, const dj_catmap&) = 0;
    virtual dj_cat_mapping store(const dj_gcat&, uint64_t lcat, uint64_t uct) = 0;

    static catmgr* alloc();
};

#endif