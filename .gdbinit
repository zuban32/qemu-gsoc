handle SIGUSR1 noprint nostop
b ide_start_dma
#r -drive if=none,file=/home/zuban32/Downloads/dsl.iso,id=cdrom -drive if=none,id=fake -device ide-bridge,id=bridge,drive=fake -device scsi-cd,drive=cdrom,bus=bridge.0 2> ~/ide_bridge
r -cdrom ~/Downloads/dsl.iso 2> ~/read_pio
