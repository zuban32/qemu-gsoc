/*
 * ide bus support for qdev.
 *
 * Copyright (c) 2009 Gerd Hoffmann <kraxel@redhat.com>
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
#include <hw/hw.h>
#include "sysemu/dma.h"
#include "qemu/error-report.h"
#include <hw/ide/internal.h>
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/block/block.h"
#include "sysemu/sysemu.h"
#include "qapi/visitor.h"

/* --------------------------------- */

static inline int ube16_to_cpu(const uint8_t *buf)
{
    return (buf[0] << 8) | buf[1];
}

static inline int ube32_to_cpu(const uint8_t *buf)
{
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

static void ide_bridge_ok(IDEState *s)
{
//     fprintf(stderr, "bridge_ok\n");
    s->status = READY_STAT | SEEK_STAT;

    
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, s->cur_req);
    if(r->buflen > 0) {
        int size = r->buflen;
        
        int byte_count_limit = s->lcyl | (s->hcyl << 8);
        if (byte_count_limit == 0xffff)
            byte_count_limit--;
        if (size > byte_count_limit) {
            /* byte count limit must be even if this case */
            if (byte_count_limit & 1)
                byte_count_limit--;
            size = byte_count_limit;
        }
        s->lcyl = size;
        s->hcyl = size >> 8;
        
        ide_transfer_start(s, s->io_buffer + r->qiov.size - r->buflen, size, &ide_bridge_ok);
        
        r->buflen -= size;
        ide_set_irq(s->bus);
    } else {
        ide_atapi_cmd_ok(s);
        ide_set_irq(s->bus);
    }
}

// Only for other SCSI device compatibility
static void ide_bridge_transfer(SCSIRequest *req, uint32_t len)
{
}

static void ide_bridge_dma_complete(void *opaque, int ret)
{
    IDEState *s = opaque;
    
//     fprintf(stderr, "Finishing DMA transfer\nGot data:\n");
//     int i = 0;
//     for(i = 0; i < 16; i++) {
//         fprintf(stderr, "[%x]", ((uint8_t *)s->io_buffer)[i]);
//         if(i % 8 == 7) fprintf(stderr, "\n");
//     }
    s->io_buffer_size = s->bus->dma->iov.iov_len;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    s->bus->dma->ops->rw_buf(s->bus->dma, 1);
    
    s->status = READY_STAT | SEEK_STAT;
    
    ide_set_irq(s->bus);
    ide_set_inactive(s, false);
}

static void ide_bridge_complete(SCSIRequest *req, uint32_t status, size_t resid)
{
//     fprintf(stderr, "bridge_complete\n");
    
    IDEDevice *dev = IDE_DEVICE(req->bus->qbus.parent);
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, dev->qdev.parent_bus);
    IDEState *s = bus->ifs;  
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    
    int cmd = req->cmd.buf[0];
//     fprintf(stderr, "iov.len = %d\n", (int)r->iov.iov_len);
    
    if(cmd == 0x52) {
        fprintf(stderr, "Incorrect command: %d\n", cmd);
//         ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE);
        return;
    }
    
    
    if(cmd == READ_10) {
        if(s->feature & 0x1) {
            s->status = READY_STAT | SEEK_STAT | DRQ_STAT | BUSY_STAT;
            qemu_iovec_clone(&s->bus->dma->qiov, &r->qiov, NULL);
            qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
//             fprintf(stderr, "DMA complete\n");
        } else {
            qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
        }
    }
    else if(cmd == INQUIRY || cmd == MODE_SENSE_10 || cmd == READ_TOC ||
        cmd == READ_CAPACITY_10 || cmd == GET_CONFIGURATION || cmd == GET_EVENT_STATUS_NOTIFICATION
    || cmd == READ_DISC_INFORMATION)
    {
        switch(cmd) {
            case INQUIRY:
                r->iov.iov_len = 36;
                break;
            case MODE_SENSE_10:
                r->iov.iov_len = 30;
                break;
            case READ_TOC:
                r->iov.iov_len = 12;
                break;
            case READ_CAPACITY_10:
                r->iov.iov_len = 8;
                break;
            case GET_CONFIGURATION:
                r->iov.iov_len = 40;
                break;
            case GET_EVENT_STATUS_NOTIFICATION:
                r->iov.iov_len = 8;
                break;
            case READ_DISC_INFORMATION:
                r->iov.iov_len = 34;
                break;
            default:
                break;
        }
        
//         fprintf(stderr, "r->buflen = %d\n", r->buflen);
        
        qemu_iovec_concat_iov(&r->qiov, &r->iov, r->iov.iov_len, 0, r->iov.iov_len);
//         memmove(s->io_buffer, req->cmd.buf, r->iov.iov_len);
//         fprintf(stderr, "after concat: rqiov.size = %d\n", (int)r->qiov.size);
        
        qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
//         fprintf(stderr, "transfer: io_data [%x][%x][%x][%x]\n", s->io_buffer[0], s->io_buffer[1], s->io_buffer[2], s->io_buffer[3]);
    }
    
    
    s->status = READY_STAT | SEEK_STAT;
    s->io_buffer_index = 0;
    int size = r->qiov.size;
    
    r->buflen = size;
    
    int byte_count_limit = s->lcyl | (s->hcyl << 8);
    if (byte_count_limit == 0xffff)
        byte_count_limit--;
    if (size > byte_count_limit) {
        /* byte count limit must be even if this case */
        if (byte_count_limit & 1)
            byte_count_limit--;
        size = byte_count_limit;
    }
    if(!(s->feature & 1)) {
    s->lcyl = size;
    s->hcyl = size >> 8;    
    r->buflen -= size;
    
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
    }
