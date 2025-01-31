/**********************************************************************
 * Copyright (c) 2020-2024
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "list_head.h"
#include "vm.h"

/**
 * Ready queue of the system
 */
extern struct list_head processes;

/**
 * Currently running process
 */
extern struct process *current;

/**
 * Page Table Base Register that MMU will walk through for address translation
 */
extern struct pagetable *ptbr;

/**
 * TLB of the system.
 */
extern struct tlb_entry tlb[];


/**
 * The number of mappings for each page frame. Can be used to determine how
 * many processes are using the page frames.
 */
extern unsigned int mapcounts[];


/**
 * lookup_tlb(@vpn, @rw, @pfn)
 *
 * DESCRIPTION
 *   Translate @vpn of the current process through TLB. DO NOT make your own
 *   data structure for TLB, but should use the defined @tlb data structure
 *   to translate. If the requested VPN exists in the TLB and it has the same
 *   rw flag, return true with @pfn is set to its PFN. Otherwise, return false.
 *   The framework calls this function when needed, so do not call
 *   this function manually.
 *
 * RETURN
 *   Return true if the translation is cached in the TLB.
 *   Return false otherwise
 */
bool lookup_tlb(unsigned int vpn, unsigned int rw, unsigned int *pfn) {
    for (int i = 0; i < NR_TLB_ENTRIES; i++) {
        if (tlb[i].vpn == vpn && tlb[i].rw & rw) {
            *pfn = tlb[i].pfn;
            return true;
        }
    }
    return false;
}


/**
 * insert_tlb(@vpn, @rw, @pfn)
 *
 * DESCRIPTION
 *   Insert the mapping from @vpn to @pfn for @rw into the TLB. The framework will
 *   call this function when required, so no need to call this function manually.
 *   Note that if there exists an entry for @vpn already, just update it accordingly
 *   rather than removing it or creating a new entry.
 *   Also, in the current simulator, TLB is big enough to cache all the entries of
 *   the current page table, so don't worry about TLB entry eviction. ;-)
 */
void insert_tlb(unsigned int vpn, unsigned int rw, unsigned int pfn) {
    int idx = -1;
//    fprintf(stderr, "current : %d\n", current->pid);
    for (int i = 0; i < NR_TLB_ENTRIES; i++) {
//        fprintf(stderr, "tlb[%d] :  %d %d\n", i, tlb[i].vpn, tlb[i].pfn);

        if (tlb[i].valid == false && idx == -1) {
            idx = i;
            break;
        }
        if (tlb[i].vpn == vpn) {
//            fprintf(stderr, "%d %d %d %d\n",i,vpn,pfn,tlb[i].pfn);
            tlb[i].valid = true;
            tlb[i].rw = rw;
            tlb[i].pfn = pfn;
            return;
        }
    }
    if (idx != -1) {
//        fprintf(stderr, "insert : %d %d %d\n",idx,vpn,pfn);

        tlb[idx].valid = true;
        tlb[idx].pfn = pfn;
        tlb[idx].rw = rw;
        tlb[idx].vpn = vpn;
    }
}


/**
 * alloc_page(@vpn, @rw)
 *
 * DESCRIPTION
 *   Allocate a page frame that is not allocated to any process, and map it
 *   to @vpn. When the system has multiple free pages, this function should
 *   allocate the page frame with the **smallest pfn**.
 *   You may construct the page table of the @current process. When the page
 *   is allocated with ACCESS_WRITE flag, the page may be later accessed for writes.
 *   However, the pages populated with ACCESS_READ should not be accessible with
 *   ACCESS_WRITE accesses.
 *
 * RETURN
 *   Return allocated page frame number.
 *   Return -1 if all page frames are allocated.
 */
unsigned int alloc_page(unsigned int vpn, unsigned int rw) {
    for (unsigned int pfn = 0; pfn < NR_PAGEFRAMES; pfn++) {
        if (mapcounts[pfn] == 0) {
            mapcounts[pfn] = 1;

            int page_dir_idx = vpn / NR_PTES_PER_PAGE;
            int page_entry_idx = vpn - page_dir_idx * NR_PTES_PER_PAGE;

            struct pte_directory *pd = current->pagetable.pdes[page_dir_idx];

            if (!pd) {
                pd = (struct pte_directory *) malloc(sizeof(struct pte_directory));
                current->pagetable.pdes[page_dir_idx] = pd;
            }

            struct pte *pte = &pd->ptes[page_entry_idx];
            pte->valid = true;
            pte->rw = rw;
            pte->pfn = pfn;
            return pfn;
        }
    }
    return (unsigned int) -1;
}


/**
 * free_page(@vpn)
 *
 * DESCRIPTION
 *   Deallocate the page from the current processor. Make sure that the fields
 *   for the corresponding PTE (valid, rw, pfn) is set @false or 0.
 *   Also, consider the case when a page is shared by two processes,
 *   and one process is about to free the page. Also, think about TLB as well ;-)
 */
void free_page(unsigned int vpn) {
    int page_dir_idx = vpn / NR_PTES_PER_PAGE;
    int page_entry_idx = vpn - page_dir_idx * NR_PTES_PER_PAGE;

    struct pte_directory *pd = current->pagetable.pdes[page_dir_idx];
    if (pd) {
        struct pte *pte = &pd->ptes[page_entry_idx];
        if (pte->valid) {
            mapcounts[pte->pfn]--;
            for (int i = 0; i < NR_TLB_ENTRIES; i++) {
                if (tlb[i].valid && tlb[i].pfn == pte->pfn) {
                    tlb[i].valid = false;
                    tlb[i].rw = 0;
                    tlb[i].pfn = 0;
                }
            }
            pte->valid = false;
            pte->rw = 0;
            pte->pfn = 0;
        }
    }


}




