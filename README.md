eie-pro-linux
=============

WIP on a Linux driver for Akai EIE Pro sound interface.

*The driver is by no means stable or ready to use.*

The USB part of the driver is probably nearly finished but the ALSA and (error) state handling parts require more work. 

The `exp` directory is a libusb "driver" prototype capable of simutaneous playback and capture of all 4 channels at 44.1 kHz.

The driver is heavily inspired by the ua101 driver from Linux.