//     fprintf(stderr, "r.qiov.size = %d\n", (unsigned)r->qiov.size);
    
//     printf("bridge_complete: cmd = %x\n", req->cmd.buf[0]);
    fflush(stdout);
    
    if(req->cmd.buf[0] != TEST_UNIT_READY && req->cmd.buf[0] != ALLOW_MEDIUM_REMOVAL) {
        if(s->feature & 0x1) {
            
//             fprintf(stderr, "DMA started\n");
            s->io_buffer_index = 0;
            s->bus->retry_unit = s->unit;
            s->bus->retry_sector_num = ide_get_sector(s);
            s->bus->retry_nsector = s->nsector;
            
           
            s->bus->dma->iov.iov_base = (void *)(s->io_buffer);
            s->bus->dma->iov.iov_len = r->qiov.size;
            
            if(cmd != READ_10)
                s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
            
            if (s->bus->dma->ops->start_dma) {
                s->bus->dma->ops->start_dma(s->bus->dma, s, ide_bridge_dma_complete);
            }
            return;
        }
        else {
            ide_transfer_start(s, s->io_buffer, size, &ide_bridge_ok);
            ide_set_irq(s->bus);
        }
    }
    else
        ide_bridge_ok(s);
}

static const struct SCSIBusInfo atapi_scsi_info = {
    .tcq = true,
    .max_target = 0,
    .max_lun = 0,
    
    .transfer_data = ide_bridge_transfer,
    .complete = ide_bridge_complete,
    .cancel = NULL
};

static char *idebus_get_fw_dev_path(DeviceState *dev);

static Property ide_props[] = {
    DEFINE_PROP_UINT32("unit", IDEDevice, unit, -1),
    DEFINE_PROP_END_OF_LIST(),
};

static void ide_bus_class_init(ObjectClass *klass, void *data)
{
    BusClass *k = BUS_CLASS(klass);

    k->get_fw_dev_path = idebus_get_fw_dev_path;
}

static const TypeInfo ide_bus_info = {
    .name = TYPE_IDE_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(IDEBus),
    .class_init = ide_bus_class_init,
};

void ide_bus_new(IDEBus *idebus, size_t idebus_size, DeviceState *dev,
                 int bus_id, int max_units)
{
    qbus_create_inplace(idebus, idebus_size, TYPE_IDE_BUS, dev, NULL);
    idebus->bus_id = bus_id;
    idebus->max_units = max_units;
}

static char *idebus_get_fw_dev_path(DeviceState *dev)
{
    char path[30];

    snprintf(path, sizeof(path), "%s@%x", qdev_fw_name(dev),
             ((IDEBus*)dev->parent_bus)->bus_id);

    return g_strdup(path);
}

static int ide_qdev_init(DeviceState *qdev)
{
    IDEDevice *dev = IDE_DEVICE(qdev);
    IDEDeviceClass *dc = IDE_DEVICE_GET_CLASS(dev);
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, qdev->parent_bus);

    if (!dev->conf.blk && dc->parent_class.desc ) {
        error_report("No drive specified");
        goto err;
    }
    if (dev->unit == -1) {
        dev->unit = bus->master ? 1 : 0;
    }

    if (dev->unit >= bus->max_units) {
        error_report("Can't create IDE unit %d, bus supports only %d units",
                     dev->unit, bus->max_units);
        goto err;
    }

    switch (dev->unit) {
    case 0:
        if (bus->master) {
            error_report("IDE unit %d is in use", dev->unit);
            goto err;
        }
        bus->master = dev;
        break;
    case 1:
        if (bus->slave) {
            error_report("IDE unit %d is in use", dev->unit);
            goto err;
        }
        bus->slave = dev;
        break;
    default:
        error_report("Invalid IDE unit %d", dev->unit);
        goto err;
    }
    return dc->init(dev);