/**
 * handle_page_fault()
 *
 * DESCRIPTION
 *   Handle the page fault for accessing @vpn for @rw. This function is called
 *   by the framework when the __translate() for @vpn fails. This implies;
 *   0. page directory is invalid
 *   1. pte is invalid
 *   2. pte is not writable but @rw is for write
 *   This function should identify the situation, and do the copy-on-write if
 *   necessary.
 *
 * RETURN
 *   @true on successful fault handling
 *   @false otherwise
 */
#include "vm.h"

bool handle_page_fault(unsigned int vpn, unsigned int rw) {
    int page_dir_idx = vpn / NR_PTES_PER_PAGE;
    int page_entry_idx = vpn - page_dir_idx * NR_PTES_PER_PAGE;

    struct pte_directory *pd = current->pagetable.pdes[page_dir_idx];
    struct pte *pte;

    if (!pd) {
        pd = malloc(sizeof(struct pte_directory));
        memset(pd, 0, sizeof(struct pte_directory));
        current->pagetable.pdes[page_dir_idx] = pd;
    }

    pte = &pd->ptes[page_entry_idx];

    if (!pte->valid) {
        if (alloc_page(vpn, rw) == (unsigned int) -1) return false;
        return true;
    }

    if (pte->rw != rw && rw & ACCESS_WRITE) {
        if (pte->rw & ACCESS_READ && pte->private & ACCESS_WRITE) {
            if (mapcounts[pte->pfn] > 1) {
//                    fprintf(stderr, "private : %d\n",pte->private);
                unsigned int old_pfn = pte->pfn;
                unsigned int new_pfn = alloc_page(vpn, 3);
                mapcounts[old_pfn]--;
                pte->rw |= ACCESS_WRITE;
                pte->pfn = new_pfn;
                return true;
            } else {
                pte->rw |= ACCESS_WRITE;
                return true;

            }
        }
        return false;
    }

    return true;
}


/**
 * switch_process()
 *
 * DESCRIPTION
 *   If there is a process with @pid in @processes, switch to the process.
 *   The @current process at the moment should be put into the @processes
 *   list, and @current should be replaced to the requested process.
 *   Make sure that the next process is unlinked from the @processes, and
 *   @ptbr is set properly.
 *
 *   If there is no process with @pid in the @processes list, fork a process
 *   from the @current. This implies the forked child process should have
 *   the identical page table entry 'values' to its parent's (i.e., @current)
 *   page table. 
 *   To implement the copy-on-write feature, you should manipulate the writable
 *   bit in PTE and mapcounts for shared pages. You may use pte->private for 
 *   storing some useful information :-)
 */
void switch_process(unsigned int pid) {
    struct process *next = NULL;
    struct process *p = NULL;
    list_for_each_entry(p, &processes, list) {
        if (p->pid == pid) {
            next = p;
            break;
        }
    }

    if (!next) {
        next = malloc(sizeof(struct process));
        memset(&next->pagetable, 0, sizeof(struct pagetable));
        next->pid = pid;
        INIT_LIST_HEAD(&next->list);

        for (int i = 0; i < NR_PDES_PER_PAGE; i++) {
            if (current->pagetable.pdes[i] != NULL) {
                next->pagetable.pdes[i] = malloc(sizeof(struct pte_directory));
                memset(next->pagetable.pdes[i], 0, sizeof(struct pte_directory));

                for (int j = 0; j < NR_PTES_PER_PAGE; j++) {
                    next->pagetable.pdes[i]->ptes[j] = current->pagetable.pdes[i]->ptes[j];
                    if (current->pagetable.pdes[i]->ptes[j].valid) {
                        next->pagetable.pdes[i]->ptes[j].valid = true;
//                        if (next->pagetable.pdes[i]->ptes[j].rw & ACCESS_WRITE) {
//                            next->pagetable.pdes[i]->ptes[j].rw &= ~ACCESS_WRITE;
//                        }
                        if (!current->pagetable.pdes[i]->ptes[j].private) {
                            current->pagetable.pdes[i]->ptes[j].private = current->pagetable.pdes[i]->ptes[j].rw;
                            next->pagetable.pdes[i]->ptes[j].private = current->pagetable.pdes[i]->ptes[j].rw;
                        }
                        current->pagetable.pdes[i]->ptes[j].rw = ACCESS_READ;
                        next->pagetable.pdes[i]->ptes[j].rw = ACCESS_READ;
                        next->pagetable.pdes[i]->ptes[j].pfn = current->pagetable.pdes[i]->ptes[j].pfn;
                        mapcounts[next->pagetable.pdes[i]->ptes[j].pfn]++;
//                        fprintf(stderr, "%d %d %d %d\n",current->pid,j,current->pagetable.pdes[i]->ptes[j].private,next->pagetable.pdes[i]->ptes[j].private);
                    }
                }
            }
        }

    } else {
        list_del_init(&next->list);
    }
//    memset(tlb,0,sizeof(struct tlb_entry));
    for (int i = 0; i < NR_TLB_ENTRIES; i++) {
        if (tlb[i].valid == false) {
            break;
        }
        tlb[i].valid = false;
        tlb[i].rw = 0;
        tlb[i].vpn = 0;
        tlb[i].pfn = 0;
        tlb[i].private = 0;
    }
    list_add_tail(&current->list, &processes);
    current = next;
    ptbr = &current->pagetable;
}



