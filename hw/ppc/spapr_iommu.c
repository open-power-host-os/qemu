/*
 * QEMU sPAPR IOMMU (TCE) code
 *
 * Copyright (c) 2010 David Gibson, IBM Corporation <dwg@au1.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "sysemu/kvm.h"
#include "hw/qdev.h"
#include "kvm_ppc.h"
#include "sysemu/dma.h"
#include "exec/address-spaces.h"
#include "trace.h"

#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_vio.h"

#include <libfdt.h>

enum sPAPRTCEAccess {
    SPAPR_TCE_FAULT = 0,
    SPAPR_TCE_RO = 1,
    SPAPR_TCE_WO = 2,
    SPAPR_TCE_RW = 3,
};

#define IOMMU_PAGE_SIZE(shift)      (1ULL << (shift))
#define IOMMU_PAGE_MASK(shift)      (~(IOMMU_PAGE_SIZE(shift) - 1))

static QLIST_HEAD(spapr_tce_tables, sPAPRTCETable) spapr_tce_tables;

sPAPRTCETable *spapr_tce_find_by_liobn(target_ulong liobn)
{
    sPAPRTCETable *tcet;

    if (liobn & 0xFFFFFFFF00000000ULL) {
        hcall_dprintf("Request for out-of-bounds LIOBN 0x" TARGET_FMT_lx "\n",
                      liobn);
        return NULL;
    }

    QLIST_FOREACH(tcet, &spapr_tce_tables, list) {
        if (tcet->liobn == (uint32_t)liobn) {
            return tcet;
        }
    }

    return NULL;
}

static IOMMUAccessFlags spapr_tce_iommu_access_flags(uint64_t tce)
{
    switch (tce & SPAPR_TCE_RW) {
    case SPAPR_TCE_FAULT:
        return IOMMU_NONE;
    case SPAPR_TCE_RO:
        return IOMMU_RO;
    case SPAPR_TCE_WO:
        return IOMMU_WO;
    default: /* SPAPR_TCE_RW */
        return IOMMU_RW;
    }
}

static uint64_t *spapr_tce_alloc_table(uint32_t liobn,
                                       uint32_t page_shift,
                                       uint64_t bus_offset,
                                       uint32_t nb_table,
                                       int *fd,
                                       bool need_vfio)
{
    uint64_t *table = NULL;

    if (kvm_enabled()) {
        table = kvmppc_create_spapr_tce(liobn, page_shift, bus_offset, nb_table,
                                        fd, need_vfio);
    }

    if (!table) {
        *fd = -1;
        table = g_malloc0(nb_table * sizeof(uint64_t));
    }

    trace_spapr_iommu_new_table(liobn, table, *fd);

    return table;
}

static void spapr_tce_free_table(uint64_t *table, int fd, uint32_t nb_table)
{
    if (!kvm_enabled() ||
        (kvmppc_remove_spapr_tce(table, fd, nb_table) != 0)) {
        g_free(table);
    }
}

/* Called from RCU critical section */
static IOMMUTLBEntry spapr_tce_translate_iommu(MemoryRegion *iommu, hwaddr addr,
                                               bool is_write)
{
    sPAPRTCETable *tcet = container_of(iommu, sPAPRTCETable, iommu);
    uint64_t tce;
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = 0,
        .translated_addr = 0,
        .addr_mask = ~(hwaddr)0,
        .perm = IOMMU_NONE,
    };

    if ((addr >> tcet->page_shift) < tcet->nb_table) {
        /* Check if we are in bound */
        hwaddr page_mask = IOMMU_PAGE_MASK(tcet->page_shift);

        tce = tcet->table[addr >> tcet->page_shift];
        ret.iova = addr & page_mask;
        ret.translated_addr = tce & page_mask;
        ret.addr_mask = ~page_mask;
        ret.perm = spapr_tce_iommu_access_flags(tce);
    }
    trace_spapr_iommu_xlate(tcet->liobn, addr, ret.iova, ret.perm,
                            ret.addr_mask);

    return ret;
}

static int spapr_tce_vfio_notify(MemoryRegion *iommu, bool attached)
{
    sPAPRTCETable *tcet = container_of(iommu, sPAPRTCETable, iommu);

    spapr_tce_set_need_vfio(tcet, attached);

    return 0;
}