err:
    return -1;
}

IDEDevice *ide_create_drive(IDEBus *bus, int unit, DriveInfo *drive)
{
    DeviceState *dev;

    dev = qdev_create(&bus->qbus, drive->media_cd ? "ide-cd" : "ide-hd");
    qdev_prop_set_uint32(dev, "unit", unit);
    qdev_prop_set_drive_nofail(dev, "drive", blk_by_legacy_dinfo(drive));
    qdev_init_nofail(dev);
    return DO_UPCAST(IDEDevice, qdev, dev);
}

int ide_get_geometry(BusState *bus, int unit,
                     int16_t *cyls, int8_t *heads, int8_t *secs)
{
    IDEState *s = &DO_UPCAST(IDEBus, qbus, bus)->ifs[unit];

    if (s->drive_kind != IDE_HD || !s->blk) {
        return -1;
    }

    *cyls = s->cylinders;
    *heads = s->heads;
    *secs = s->sectors;
    return 0;
}

int ide_get_bios_chs_trans(BusState *bus, int unit)
{
    return DO_UPCAST(IDEBus, qbus, bus)->ifs[unit].chs_trans;
}

/* --------------------------------- */

typedef struct IDEDrive {
    IDEDevice dev;
} IDEDrive;

static int ide_dev_initfn(IDEDevice *dev, IDEDriveKind kind)
{
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, dev->qdev.parent_bus);
    fprintf(stderr, "unit = %d\n", dev->unit);
    IDEState *s = bus->ifs + dev->unit;
    Error *err = NULL;

    if (dev->conf.discard_granularity == -1) {
        dev->conf.discard_granularity = 512;
    } else if (dev->conf.discard_granularity &&
               dev->conf.discard_granularity != 512) {
        error_report("discard_granularity must be 512 for ide");
        return -1;
    }

    blkconf_blocksizes(&dev->conf);
    if (dev->conf.logical_block_size != 512) {
        error_report("logical_block_size must be 512 for IDE");
        return -1;
    }

    blkconf_serial(&dev->conf, &dev->serial);
    if (kind != IDE_CD && kind != IDE_BRIDGE) {
        blkconf_geometry(&dev->conf, &dev->chs_trans, 65536, 16, 255, &err);
        if (err) {
            error_report_err(err);
            return -1;
        }
    }

    if (ide_init_drive(s, dev->conf.blk, kind,
                       dev->version, dev->serial, dev->model, dev->wwn,
                       dev->conf.cyls, dev->conf.heads, dev->conf.secs,
                       dev->chs_trans) < 0) {
        return -1;
    }

    if (kind == IDE_BRIDGE) {
        scsi_bus_new(&dev->scsi_bus, sizeof(dev->scsi_bus), &dev->qdev,
                     &atapi_scsi_info, NULL);
        scsi_bus_legacy_handle_cmdline(&dev->scsi_bus, NULL);
    }

    if (!dev->version) {
        dev->version = g_strdup(s->version);
    }
    if (!dev->serial) {
        dev->serial = g_strdup(s->drive_serial_str);
    }

//     add_boot_device_path(dev->conf.bootindex, &dev->qdev,
//                          dev->unit ? "/disk@1" : "/disk@0");

    return 0;
}

static void ide_dev_get_bootindex(Object *obj, Visitor *v, void *opaque,
                                  const char *name, Error **errp)
{
    IDEDevice *d = IDE_DEVICE(obj);

    visit_type_int32(v, &d->conf.bootindex, name, errp);
}

static void ide_dev_set_bootindex(Object *obj, Visitor *v, void *opaque,
                                  const char *name, Error **errp)
{
    IDEDevice *d = IDE_DEVICE(obj);
    int32_t boot_index;
    Error *local_err = NULL;

    visit_type_int32(v, &boot_index, name, &local_err);
    if (local_err) {
        goto out;
    }
    /* check whether bootindex is present in fw_boot_order list  */
    check_boot_index(boot_index, &local_err);
    if (local_err) {
        goto out;
    }
    /* change bootindex to a new one */
    d->conf.bootindex = boot_index;

    if (d->unit != -1) {
        add_boot_device_path(d->conf.bootindex, &d->qdev,
                             d->unit ? "/disk@1" : "/disk@0");
    }
out:
    if (local_err) {
        error_propagate(errp, local_err);
    }
}

static void ide_dev_instance_init(Object *obj)
{
    object_property_add(obj, "bootindex", "int32",
                        ide_dev_get_bootindex,
                        ide_dev_set_bootindex, NULL, NULL, NULL);
    object_property_set_int(obj, -1, "bootindex", NULL);
}

static int ide_hd_initfn(IDEDevice *dev)
{
    return ide_dev_initfn(dev, IDE_HD);
}

