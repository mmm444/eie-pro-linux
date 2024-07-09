# eie-pro-linux

Linux driver for Akai EIE Pro sound interface. The current development status
is highly experimental.

*The driver is by no means stable or ready to use.*

The USB part of the driver is probably nearly finished but the ALSA and
(error) state handling require more work.

The `exp` directory is a libusb "driver" prototype capable of simutaneous
playback and capture of all 4 channels at 44.1 kHz.

The driver is heavily inspired by the ua101 driver from the Linux kernel
source tree.


## USB protocol

The USB device is identified by vednor ID 0x09e8 and product ID 0x0010. It has
1 configuration with 2 interfaces. When alternate setting 1 is applied on both
interfaces and an intilization control sequence is sent to endpoint 0 the
device becomes fully operational. It sends clock, audio and midi and expects
audio and midi. The audio has always 24-bit depth. The audio input and output
run at the same frequency. There are 4 possible frequencies: 44100, 48000,
88200 and 96000 Hz.


### Initialization sequence

Before any other communication takes place the card is initialized with the
following sequence of USB control messages.

 RType | Req | Value | Index | Len | Data | Comment
:-----:|:---:|:-----:|:-----:|:---:|------|--------
`0xc0` | `0x56`| `0x0000`| `0x0000`|  3  | `31 01 04` | get firmware version, probably, mine FW is 1.04
`0xc0` | `0x56`| `0x0000`| `0x0000`|  5  | `31 01 04` | the same again with larger buffer
`0xc0` | `0x49`| `0x0000`| `0x0000`|  1  | `32` | ? 50 in decimal
`0xa2` | `0x81`| `0x0100`| `0x0000`|  3  | `44 ac 00` | get current sampling rate
`0x22` | `0x01`| `0x0100`| `0x0086`|  3  | `44 ac 00` | set sampling rate
`0x22` | `0x01`| `0x0100`| `0x0002`|  3  | ? | ?, when sampling rate is sent again nothing bad happens
`0x22` | `0x01`| `0x0100`| `0x0086`|  3  | `44 ac 00` | set sampling rate again
`0xa2` | `0x81`| `0x0100`| `0x0086`|  3  | `44 ac 00` | verify rate
`0xc0` | `0x49`| `0x0000`| `0x0000`|  1  | `32` | ? again 50
`0x40` | `0x49`| `0x0032`| `0x0000`|  0  | | ? 50 sent using value

The Data column is in hex. When RType is less then 0x80 it means is the data
is sent to the card, received otherwise.

The only thing I am sure about is that the sampling rate of the card is set
here. The 3 bytes are simply little-endian representation of the sampling
rate.

* `44 ac 00` = 0xac44 = 44100
* `80 bb 00` = 0xbb80 = 48000
* `88 58 01` = 0x15888 = 88200
* `00 17 01` = 0x11700 = 96000


### Interface 0 Endpoint 2 Iso Out - Audio Out

The audio is sent interleaved in little-endian byte order. The bytes are
tightly packed and all 4 channels are always present so each audio frame has
12 B. The ISO packets are sent every 5ms (USB clock). Each contains 40 USB
microframes evenly filled with audio frames. The number of audio frames in
each USB frame should be equal to the number of audio frames elapsed as
measured by the clock on the card.


### Interface 0 Endpoint 3 Bulk In - MIDI In

9 B transfers of raw MIDI data "randomly" interleaved with 0xfd.


### Interface 0 Endpoint 4 Bulk Out - MIDI Out

9 B transfers where first 1-4 (usually 3) bytes are raw MIDI data padded with
0xfd and ended with 0xe0.


### Interface 1 Endpoint 1 Iso In - Clock

Every microframe received is 3 B long. The first byte contains the number of
audio frames processed on the card since the last USB microframe was sent. The
2nd byte contains the number that was first in the previous microframe and the
3rd is the first from one frame before. They just shift.


### Interface 1 Endpoint 5 Bulk In - Audio In

Each audio frame (4 channels, 24-bit) is stored in 64 B. In each byte only 2
least significant bits are used. The decoding to 4 24-bit values looks like
this:

	ch1 = ch2 = ch3 = ch4 = 0;
	for (i = 0; i < 24; i++) {
		ch1 |= ( in[i+ 0]       & 1) << (23-i);
		ch2 |= ( in[i+32]       & 1) << (23-i);
		ch3 |= ((in[i+ 0] >> 1) & 1) << (23-i);
		ch4 |= ((in[i+32] >> 1) & 1) << (23-i);
	}
