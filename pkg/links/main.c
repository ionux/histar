#include "links.h"

int retval = RET_OK;

void unhandle_basic_signals(struct terminal *);

void sig_terminate(struct terminal *t)
{
	unhandle_basic_signals(t);
	terminate = 1;
	retval = RET_SIGNAL;
}

void sig_intr(struct terminal *t)
{
	if (!t) {
		unhandle_basic_signals(t);
		terminate = 1;
	} else {
		unhandle_basic_signals(t);
		exit_prog(t, NULL, NULL);
	}
}

void sig_ctrl_c(struct terminal *t)
{
	if (!is_blocked()) kbd_ctrl_c();
}

void sig_ign(void *x)
{
}

void sig_tstp(struct terminal *t)
{
#ifdef SIGSTOP
	int pid = getpid();
	block_itrm(0);
#if defined (SIGCONT) && defined(SIGTTOU)
	if (!fork()) {
		sleep(1);
		kill(pid, SIGCONT);
		exit(0);
	}
#endif
	raise(SIGSTOP);
#endif
}

void sig_cont(struct terminal *t)
{
	if (!unblock_itrm(0)) /*redraw_terminal_cls(t)*/resize_terminal();
	/*else register_bottom_half(raise, SIGSTOP);*/
}

void handle_basic_signals(struct terminal *term)
{
	install_signal_handler(SIGHUP, (void (*)(void *))sig_intr, term, 0);
	install_signal_handler(SIGINT, (void (*)(void *))sig_ctrl_c, term, 0);
	install_signal_handler(SIGTERM, (void (*)(void *))sig_terminate, term, 0);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, (void (*)(void *))sig_tstp, term, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, (void (*)(void *))sig_tstp, term, 0);
#endif
#ifdef SIGTTOU
	install_signal_handler(SIGTTOU, (void (*)(void *))sig_ign, term, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, (void (*)(void *))sig_cont, term, 0);
#endif
}

/*void handle_slave_signals(struct terminal *term)
{
	install_signal_handler(SIGHUP, (void (*)(void *))sig_terminate, term, 0);
	install_signal_handler(SIGINT, (void (*)(void *))sig_terminate, term, 0);
	install_signal_handler(SIGTERM, (void (*)(void *))sig_terminate, term, 0);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, (void (*)(void *))sig_tstp, term, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, (void (*)(void *))sig_tstp, term, 0);
#endif
#ifdef SIGTTOU
	install_signal_handler(SIGTTOU, (void (*)(void *))sig_ign, term, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, (void (*)(void *))sig_cont, term, 0);
#endif
}*/

void unhandle_terminal_signals(struct terminal *term)
{
	install_signal_handler(SIGHUP, NULL, NULL, 0);
	install_signal_handler(SIGINT, NULL, NULL, 0);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, NULL, NULL, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, NULL, NULL, 0);
#endif
#ifdef SIGTTOU
	install_signal_handler(SIGTTOU, NULL, NULL, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, NULL, NULL, 0);
#endif
}

void unhandle_basic_signals(struct terminal *term)
{
	install_signal_handler(SIGHUP, NULL, NULL, 0);
	install_signal_handler(SIGINT, NULL, NULL, 0);
	install_signal_handler(SIGTERM, NULL, NULL, 0);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, NULL, NULL, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, NULL, NULL, 0);
#endif
#ifdef SIGTTOU
	install_signal_handler(SIGTTOU, NULL, NULL, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, NULL, NULL, 0);
#endif
}

int terminal_pipe[2];

