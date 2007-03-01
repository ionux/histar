#ifndef JOS_HTTPD_INC_SSLPROXY_HH
#define JOS_HTTPD_INC_SSLPROXY_HH

class ssl_proxy
{
 public:
    ssl_proxy(struct cobj_ref ssld_gate, struct cobj_ref eproc_gate,
	      uint64_t base_ct, int sock_fd);
    ~ssl_proxy(void);

    void start(void);

    cobj_ref plain_bipipe(void) { return plain_bipipe_; }

    static const char bipipe_client = 0;
    static const char bipipe_ssld   = 1;

 private:
    struct info {
	uint64_t taint_;
	cobj_ref cipher_bipipe_;
	int sock_fd_;
	uint64_t base_ct_;
	uint64_t ssl_ct_;
	atomic64_t ref_count_;
    } *nfo_;
    cobj_ref ssld_gate_;
    cobj_ref eproc_gate_;
    cobj_ref plain_bipipe_;

    char proxy_started_;
    char ssld_started_;
    char eproc_started_;
    struct thread_args ssld_worker_args_;
    struct thread_args eproc_worker_args_;
    
    static void proxy_thread(void *a);
    static void cleanup(struct info *nfo);
    static int select(int sock_fd, int cipher_fd, uint64_t msec);

    static const uint32_t SELECT_SOCK =   0x0001;
    static const uint32_t SELECT_CIPHER = 0x0002;
};

#endif
