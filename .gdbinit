handle SIGUSR1 noprint nostop
b cmd_packet
b ide_transfer_stop
b ide_transfer_start
b ide_cmd_done
b ide_atapi_cmd_reply_end
r -bios ~/seabios/out/bios.bin -drive if=none,file=/home/zuban32/Downloads/dsl.iso,id=cdrom -drive if=none,id=fake -device ide-bridge,id=bridge,drive=fake -device scsi-cd,drive=cdrom,bus=bridge.0 2> ~/ide_bridge
#r -bios ~/seabios/out/bios.bin -cdrom ~/Downloads/dsl.iso 2> ~/read_pio