static uint64_t spapr_tce_get_page_sizes(MemoryRegion *iommu)
{
    sPAPRTCETable *tcet = container_of(iommu, sPAPRTCETable, iommu);

    return 1ULL << tcet->page_shift;
}

static uint64_t spapr_tce_get_kvm_id(MemoryRegion *iommu)
{
    sPAPRTCETable *tcet = container_of(iommu, sPAPRTCETable, iommu);

    return tcet->liobn;
}

static void spapr_tce_table_pre_save(void *opaque)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(opaque);

    tcet->migtable = tcet->table;
}

static void spapr_tce_table_do_enable(sPAPRTCETable *tcet, bool vfio_accel);

static int spapr_tce_table_pre_load(void *opaque)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(opaque);

    if (tcet->table) {
        memory_region_del_subregion(&tcet->root, &tcet->iommu);
        spapr_tce_free_table(tcet->table, tcet->fd, tcet->nb_table);
        tcet->table = NULL;
    }

    return 0;
}

static int spapr_tce_table_post_load(void *opaque, int version_id)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(opaque);

    if (tcet->vdev) {
        spapr_vio_set_bypass(tcet->vdev, tcet->bypass);
    }

    if (tcet->enabled) {
        if (!tcet->table) {
            tcet->enabled = false;
            /* VFIO does not migrate so pass vfio_accel == false */
            spapr_tce_table_do_enable(tcet, false);
        }
        memcpy(tcet->table, tcet->migtable,
               tcet->nb_table * sizeof(tcet->table[0]));
        free(tcet->migtable);
        tcet->migtable = NULL;
    }

    return 0;
}

static bool version_before_3(void *opaque, int version_id)
{
    return version_id < 3;
}

static const VMStateDescription vmstate_spapr_tce_table = {
    .name = "spapr_iommu",
    .version_id = 3,
    .minimum_version_id = 2,
    .pre_save = spapr_tce_table_pre_save,
    .pre_load = spapr_tce_table_pre_load,
    .post_load = spapr_tce_table_post_load,
    .fields      = (VMStateField []) {
        /* Sanity check */
        VMSTATE_UINT32_EQUAL(liobn, sPAPRTCETable),
        VMSTATE_UNUSED_TEST(version_before_3, sizeof(uint32_t)),

        /* IOMMU state */
        VMSTATE_BOOL_V(enabled, sPAPRTCETable, 3),
        VMSTATE_UINT64_V(bus_offset, sPAPRTCETable, 3),
        VMSTATE_UINT32_V(page_shift, sPAPRTCETable, 3),
        VMSTATE_UINT32(nb_table, sPAPRTCETable),
        VMSTATE_BOOL(bypass, sPAPRTCETable),
        VMSTATE_VARRAY_UINT32_ALLOC(migtable, sPAPRTCETable, nb_table, 0,
                                    vmstate_info_uint64, uint64_t),

        VMSTATE_END_OF_LIST()
    },
};

static MemoryRegionIOMMUOps spapr_iommu_ops = {
    .translate = spapr_tce_translate_iommu,
    .vfio_notify = spapr_tce_vfio_notify,
    .get_page_sizes = spapr_tce_get_page_sizes,
    .get_kvm_id = spapr_tce_get_kvm_id,
};

static int spapr_tce_table_realize(DeviceState *dev)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(dev);
    Object *tcetobj = OBJECT(tcet);
    char tmp[32];

    tcet->fd = -1;
    snprintf(tmp, sizeof(tmp), "tce-root-%x", tcet->liobn);
    memory_region_init(&tcet->root, tcetobj, tmp, UINT64_MAX);

    snprintf(tmp, sizeof(tmp), "tce-iommu-%x", tcet->liobn);
    memory_region_init_iommu(&tcet->iommu, tcetobj, &spapr_iommu_ops, tmp, 0);

    QLIST_INSERT_HEAD(&spapr_tce_tables, tcet, list);

    vmstate_register(DEVICE(tcet), tcet->liobn, &vmstate_spapr_tce_table,
                     tcet);

    return 0;
}

