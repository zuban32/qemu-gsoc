#include "hw/ide/bridge.h"

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
void ide_bridge_transfer(SCSIRequest *req, uint32_t len)
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

void ide_bridge_complete(SCSIRequest *req, uint32_t status, size_t resid)
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