int attach_terminal(int in, int out, int ctl, void *info, int len)
{
	struct terminal *term;
	fcntl(terminal_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(terminal_pipe[1], F_SETFL, O_NONBLOCK);
	handle_trm(in, out, out, terminal_pipe[1], ctl, info, len);
	mem_free(info);
	if ((term = init_term(terminal_pipe[0], out, win_func))) {
		handle_basic_signals(term);	/* OK, this is race condition, but it must be so; GPM installs it's own buggy TSTP handler */
		return terminal_pipe[1];
	}
	close(terminal_pipe[0]);
	close(terminal_pipe[1]);
	return -1;
}

struct status dump_stat;
int dump_pos;

int dump_red_count = 0;

void end_dump(struct status *stat, void *p)
{
	struct cache_entry *ce = stat->ce;
	int oh = get_output_handle();
	if (oh == -1) return;
	if (ce && ce->redirect && dump_red_count++ < MAX_REDIRECTS) {
		unsigned char *u, *p;
		if (stat->state >= 0) change_connection(stat, NULL, PRI_CANCEL);
		u = join_urls(ce->url, ce->redirect);
		if (!http_bugs.bug_302_redirect) if (!ce->redirect_get && (p = strchr(ce->url, POST_CHAR))) add_to_strn(&u, p);
		load_url(u, stat, PRI_MAIN, 0);
		mem_free(u);
		return;
	}
	if (stat->state >= 0 && stat->state < S_TRANS) return;
	if (stat->state >= S_TRANS && dmp != D_SOURCE) return;
	if (dmp == D_SOURCE) {
		if (ce) {
			struct fragment *frag;
			nextfrag:
			foreach(frag, ce->frag) if (frag->offset <= dump_pos && frag->offset + frag->length > dump_pos) {
				int l = frag->length - (dump_pos - frag->offset);
				int w = hard_write(oh, frag->data + dump_pos - frag->offset, l);
				if (w != l) {
					detach_connection(stat, dump_pos);
					if (w < 0) fprintf(stderr, "Error writing to stdout: %s.\n", strerror(errno));
					else fprintf(stderr, "Can't write to stdout.\n");
					retval = RET_ERROR;
					goto terminate;
				}
				dump_pos += w;
				detach_connection(stat, dump_pos);
				goto nextfrag;
			}
		}
		if (stat->state >= 0) return;
	} else if (ce) {
		struct document_options o;
		struct view_state *vs;
		struct f_data_c fd;
		if (!(vs = mem_alloc(sizeof(struct view_state) + strlen(stat->ce->url) + 1)))
			goto terminate;
		memset(&o, 0, sizeof(struct document_options));
		memset(vs, 0, sizeof(struct view_state));
		memset(&fd, 0, sizeof(struct f_data_c));
		o.xp = 0;
		o.yp = 1;
		o.xw = screen_width;
		o.yw = 25;
		o.col = 0;
		o.cp = 0;
		ds2do(&dds, &o);
		o.plain = 0;
		o.frames = 0;
		memcpy(&o.default_fg, &default_fg, sizeof(struct rgb));
		memcpy(&o.default_bg, &default_bg, sizeof(struct rgb));
		memcpy(&o.default_link, &default_link, sizeof(struct rgb));
		memcpy(&o.default_vlink, &default_vlink, sizeof(struct rgb));
		o.framename = "";
		init_vs(vs, stat->ce->url);
		cached_format_html(vs, &fd, &o);
		dump_to_file(fd.f_data, oh);
		detach_formatted(&fd);
		destroy_vs(vs);
		mem_free(vs);

	}
	if (stat->state != S_OK) {
		unsigned char *m = get_err_msg(stat->state);
		fprintf(stderr, "%s\n", get_english_translation(m));
		retval = RET_ERROR;
		goto terminate;
	}
	terminate:
	terminate = 1;
}

int ac;
unsigned char **av;

unsigned char *path_to_exe;

int init_b = 0;

void init()
{
	int uh;
	void *info;
	int len;
	unsigned char *u;
	init_trans();
	set_sigcld();
	init_home();
	init_keymaps();

/* OS/2 has some stupid bug and the pipe must be created before socket :-/ */
	if (c_pipe(terminal_pipe)) {
		error("ERROR: can't create pipe for internal communication");
		goto ttt;
	}
	if (!(u = parse_options(ac - 1, av + 1))) goto ttt;
	if (!no_connect && (uh = bind_to_af_unix()) != -1) {
		close(terminal_pipe[0]);
		close(terminal_pipe[1]);
		if (!(info = create_session_info(base_session, u, &len))) goto ttt;
		handle_trm(get_input_handle(), get_output_handle(), uh, uh, get_ctl_handle(), info, len);
		handle_basic_signals(NULL);	/* OK, this is race condition, but it must be so; GPM installs it's own buggy TSTP handler */
		mem_free(info);
		return;
	}
	if ((dds.assume_cp = get_cp_index("ISO-8859-1")) == -1) dds.assume_cp = 0;
	load_config();
	init_b = 1;
	read_bookmarks();
	load_url_history();
	init_cookies();
	u = parse_options(ac - 1, av + 1);
	if (!u) {
		ttt:
		terminate = 1;
		retval = RET_SYNTAX;
		return;
	}
	if (!dmp) {
		if (!((info = create_session_info(base_session, u, &len)) && attach_terminal(get_input_handle(), get_output_handle(), get_ctl_handle(), info, len) != -1)) {
			retval = RET_FATAL;
			terminate = 1;
		}
	} else {
		unsigned char *uu, *wd;
		close(terminal_pipe[0]);
		close(terminal_pipe[1]);
		if (!*u) {
			fprintf(stderr, "URL expected after %s\n.", dmp == D_DUMP ? "-dump" : "-source");
			goto ttt;
		}
		dump_stat.end = end_dump;
		dump_pos = 0;
		if (!(uu = translate_url(u, wd = get_cwd()))) uu = stracpy(u);
		if (load_url(uu, &dump_stat, PRI_MAIN, 0)) goto ttt;
		mem_free(uu);
		if (wd) mem_free(wd);
	}
}

void terminate_all_subsystems()
{
	af_unix_close();
	destroy_all_sessions();
	check_bottom_halves();
	abort_all_downloads();
	check_bottom_halves();
	destroy_all_terminals();
	check_bottom_halves();
	free_all_itrms();
	abort_all_connections();
#ifdef HAVE_SSL
	ssl_finish();
#endif
	shrink_memory(1);
	if (init_b) save_url_history();
	free_history_lists();
	free_term_specs();
	free_types();
	free_auth();
	if (init_b) finalize_bookmarks();
	free_keymaps();
	free_conv_table();
	free_blacklist();
	if (init_b) cleanup_cookies();
	check_bottom_halves();
	end_config();
	free_strerror_buf();
	shutdown_trans();
	terminate_osdep();
}

int main(int argc, char *argv[])
{
	path_to_exe = argv[0];
	ac = argc;
	av = (unsigned char **)argv;

	select_loop(init);
	terminate_all_subsystems();

	check_memory_leaks();
	return retval;
}

void shrink_memory(int u)
{
	shrink_dns_cache(u);
	shrink_format_cache(u);
	garbage_collection(u);
	delete_unused_format_cache_entries();
}
