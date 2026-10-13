# 0153: the CD drive passes one sector per late service

Status: fixed

## Observed
Spyro 1 (`SCUS_942.28`) in the player build and environment (`build/player/bin/spyro_port`, no arguments,
`tools/shipping_settings.ini`, mapped window), picker, `pick spyro1`, the user's pad recording replayed,
`PSXPORT_DEBUG=xa,audiofield,cdcpace`. The title-screen music (XA, LBA 113448, file 1 channel 4, mode 0xC8)
decoded 97 audio sectors in 12.7 s where the disc rate owes 238. The XA ring stayed at 0-179 frames and the
SPU consumed 14,290-15,950 source frames/s of 37,800: the music played in fragments with ~60% silence.
`cdcpace`: 60.1 sector events/s against the drive's 150, each serviced 1.5 sector periods after its
deadline.

## Cause
`runtime/psx/cd/cdc_native.cpp:service_drive_event` handled one sector per call and re-armed the next one a
period after the service time. Spyro 1's music stream raises no guest interrupts, so the drive is serviced
only at each display field, which is 2.5 sector periods at double speed; the remaining 1.5 periods were
lost every field.

## Resolution
`service_drive_event` serves every sector whose deadline has passed, and an audio sector re-arms one period
after its own deadline (`schedule_sector_event_after`). Data sectors still hold the head until the guest
consumes them. Test: `test_cdc_xa_filter` `testLateServicePassesEveryElapsedAudioSector` (failed before:
"a service 2.5 periods late did not pass the 3 sectors due by then").

After, same run: 150.2 sector events/s, 238 of 238 audio sectors for the same title stream, continuous
consumption at 37,800 frames/s.