void spapr_tce_set_need_vfio(sPAPRTCETable *tcet, bool need_vfio)
{
    size_t table_size = tcet->nb_table * sizeof(uint64_t);
    uint64_t *oldtable;
    int newfd = -1;
    bool tcet_can_vfio = (tcet->fd < 0) || kvmppc_has_cap_spapr_vfio();

    if (need_vfio == tcet_can_vfio) {
        /* Nothing to do */
        return;
    }

    oldtable = tcet->table;

    tcet->table = spapr_tce_alloc_table(tcet->liobn,
                                        tcet->page_shift,
                                        tcet->bus_offset,
                                        tcet->nb_table,
                                        &newfd,
                                        need_vfio);
    memcpy(tcet->table, oldtable, table_size);

    kvmppc_remove_spapr_tce(oldtable, tcet->fd, tcet->nb_table);

    tcet->fd = newfd;
}

sPAPRTCETable *spapr_tce_new_table(DeviceState *owner, uint32_t liobn)
{
    sPAPRTCETable *tcet;
    char tmp[32];

    if (spapr_tce_find_by_liobn(liobn)) {
        fprintf(stderr, "Attempted to create TCE table with duplicate"
                " LIOBN 0x%x\n", liobn);
        return NULL;
    }

    tcet = SPAPR_TCE_TABLE(object_new(TYPE_SPAPR_TCE_TABLE));
    tcet->liobn = liobn;
    tcet->owner = owner;

    snprintf(tmp, sizeof(tmp), "tce-table-%x", liobn);
    object_property_add_child(OBJECT(owner), tmp, OBJECT(tcet), NULL);

    object_property_set_bool(OBJECT(tcet), true, "realized", NULL);

    return tcet;
}

static void spapr_tce_table_do_enable(sPAPRTCETable *tcet, bool need_vfio)
{
    if (!tcet->nb_table) {
        return;
    }

    tcet->table = spapr_tce_alloc_table(tcet->liobn,
                                        tcet->page_shift,
                                        tcet->bus_offset,
                                        tcet->nb_table,
                                        &tcet->fd,
                                        need_vfio);

    memory_region_set_size(&tcet->iommu,
                           (uint64_t)tcet->nb_table << tcet->page_shift);
    memory_region_add_subregion(&tcet->root, tcet->bus_offset, &tcet->iommu);

    tcet->enabled = true;
}

void spapr_tce_table_enable(sPAPRTCETable *tcet,
                            uint32_t page_shift, uint64_t bus_offset,
                            uint32_t nb_table, bool need_vfio)
{
    if (tcet->enabled) {
        return;
    }

    tcet->bus_offset = bus_offset;
    tcet->page_shift = page_shift;
    tcet->nb_table = nb_table;

    spapr_tce_table_do_enable(tcet, need_vfio);
}

void spapr_tce_table_disable(sPAPRTCETable *tcet)
{
    if (!tcet->enabled) {
        return;
    }

    memory_region_del_subregion(&tcet->root, &tcet->iommu);
    memory_region_set_size(&tcet->iommu, 0);

    spapr_tce_free_table(tcet->table, tcet->fd, tcet->nb_table);
    tcet->fd = -1;
    tcet->table = NULL;
    tcet->enabled = false;
    tcet->bus_offset = 0;
    tcet->page_shift = 0;
    tcet->nb_table = 0;
}

static void spapr_tce_table_unrealize(DeviceState *dev, Error **errp)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(dev);

    QLIST_REMOVE(tcet, list);

    spapr_tce_table_disable(tcet);
}

MemoryRegion *spapr_tce_get_iommu(sPAPRTCETable *tcet)
{
    return &tcet->root;
}

static void spapr_tce_reset(DeviceState *dev)
{
    sPAPRTCETable *tcet = SPAPR_TCE_TABLE(dev);
    size_t table_size = tcet->nb_table * sizeof(uint64_t);

    memset(tcet->table, 0, table_size);
}

