#include <dev/ahci.h>
#include <dev/disk.h>
#include <dev/pcireg.h>
#include <dev/ahcireg.h>
#include <kern/arch.h>
#include <kern/timer.h>
#include <inc/error.h>

struct ahci_hba {
    uint32_t irq;
    uint32_t membase;
    uint32_t ncs;			/* # control slots */
    volatile struct ahci_reg *r;

    struct ahci_recv_fis *rfis[32];
    struct ahci_cmd_header *cmd[32];
};

/*
 * Helper functions
 */

static uint32_t
    __attribute__((unused))
ahci_build_prd(struct ahci_hba *a, uint32_t port, uint32_t cslot,
	       struct kiovec *iov_buf, uint32_t iov_cnt)
{
    uint32_t nbytes = 0;

    struct ahci_cmd_table *cmd = pa2kva(a->cmd[port][cslot].ctba);
    assert(iov_cnt < sizeof(cmd->prdt) / sizeof(cmd->prdt[0]));

    for (uint32_t slot = 0; slot < iov_cnt; slot++) {
	cmd->prdt[slot].dba = kva2pa(iov_buf[slot].iov_base);
	cmd->prdt[slot].dbc = iov_buf[slot].iov_len - 1;
	nbytes += iov_buf[slot].iov_len;
    }

    a->cmd[port][cslot].prdtl = iov_cnt;
    return nbytes;
}

/*
 * Memory allocation.
 */

struct ahci_malloc_state {
    void *p;
    uint32_t off;
};

static void *
ahci_malloc(struct ahci_malloc_state *ms, uint32_t bytes)
{
    assert(bytes <= PGSIZE);

    if (!ms->p || bytes > PGSIZE - ms->off) {
	int r = page_alloc(&ms->p);
	if (r < 0)
	    return 0;
	ms->off = 0;
    }

    void *m = ms->p + ms->off;
    ms->off += bytes;
    return m;
}

static int
ahci_alloc(struct ahci_hba **ap)
{
    struct ahci_malloc_state ms;
    memset(&ms, 0, sizeof(ms));

    struct ahci_hba *a = ahci_malloc(&ms, sizeof(*a));
    if (!a)
	return -E_NO_MEM;

    for (int i = 0; i < 32; i++) {
	a->rfis[i] = ahci_malloc(&ms, sizeof(struct ahci_recv_fis));
	a->cmd[i] = ahci_malloc(&ms, sizeof(struct ahci_cmd_header) * 32);
	if (!a->rfis[i] || !a->cmd[i])
	    return -E_NO_MEM;

	for (int j = 0; j < 32; j++) {
	    struct ahci_cmd_table *ctab = ahci_malloc(&ms, sizeof(*ctab));
	    if (!ctab)
		return -E_NO_MEM;

	    a->cmd[i][j].ctba = kva2pa(ctab);
	}
    }

    *ap = a;
    return 0;
}

/*
 * Initialization.
 */

static void
ahci_reset_port(struct ahci_hba *a, uint32_t port)
{
    cprintf("ahci_reset_port: %d\n", port);
}

static void
ahci_reset(struct ahci_hba *a)
{
    a->r->ghc |= AHCI_GHC_AE;
    a->r->ghc |= AHCI_GHC_HR;
    while (a->r->ghc & AHCI_GHC_HR)
	timer_delay(1000);

    a->r->ghc |= AHCI_GHC_AE;
    a->ncs = AHCI_CAP_NCS(a->r->cap);

    for (uint32_t i = 0; i < 32; i++) {
	a->r->port[i].clb = kva2pa(a->cmd[i]);
	a->r->port[i].fb = kva2pa(a->rfis[i]);
	if (a->r->pi & (1 << i))
	    ahci_reset_port(a, i);
    }

    a->r->ghc |= AHCI_GHC_IE;
}

int
ahci_init(struct pci_func *f)
{
    if (PCI_INTERFACE(f->dev_class) != 0x01) {
	cprintf("ahci_init: not an AHCI controller\n");
	return 0;
    }

    struct ahci_hba *a;
    int r = ahci_alloc(&a);
    if (r < 0)
	return r;

    pci_func_enable(f);
    a->irq = f->irq_line;
    a->membase = f->reg_base[5];
    a->r = pa2kva(a->membase);

    ahci_reset(a);

    cprintf("AHCI: base 0x%x, irq %d, v 0x%x\n",
	    a->membase, a->irq, a->r->vs);
    return 1;
}
