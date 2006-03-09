#include <machine/trapcodes.h>
#include <machine/x86.h>
#include <kern/hashtable.h>
#include <kern/prof.h>
#include <kern/timer.h>
#include <kern/lib.h>

struct entry {
	uint64_t count ;
	uint64_t time ;
} ;

#define NTRAPS (T_SYSCALL + 1)

struct entry sysc_table[NSYSCALLS] ;
struct entry trap_table[NTRAPS] ;
struct entry user_table[1];

static struct periodic_task timer ;
static int prof_print_enable = 0;

static struct periodic_task timer2 ;

// for profiling using gcc's -finstrument-functions
static int cyg_prof_print_enable = 0 ;
static int cyg_prof_enable = 0 ;

#define NUM_PROFS_PRINTED 2 
static uint64_t cyg_profs_printed[NUM_PROFS_PRINTED] = {
    (uint64_t)&memset,
    (uint64_t)&memcpy,
} ;
static uint64_t cyg_profs_threshold = 1000000000UL;

struct func_stamp
{
    uint64_t func_addr ;
    uint64_t entry_tsc ;
} ;

struct cyg_stack
{
    uint64_t rsp ;
    
    int size ;     
    struct func_stamp func_stamp[PGSIZE] ;       
} ;

static struct
{
    char enable ;

#define NUM_STACKS 16
    struct cyg_stack stack[NUM_STACKS] ;

#define NUM_SYMS 800
        // map from func ptr to index into stats table
    struct hashtable stats_lookup ;
    struct hashentry stats_lookup_back[NUM_SYMS] ;
 
    int stat_size ;
    struct entry stat[NUM_SYMS] ;
        
    uint64_t last_tsc;

} cyg_data ;

void __attribute__((no_instrument_function))
prof_init(void) 
{
	memset(sysc_table, 0, sizeof(sysc_table)) ;
	memset(trap_table, 0, sizeof(trap_table)) ;
	
    memset(&cyg_data, 0, sizeof(cyg_data)) ;
    hash_init(&cyg_data.stats_lookup, cyg_data.stats_lookup_back, NUM_SYMS) ;
        
	timer.pt_fn = &prof_print ;
	timer.pt_interval_ticks = kclock_hz * 10;
	if (prof_print_enable)
		timer_add_periodic(&timer) ;

        timer2.pt_fn = &cyg_profile_print ;
        timer2.pt_interval_ticks = kclock_hz * 10 ;
        if (cyg_prof_print_enable)
                timer_add_periodic(&timer2) ;	
                
        if (cyg_prof_enable)
                cyg_data.enable = 1 ;
}

void 
prof_syscall(syscall_num num, uint64_t time)
{
	assert(num < NSYSCALLS) ;
	sysc_table[num].count++ ;
	sysc_table[num].time += time ;
}

void
prof_trap(int num, uint64_t time)
{
	assert(num < NTRAPS) ;
	trap_table[num].count++ ;
	trap_table[num].time += time ;
}

void
prof_user(uint64_t time)
{
	user_table[0].count++;
	user_table[0].time += time;
}

static void
prof_reset(void)
{
	memset(sysc_table, 0, sizeof(sysc_table));
	memset(trap_table, 0, sizeof(trap_table));
	memset(user_table, 0, sizeof(user_table));
}

static void
print_entry(struct entry *tab, int i, const char *name)
{
	if (tab[i].count)
		cprintf("%3d cnt%12ld tot%12ld avg%12ld %s\n",
			i,
			tab[i].count,
			tab[i].time,
			tab[i].time / tab[i].count,
			name);
}

void
prof_print(void)
{
	cprintf("prof_print: syscalls\n");
	for (int i = 0 ; i < NSYSCALLS ; i++)
		print_entry(&sysc_table[0], i, syscall2s(i));

	cprintf("prof_print: traps\n");
	for (int i = 0 ; i < NTRAPS ; i++)
		print_entry(&trap_table[0], i, "trap");

	cprintf("prof_print: user\n");
	print_entry(&user_table[0], 0, "user");

	prof_reset();
}

//////////////////
// cyg_profile 
//////////////////

static struct cyg_stack* __attribute__ ((no_instrument_function))
stack_for(uint64_t sp)
{
    for (int i = 0 ; i < NUM_STACKS ; i++) {
        if (cyg_data.stack[i].rsp == sp)
            return &cyg_data.stack[i] ;       
    }
    return 0 ;       
}

