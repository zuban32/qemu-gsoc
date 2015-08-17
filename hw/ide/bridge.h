#ifndef HW_IDE_BRIDGE_H
#define HW_IDE_BRIDGE_H

#include "hw/ide/internal.h"

void ide_bridge_start_transfer(SCSIRequest *req, uint32_t len);
void ide_bridge_complete(SCSIRequest *req, uint32_t status, size_t resid);
void ide_bridge_do_transfer(IDEState *s);

#endif
