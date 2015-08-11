handle SIGUSR1 noprint nostop
b scsi_disk_emulate_read_data
b ide_atapi_cmd_reply_end if s.io_buffer_index==36
r -drive if=none,file=/home/zuban32/Downloads/dsl.iso,id=cdrom -drive if=none,id=fake -device ide-bridge,id=bridge,drive=fake -device scsi-cd,drive=cdrom,bus=bridge.0 2> ~/ide_bridge
#r -cdrom ~/Downloads/dsl.iso 2> ~/read_pio