static struct cyg_stack* __attribute__ ((no_instrument_function))
stack_alloc(uint64_t sp)
{
    for (int i = 0 ; i < NUM_STACKS ; i++) {       
        if (cyg_data.stack[i].size == 0) {
            cyg_data.stack[i].rsp = sp ;
            return &cyg_data.stack[i] ;
        }
    }       
    return 0 ;
}

static void __attribute__((no_instrument_function))
cyg_profile_reset(void)
{
    memset(cyg_data.stat, 0, sizeof(cyg_data.stat)) ;
}

static void __attribute__((no_instrument_function))
cyg_profile_data(void *func_addr, uint64_t time, int count)
{
    uint64_t func = (uint64_t) func_addr ;
    uint64_t val ;
    
    if (hash_get(&cyg_data.stats_lookup, func, &val) < 0) {
        val = cyg_data.stat_size++;
        if (hash_put(&cyg_data.stats_lookup, func, val) < 0)
            return ;
    }
    
    cyg_data.stat[val].count += count ;
    cyg_data.stat[val].time += time ;
    
    return ;
}

void __attribute__ ((no_instrument_function))
cyg_profile_free_stack(uint64_t sp)
{
    if (!cyg_data.enable)
        return ;
    
    sp = ROUNDUP(sp, PGSIZE) ;
    
    struct cyg_stack *s ;
    if ((s = stack_for(sp)) != 0) {
        s->rsp = 0 ;
        s->size = 0 ;
    }
    return ;
}

void __attribute__((no_instrument_function))
cyg_profile_print(void) 
{
    if (!cyg_data.enable)
        return ;
            
    cyg_data.enable = 0 ;
    
    cprintf("cyg_profile_print: selected functions\n");
    for (int i = 0 ; i < NUM_PROFS_PRINTED ; i++) {
        uint64_t val ;
        char buf[32] ;
        if (hash_get(&cyg_data.stats_lookup, cyg_profs_printed[i], &val) == 0) {
            sprintf(buf, "%lx", cyg_profs_printed[i]) ;
            print_entry(cyg_data.stat, val, buf) ;
        }
    }

    cprintf("cyg_profile_print: over-threshold functions\n");
    for (int i = 0; i < NUM_SYMS ; i++) {
	char buf[32];
        uint64_t key = cyg_data.stats_lookup_back[i].key;
        uint64_t val = cyg_data.stats_lookup_back[i].val;
        if (!key)
            continue;
        sprintf(buf, "%lx", key);
	if (cyg_data.stat[val].time > cyg_profs_threshold)
	    print_entry(cyg_data.stat, val, buf);
    }

    cyg_profile_reset() ;
    cyg_data.enable = 1 ;
}

void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *this_fn, void *call_site)
{
    if (!cyg_data.enable)
            return ;
    
    cyg_data.enable = 0 ;
    uint64_t sp = ROUNDUP(read_rsp(), PGSIZE) ;
    
    struct cyg_stack *s ;
    if ((s = stack_for(sp)) == 0)
        assert((s = stack_alloc(sp)) != 0) ; 
        // out of func addr stacks

    uint64_t f = read_tsc();
    if (s->size > 0) {
	uint64_t caller = s->func_stamp[s->size - 1].func_addr;
	cyg_profile_data((void *) caller, f - cyg_data.last_tsc, 0) ;
    }
    cyg_data.last_tsc = f;

    s->func_stamp[s->size].func_addr = (uint64_t) this_fn ;
    s->func_stamp[s->size].entry_tsc = read_tsc() ;
    s->size++ ;
    
    // overflow func addr stack
    assert(s->size != PGSIZE) ;

    cyg_data.enable = 1 ;
}    

void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *this_fn, void *call_site)
{
    if (!cyg_data.enable)
            return ;
    
    uint64_t f = read_tsc() ;
    
    cyg_data.enable = 0 ;
    uint64_t sp = ROUNDUP(read_rsp(), PGSIZE) ;
    
    struct cyg_stack *s = stack_for(sp) ;
    
    while (1) {
        s->size-- ;
        if (s->func_stamp[s->size].func_addr == (uint64_t)this_fn)
                break ;
        // bottom out func addr stack
        assert(s->size != 0) ;        
    }
       
    uint64_t time = f - cyg_data.last_tsc;
    cyg_data.last_tsc = f;
    cyg_profile_data(this_fn, time, 1) ;

    // To account for time spent in a function including all of the
    // children called from there, use this:
    //uint64_t time_with_children = f - s->func_stamp[s->size].entry_tsc;

    cyg_data.enable = 1 ;
}