static target_ulong put_tce_emu(sPAPRTCETable *tcet, target_ulong ioba,
                                target_ulong tce)
{
    IOMMUTLBEntry entry;
    hwaddr page_mask = IOMMU_PAGE_MASK(tcet->page_shift);
    unsigned long index = (ioba - tcet->bus_offset) >> tcet->page_shift;

    if (index >= tcet->nb_table) {
        hcall_dprintf("spapr_vio_put_tce on out-of-bounds IOBA 0x"
                      TARGET_FMT_lx "\n", ioba);
        return H_PARAMETER;
    }

    tcet->table[index] = tce;

    entry.target_as = &address_space_memory,
    entry.iova = (ioba - tcet->bus_offset) & page_mask;
    entry.translated_addr = tce & page_mask;
    entry.addr_mask = ~page_mask;
    entry.perm = spapr_tce_iommu_access_flags(tce);
    memory_region_notify_iommu(&tcet->iommu, entry);

    return H_SUCCESS;
}

static target_ulong h_put_tce_indirect(PowerPCCPU *cpu,
                                       sPAPRMachineState *spapr,
                                       target_ulong opcode, target_ulong *args)
{
    int i;
    target_ulong liobn = args[0];
    target_ulong ioba = args[1];
    target_ulong ioba1 = ioba;
    target_ulong tce_list = args[2];
    target_ulong npages = args[3];
    target_ulong ret = H_PARAMETER, tce = 0;
    sPAPRTCETable *tcet = spapr_tce_find_by_liobn(liobn);
    CPUState *cs = CPU(cpu);
    hwaddr page_mask, page_size;

    if (!tcet) {
        return H_PARAMETER;
    }

    if ((npages > 512) || (tce_list & SPAPR_TCE_PAGE_MASK)) {
        return H_PARAMETER;
    }

    page_mask = IOMMU_PAGE_MASK(tcet->page_shift);
    page_size = IOMMU_PAGE_SIZE(tcet->page_shift);
    ioba &= page_mask;

    for (i = 0; i < npages; ++i, ioba += page_size) {
        tce = ldq_be_phys(cs->as, tce_list + i * sizeof(target_ulong));

        ret = put_tce_emu(tcet, ioba, tce);
        if (ret) {
            break;
        }
    }

    /* Trace last successful or the first problematic entry */
    i = i ? (i - 1) : 0;
    if (SPAPR_IS_PCI_LIOBN(liobn)) {
        trace_spapr_iommu_pci_indirect(liobn, ioba1, tce_list, i, tce, ret);
    } else {
        trace_spapr_iommu_indirect(liobn, ioba1, tce_list, i, tce, ret);
    }
    return ret;
}

static target_ulong h_stuff_tce(PowerPCCPU *cpu, sPAPRMachineState *spapr,
                              target_ulong opcode, target_ulong *args)
{
    int i;
    target_ulong liobn = args[0];
    target_ulong ioba = args[1];
    target_ulong tce_value = args[2];
    target_ulong npages = args[3];
    target_ulong ret = H_PARAMETER;
    sPAPRTCETable *tcet = spapr_tce_find_by_liobn(liobn);
    hwaddr page_mask, page_size;

    if (!tcet) {
        return H_PARAMETER;
    }

    if (npages > tcet->nb_table) {
        return H_PARAMETER;
    }

    page_mask = IOMMU_PAGE_MASK(tcet->page_shift);
    page_size = IOMMU_PAGE_SIZE(tcet->page_shift);
    ioba &= page_mask;

    for (i = 0; i < npages; ++i, ioba += page_size) {
        ret = put_tce_emu(tcet, ioba, tce_value);
        if (ret) {
            break;
        }
    }
    if (SPAPR_IS_PCI_LIOBN(liobn)) {
        trace_spapr_iommu_pci_stuff(liobn, ioba, tce_value, npages, ret);
    } else {
        trace_spapr_iommu_stuff(liobn, ioba, tce_value, npages, ret);
    }

    return ret;
}

