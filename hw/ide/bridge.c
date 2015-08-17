#include "hw/ide/bridge.h"

static void ide_bridge_do_transfer(IDEState *s)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, s->cur_req);

    if (r->buflen > 0) {
        int size = r->buflen;

        int byte_count_limit = s->lcyl | (s->hcyl << 8);
        if (byte_count_limit == 0xffff) {
            byte_count_limit--;
        }
        if (size > byte_count_limit) {
            /* byte count limit must be even if this case */
            if (byte_count_limit & 1) {
                byte_count_limit--;
            }
            size = byte_count_limit;
        }
        s->lcyl = size;
        s->hcyl = size >> 8;
        s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;

        int offset = (r->buflen == r->qiov.size) ? 0 : r->qiov.size - r->buflen;
        r->buflen -= size;

        ide_transfer_start(s, s->io_buffer + offset, size,
                           &ide_bridge_do_transfer);
        ide_set_irq(s->bus);
    } else {
        scsi_req_complete(s->cur_req, GOOD);
    }
}

static void ide_bridge_dma_complete(void *opaque, int ret)
{
    IDEState *s = opaque;

    s->io_buffer_size = s->bus->dma->iov.iov_len;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    s->bus->dma->ops->rw_buf(s->bus->dma, 1);
    scsi_req_complete(s->cur_req, GOOD);

    s->status = READY_STAT | SEEK_STAT;

    ide_set_irq(s->bus);
    ide_set_inactive(s, false);
}

void ide_bridge_start_transfer(SCSIRequest *req, uint32_t len)
{
    IDEDevice *dev = IDE_DEVICE(req->bus->qbus.parent);
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, dev->qdev.parent_bus);
    IDEState *s = bus->ifs;
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    int cmd = req->cmd.buf[0];
    if (cmd == READ_10) {
        if (s->feature & 1) {
            s->status = READY_STAT | SEEK_STAT | DRQ_STAT | BUSY_STAT;
            qemu_iovec_clone(&s->bus->dma->qiov, &r->qiov, NULL);
            qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
        } else {
            qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
        }
    } else {
        if (cmd == INQUIRY) {
            len = 36;
        }
        r->iov.iov_len = len;
        qemu_iovec_concat_iov(&r->qiov, &r->iov, len, 0, len);
        qemu_iovec_to_buf(&r->qiov, 0, s->io_buffer, r->qiov.size);
    }

    s->io_buffer_index = 0;
    s->status = READY_STAT | SEEK_STAT;

    if (cmd != TEST_UNIT_READY && cmd != ALLOW_MEDIUM_REMOVAL) {
        if (s->feature & 1) {
            s->io_buffer_index = 0;
            s->bus->retry_unit = s->unit;
            s->bus->retry_sector_num = ide_get_sector(s);
            s->bus->retry_nsector = s->nsector;

            s->bus->dma->iov.iov_base = (void *)(s->io_buffer);
            s->bus->dma->iov.iov_len = r->qiov.size;

            if (cmd != READ_10) {
                s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
            }

            if (s->bus->dma->ops->start_dma) {
                s->bus->dma->ops->start_dma(s->bus->dma, s,
                                            ide_bridge_dma_complete);
            }
        } else {
            r->buflen = r->qiov.size;
            ide_bridge_do_transfer(s);
        }
    } else {
        scsi_req_complete(req, GOOD);
    }
}

void ide_bridge_complete(SCSIRequest *req, uint32_t status, size_t resid)
{
    IDEDevice *dev = IDE_DEVICE(req->bus->qbus.parent);
    IDEBus *bus = DO_UPCAST(IDEBus, qbus, dev->qdev.parent_bus);
    IDEState *s = bus->ifs;

    ide_atapi_cmd_ok(s);
    ide_set_irq(s->bus);
}
