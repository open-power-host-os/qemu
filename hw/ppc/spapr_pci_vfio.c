/*
 * QEMU sPAPR PCI host for VFIO
 *
 * Copyright (c) 2011-2014 Alexey Kardashevskiy, IBM Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License,
 *  or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/ppc/spapr.h"
#include "hw/pci-host/spapr.h"
#include "linux/vfio.h"
#include "hw/misc/vfio.h"
#include "dirent.h"

static Property spapr_phb_vfio_properties[] = {
    DEFINE_PROP_INT32("iommu", sPAPRPHBVFIOState, iommugroupid, -1),
    DEFINE_PROP_BOOL("scan", sPAPRPHBVFIOState, scan, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void spapr_pci_vfio_scan(sPAPRPHBVFIOState *svphb, Error **errp)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(svphb);
    char *iommupath;
    DIR *dirp;
    struct dirent *entry;
    Error *error = NULL;

    iommupath = g_strdup_printf("/sys/kernel/iommu_groups/%d/devices/",
                                svphb->iommugroupid);
    if (!iommupath) {
        return;
    }

    dirp = opendir(iommupath);
    if (!dirp) {
        error_setg_errno(errp, -errno, "spapr-vfio: vfio scan failed on opendir");
        g_free(iommupath);
        return;
    }

    while ((entry = readdir(dirp)) != NULL) {
        Error *err = NULL;
        char *tmp;
        FILE *deviceclassfile;
        unsigned deviceclass = 0, domainid, busid, devid, fnid;
        DeviceState *dev;

        if (sscanf(entry->d_name, "%X:%X:%X.%x",
                   &domainid, &busid, &devid, &fnid) != 4) {
            continue;
        }

        tmp = g_strdup_printf("%s%s/class", iommupath, entry->d_name);

        deviceclassfile = fopen(tmp, "r");
        if (deviceclassfile) {
            int ret = fscanf(deviceclassfile, "%x", &deviceclass);
            fclose(deviceclassfile);
            if (ret != 1) {
                continue;
            }
        }
        g_free(tmp);

        if (!deviceclass) {
            continue;
        }
        if ((deviceclass >> 16) == (PCI_CLASS_BRIDGE_OTHER >> 8)) {
            /* Skip bridges */
            continue;
        }

        dev = qdev_create(BUS(phb->bus), "vfio-pci");
        if (!dev) {
            continue;
        }

        object_property_parse(OBJECT(dev), entry->d_name, "host", &err);
        if (err != NULL) {
            continue;
        }
        object_property_set_bool(OBJECT(dev), true, "realized", &error);
        if (error) {
            error_propagate(errp, error);
            break;
        }
    }
    closedir(dirp);
    g_free(iommupath);
}

static void spapr_phb_vfio_finish_realize(sPAPRPHBState *sphb, Error **errp)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    struct vfio_iommu_spapr_tce_info info = { .argsz = sizeof(info) };
    int ret;
    sPAPRTCETable *tcet;
    uint32_t liobn = svphb->phb.dma_liobn;

    if (svphb->iommugroupid == -1) {
        error_setg(errp, "Wrong IOMMU group ID %d", svphb->iommugroupid);
        return;
    }

    ret = vfio_container_ioctl(&svphb->phb.iommu_as, svphb->iommugroupid,
                               VFIO_CHECK_EXTENSION,
                               (void *) VFIO_SPAPR_TCE_IOMMU);
    if (ret != 1) {
        error_setg_errno(errp, -ret,
                         "spapr-vfio: SPAPR extension is not supported");
        return;
    }

    ret = vfio_container_ioctl(&svphb->phb.iommu_as, svphb->iommugroupid,
                               VFIO_IOMMU_SPAPR_TCE_GET_INFO, &info);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "spapr-vfio: get info from container failed");
        return;
    }

    tcet = spapr_tce_new_table(DEVICE(sphb), liobn, info.dma32_window_start,
                               SPAPR_TCE_PAGE_SHIFT,
                               info.dma32_window_size >> SPAPR_TCE_PAGE_SHIFT,
                               true);
    if (!tcet) {
        error_setg(errp, "spapr-vfio: failed to create VFIO TCE table");
        return;
    }

    /* Register default 32bit DMA window */
    memory_region_add_subregion(&sphb->iommu_root, tcet->bus_offset,
                                spapr_tce_get_iommu(tcet));

    if (svphb->scan) {
        Error *error = NULL;
        spapr_pci_vfio_scan(svphb, &error);
        if (error) {
            error_propagate(errp, error);
        }
    }
}

static int spapr_phb_vfio_eeh_handler(sPAPRPHBState *sphb, int req, int opt)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    struct vfio_eeh_pe_op op = { .argsz = sizeof(op) };
    int cmd;

    switch (req) {
    case RTAS_EEH_REQ_SET_OPTION:
        switch (opt) {
        case RTAS_EEH_DISABLE:
            cmd = VFIO_EEH_PE_DISABLE;
            break;
        case RTAS_EEH_ENABLE:
            cmd = VFIO_EEH_PE_ENABLE;
            break;
        case RTAS_EEH_THAW_IO:
            cmd = VFIO_EEH_PE_UNFREEZE_IO;
            break;
        case RTAS_EEH_THAW_DMA:
            cmd = VFIO_EEH_PE_UNFREEZE_DMA;
            break;
        default:
            return -EINVAL;
        }
        break;
    case RTAS_EEH_REQ_GET_STATE:
        cmd = VFIO_EEH_PE_GET_STATE;
        break;
    case RTAS_EEH_REQ_RESET:
        switch (opt) {
        case RTAS_SLOT_RESET_DEACTIVATE:
            cmd = VFIO_EEH_PE_RESET_DEACTIVATE;
            break;
        case RTAS_SLOT_RESET_HOT:
            cmd = VFIO_EEH_PE_RESET_HOT;
            break;
        case RTAS_SLOT_RESET_FUNDAMENTAL:
            cmd = VFIO_EEH_PE_RESET_FUNDAMENTAL;
            break;
        default:
            return -EINVAL;
        }
        break;
    case RTAS_EEH_REQ_CONFIGURE:
        cmd = VFIO_EEH_PE_CONFIGURE;
        break;
    default:
         return -EINVAL;
    }

    op.op = cmd;
    return vfio_container_ioctl(&svphb->phb.iommu_as, svphb->iommugroupid,
                                VFIO_EEH_PE_OP, &op);
}

static void spapr_phb_vfio_reset(DeviceState *qdev)
{
    /* Do nothing */
}

static void spapr_phb_vfio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    sPAPRPHBClass *spc = SPAPR_PCI_HOST_BRIDGE_CLASS(klass);

    dc->props = spapr_phb_vfio_properties;
    dc->reset = spapr_phb_vfio_reset;
    spc->finish_realize = spapr_phb_vfio_finish_realize;
    spc->eeh_handler = spapr_phb_vfio_eeh_handler;
}

static const TypeInfo spapr_phb_vfio_info = {
    .name          = TYPE_SPAPR_PCI_VFIO_HOST_BRIDGE,
    .parent        = TYPE_SPAPR_PCI_HOST_BRIDGE,
    .instance_size = sizeof(sPAPRPHBVFIOState),
    .class_init    = spapr_phb_vfio_class_init,
    .class_size    = sizeof(sPAPRPHBClass),
};

static void spapr_pci_vfio_register_types(void)
{
    type_register_static(&spapr_phb_vfio_info);
}

type_init(spapr_pci_vfio_register_types)