static int ide_cd_initfn(IDEDevice *dev)
{
    return ide_dev_initfn(dev, IDE_CD);
}

static int ide_bridge_initfn(IDEDevice *dev)
{
    return ide_dev_initfn(dev, IDE_BRIDGE);
}

static int ide_drive_initfn(IDEDevice *dev)
{
    DriveInfo *dinfo = blk_legacy_dinfo(dev->conf.blk);

    return ide_dev_initfn(dev, dinfo && dinfo->media_cd ? IDE_CD : IDE_HD);
}

#define DEFINE_IDE_DEV_PROPERTIES()                     \
    DEFINE_BLOCK_PROPERTIES(IDEDrive, dev.conf),        \
    DEFINE_PROP_STRING("ver",  IDEDrive, dev.version),  \
    DEFINE_PROP_UINT64("wwn",  IDEDrive, dev.wwn, 0),    \
    DEFINE_PROP_STRING("serial",  IDEDrive, dev.serial),\
    DEFINE_PROP_STRING("model", IDEDrive, dev.model)

static Property ide_hd_properties[] = {
    DEFINE_IDE_DEV_PROPERTIES(),
    DEFINE_BLOCK_CHS_PROPERTIES(IDEDrive, dev.conf),
    DEFINE_PROP_BIOS_CHS_TRANS("bios-chs-trans",
                IDEDrive, dev.chs_trans, BIOS_ATA_TRANSLATION_AUTO),
    DEFINE_PROP_END_OF_LIST(),
};

static void ide_hd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IDEDeviceClass *k = IDE_DEVICE_CLASS(klass);
    k->init = ide_hd_initfn;
    dc->fw_name = "drive";
    dc->desc = "virtual IDE disk";
    dc->props = ide_hd_properties;
}

static const TypeInfo ide_hd_info = {
    .name          = "ide-hd",
    .parent        = TYPE_IDE_DEVICE,
    .instance_size = sizeof(IDEDrive),
    .class_init    = ide_hd_class_init,
};

static Property ide_cd_properties[] = {
    DEFINE_IDE_DEV_PROPERTIES(),
    DEFINE_PROP_END_OF_LIST(),
};

static void ide_cd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IDEDeviceClass *k = IDE_DEVICE_CLASS(klass);
    k->init = ide_cd_initfn;
    dc->fw_name = "drive";
    dc->desc = "virtual IDE CD-ROM";
    dc->props = ide_cd_properties;
}

static const TypeInfo ide_cd_info = {
    .name          = "ide-cd",
    .parent        = TYPE_IDE_DEVICE,
    .instance_size = sizeof(IDEDrive),
    .class_init    = ide_cd_class_init,
};

static void ide_bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IDEDeviceClass *k = IDE_DEVICE_CLASS(klass);
    k->init = ide_bridge_initfn;
    dc->fw_name = "drive";
    dc->desc = "virtual ATAPI-SCSI bridge";
    dc->props = ide_cd_properties;
}

static const TypeInfo ide_bridge_info = {
    .name          = "ide-bridge",
    .parent        = TYPE_IDE_DEVICE,
    .instance_size = sizeof(IDEDrive),
    .class_init    = ide_bridge_class_init,
};

static Property ide_drive_properties[] = {
    DEFINE_IDE_DEV_PROPERTIES(),
    DEFINE_PROP_END_OF_LIST(),
};

static void ide_drive_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IDEDeviceClass *k = IDE_DEVICE_CLASS(klass);
    k->init = ide_drive_initfn;
    dc->fw_name = "drive";
    dc->desc = "virtual IDE disk or CD-ROM (legacy)";
    dc->props = ide_drive_properties;
}

static const TypeInfo ide_drive_info = {
    .name          = "ide-drive",
    .parent        = TYPE_IDE_DEVICE,
    .instance_size = sizeof(IDEDrive),
    .class_init    = ide_drive_class_init,
};

static void ide_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    k->init = ide_qdev_init;
    set_bit(DEVICE_CATEGORY_STORAGE, k->categories);
    k->bus_type = TYPE_IDE_BUS;
    k->props = ide_props;
}

static const TypeInfo ide_device_type_info = {
    .name = TYPE_IDE_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IDEDevice),
    .abstract = true,
    .class_size = sizeof(IDEDeviceClass),
    .class_init = ide_device_class_init,
    .instance_init = ide_dev_instance_init,
};

static void ide_register_types(void)
{
    type_register_static(&ide_bus_info);
    type_register_static(&ide_hd_info);
    type_register_static(&ide_cd_info);
    type_register_static(&ide_bridge_info);
    type_register_static(&ide_drive_info);
    type_register_static(&ide_device_type_info);
}

type_init(ide_register_types)