static target_ulong h_put_tce(PowerPCCPU *cpu, sPAPRMachineState *spapr,
                              target_ulong opcode, target_ulong *args)
{
    target_ulong liobn = args[0];
    target_ulong ioba = args[1];
    target_ulong tce = args[2];
    target_ulong ret = H_PARAMETER;
    sPAPRTCETable *tcet = spapr_tce_find_by_liobn(liobn);

    if (tcet) {
        hwaddr page_mask = IOMMU_PAGE_MASK(tcet->page_shift);

        ioba &= page_mask;

        ret = put_tce_emu(tcet, ioba, tce);
    }
    if (SPAPR_IS_PCI_LIOBN(liobn)) {
        trace_spapr_iommu_pci_put(liobn, ioba, tce, ret);
    } else {
        trace_spapr_iommu_put(liobn, ioba, tce, ret);
    }

    return ret;
}

static target_ulong get_tce_emu(sPAPRTCETable *tcet, target_ulong ioba,
                                target_ulong *tce)
{
    unsigned long index = (ioba - tcet->bus_offset) >> tcet->page_shift;

    if (index >= tcet->nb_table) {
        hcall_dprintf("spapr_iommu_get_tce on out-of-bounds IOBA 0x"
                      TARGET_FMT_lx "\n", ioba);
        return H_PARAMETER;
    }

    *tce = tcet->table[index];

    return H_SUCCESS;
}

static target_ulong h_get_tce(PowerPCCPU *cpu, sPAPRMachineState *spapr,
                              target_ulong opcode, target_ulong *args)
{
    target_ulong liobn = args[0];
    target_ulong ioba = args[1];
    target_ulong tce = 0;
    target_ulong ret = H_PARAMETER;
    sPAPRTCETable *tcet = spapr_tce_find_by_liobn(liobn);

    if (tcet) {
        hwaddr page_mask = IOMMU_PAGE_MASK(tcet->page_shift);

        ioba &= page_mask;

        ret = get_tce_emu(tcet, ioba, &tce);
        if (!ret) {
            args[0] = tce;
        }
    }
    if (SPAPR_IS_PCI_LIOBN(liobn)) {
        trace_spapr_iommu_pci_get(liobn, ioba, ret, tce);
    } else {
        trace_spapr_iommu_get(liobn, ioba, ret, tce);
    }

    return ret;
}

int spapr_dma_dt(void *fdt, int node_off, const char *propname,
                 uint32_t liobn, uint64_t window, uint32_t size)
{
    uint32_t dma_prop[5];
    int ret;

    dma_prop[0] = cpu_to_be32(liobn);
    dma_prop[1] = cpu_to_be32(window >> 32);
    dma_prop[2] = cpu_to_be32(window & 0xFFFFFFFF);
    dma_prop[3] = 0; /* window size is 32 bits */
    dma_prop[4] = cpu_to_be32(size);

    ret = fdt_setprop_cell(fdt, node_off, "ibm,#dma-address-cells", 2);
    if (ret < 0) {
        return ret;
    }

    ret = fdt_setprop_cell(fdt, node_off, "ibm,#dma-size-cells", 2);
    if (ret < 0) {
        return ret;
    }

    ret = fdt_setprop(fdt, node_off, propname, dma_prop, sizeof(dma_prop));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int spapr_tcet_dma_dt(void *fdt, int node_off, const char *propname,
                      sPAPRTCETable *tcet)
{
    if (!tcet) {
        return 0;
    }

    return spapr_dma_dt(fdt, node_off, propname,
                        tcet->liobn, 0, tcet->nb_table << tcet->page_shift);
}

static void spapr_tce_table_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->init = spapr_tce_table_realize;
    dc->reset = spapr_tce_reset;
    dc->unrealize = spapr_tce_table_unrealize;

    QLIST_INIT(&spapr_tce_tables);

    /* hcall-tce */
    spapr_register_hypercall(H_PUT_TCE, h_put_tce);
    spapr_register_hypercall(H_GET_TCE, h_get_tce);
    spapr_register_hypercall(H_PUT_TCE_INDIRECT, h_put_tce_indirect);
    spapr_register_hypercall(H_STUFF_TCE, h_stuff_tce);
}

static TypeInfo spapr_tce_table_info = {
    .name = TYPE_SPAPR_TCE_TABLE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(sPAPRTCETable),
    .class_init = spapr_tce_table_class_init,
};

static void register_types(void)
{
    type_register_static(&spapr_tce_table_info);
}

type_init(register_types);
