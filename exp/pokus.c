#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#include <sndfile.h>

static void dump(char *prefix, unsigned char *data, int len) {
	// return;
	struct timespec spec;
	clock_gettime(CLOCK_REALTIME, &spec);

	int i;

	printf("%d.%06ld %s: ", (int) spec.tv_sec, spec.tv_nsec/1000, prefix);
	if (len >= 0) {
		for (i = 0; i < len; i++) {
			printf("%02x ", data[i]);
		}
		printf("\n");
	} else {
		printf("Error %s\n", libusb_error_name(len));
	}
}

// packets per ISO tranfer on sync endpoint
#define SYNC_PKTS 1
unsigned int sync_packet_size = 0;

enum {
	AOUT_ENDPOINT = 0x02,
	SYNC_ENDPOINT = 0x81,
	AIN_ENDPOINT = 0x86
};

unsigned char audio_data[3*4*(44100+2000)];
unsigned int aout_pos;

static SNDFILE *file;
static FILE *f;

static void init_audio_data() {
	memset(audio_data, 0, 3*4*(44100+2000));
	int i;
	int c = 0;
	for (i = 0; i < (44100+2000); i++) {
		int ch1 = (int)((sin(440.*i/44100*2*M_PI)+1)*0x7fffff) - 0x7fffff;
		audio_data[i*12   + 3*c] = ch1 & 0xff;
		audio_data[i*12+1 + 3*c] = (ch1 >> 8) & 0xff;
		audio_data[i*12+2 + 3*c] = (ch1 >> 16) & 0xff;

		audio_data[i*12   + 3*(c+1)] = ch1 & 0xff;
		audio_data[i*12+1 + 3*(c+1)] = (ch1 >> 8) & 0xff;
		audio_data[i*12+2 + 3*(c+1)] = (ch1 >> 16) & 0xff;
	}
	aout_pos = 0;
}

#define FILLS 2
static int requested_fill[FILLS];
int current_fill = 0;

unsigned int eie_clock_d = 0;


static void sync_iso_cb(struct libusb_transfer* tr) {
	if (tr->status == LIBUSB_TRANSFER_COMPLETED) {
		int i;
		int pos = 0;
		for (i = 0; i < tr->num_iso_packets; i++) {
			if (tr->iso_packet_desc[i].actual_length > 0) {
				dump("sync", &tr->buffer[pos], tr->iso_packet_desc[i].actual_length);
				// the (last or first or just one of them) number in the buffer (that shifts) 
				// is actually the number of frames (samples) requested/played
				eie_clock_d += tr->buffer[pos];
				pos += tr->iso_packet_desc[i].actual_length;
			}
		}

		int err = libusb_submit_transfer(tr);
		if (err != 0) {
			printf("%d\n", err);
		}
	} else {
		printf("sync transfer failed\n");
	}
}

static void fill_aout_data(struct libusb_transfer* tr) {
	tr->buffer = &audio_data[aout_pos*12];
	// ignore stupid values of eie clock -> allow only small adjustments
	int expected_d = requested_fill[current_fill];
	if (eie_clock_d < expected_d - 10 || eie_clock_d > expected_d + 10) {
		eie_clock_d = expected_d;
	}
	int samples = eie_clock_d;
	eie_clock_d = 0;

	int i;
	int have = 0;
	for (i = 0; i < 40; i++) {
		int l =  samples * (i+1) / 40 - have;
		have += l;
		tr->iso_packet_desc[i].length = l * 12;
		// printf("%d ", l);
	}
	printf(" - %d\n", have);
	current_fill++;
	current_fill %= FILLS;
	aout_pos += samples;
	aout_pos %= 44100;
}

static void aout_iso_cb(struct libusb_transfer* tr) {
	if (tr->status == LIBUSB_TRANSFER_COMPLETED) {
		dump("aout", 0, 0);
		fill_aout_data(tr);
		int err = libusb_submit_transfer(tr);
		if (err != 0) {
			printf("%d\n", err);
		}
	} else {
		printf("aout error: %s\n", libusb_error_name(tr->status));
	}
}

unsigned char ain[1024*1024*8];
int out[1024*1024];


static void ain_bulk_cb(struct libusb_transfer* tr) {
	if (tr->status == LIBUSB_TRANSFER_COMPLETED) {
		printf("ain transfer ok len=%d\n", tr->actual_length);
		// dump("ain: ", ain, 60);
		// fwrite(ain, 1, tr->actual_length, f);
		int i, j;
		for (i = 0; i < tr->actual_length / 64; i++) {
			out[4*i+0] = 0;
			out[4*i+1] = 0;
			out[4*i+2] = 0;
			out[4*i+3] = 0;
			for (j = 0; j < 24; j++) {
				out[4*i+0] |= (ain[64*i + j] & 1) << (31-j);
				out[4*i+2] |= (ain[64*i + j] & 2) << (30-j);
			}
			for (j = 32; j < 56; j++) {
				out[4*i+1] |= (ain[64*i + j] & 1) << (63-j);
				out[4*i+3] |= (ain[64*i + j] & 2) << (62-j);
			}
		}
		sf_writef_int(file, out, tr->actual_length / 64);
	} else {
		printf("ain error: %s\n", libusb_error_name(tr->status));
	}
	libusb_submit_transfer(tr);
}

static int tryit(libusb_device_handle* d) {
	int err;
	err = libusb_set_configuration(d, 1);
	if (err) return err;

	err = libusb_claim_interface(d, 0);
	if (err) return err;

	err = libusb_claim_interface(d, 1);
	if (err) return err;


	err = libusb_set_interface_alt_setting(d, 0, 1);
	if (err) return err;

	err = libusb_set_interface_alt_setting(d, 1, 1);
	if (err) return err;

	unsigned char data[5];
	int cnt;
	memset(data, 0, 5);

	// struct timespec ts = {0, 1000000};
	// nanosleep(&ts, NULL);

	cnt = libusb_control_transfer(d, 0xc0, 86, 0, 0, data, 3, 1000);
	dump("i", data, cnt);
	cnt = libusb_control_transfer(d, 0xc0, 86, 0, 0, data, 5, 1000);
	dump("i", data, cnt);
	cnt = libusb_control_transfer(d, 0xc0, 73, 0, 0, data, 1, 1000);
	dump("i", data, cnt);
	cnt = libusb_control_transfer(d, 0xa2, 129, 0x0100, 0, data, 3, 1000);
	dump("i", data, cnt);

	// sampling rate - big endian 44100
	data[0] = 0x44;
	data[1] = 0xac;
	data[2] = 0x00;

	cnt = libusb_control_transfer(d, 0x22, 1, 0x0100, 134, data, 3, 1000);
	dump("o", data, cnt);
	cnt = libusb_control_transfer(d, 0x22, 1, 0x0100, 2, data, 3, 1000);
	dump("o", data, cnt);
	cnt = libusb_control_transfer(d, 0x22, 1, 0x0100, 134, data, 3, 1000);
	dump("o", data, cnt);

	cnt = libusb_control_transfer(d, 0xa2, 129, 0x0100, 134, data, 3, 1000);
	dump("i", data, cnt);
	cnt = libusb_control_transfer(d, 0xc0, 73, 0, 0, data, 1, 1000);
	dump("i", data, cnt);
	
	cnt = libusb_control_transfer(d, 0x40, 73, 0x0032, 0, data, 0, 1000);
	dump("o", data, cnt);

	sync_packet_size = libusb_get_max_iso_packet_size(libusb_get_device(d), SYNC_ENDPOINT);
	
	struct libusb_transfer *tr;
	unsigned char *recv_buf;

	tr = libusb_alloc_transfer(SYNC_PKTS);
	recv_buf = malloc(SYNC_PKTS * sync_packet_size);
	libusb_fill_iso_transfer(tr, d, SYNC_ENDPOINT, recv_buf, SYNC_PKTS * sync_packet_size, SYNC_PKTS, sync_iso_cb, NULL, 0);
	libusb_set_iso_packet_lengths(tr, sync_packet_size);
	libusb_submit_transfer(tr);

	tr = libusb_alloc_transfer(SYNC_PKTS);
	recv_buf = malloc(SYNC_PKTS * sync_packet_size);
	libusb_fill_iso_transfer(tr, d, SYNC_ENDPOINT, recv_buf, SYNC_PKTS * sync_packet_size, SYNC_PKTS, sync_iso_cb, NULL, 0);
	libusb_set_iso_packet_lengths(tr, sync_packet_size);
	libusb_submit_transfer(tr);

	requested_fill[0] = 221;
	requested_fill[1] = 220;
	tr = libusb_alloc_transfer(40);
	libusb_fill_iso_transfer(tr, d, AOUT_ENDPOINT, NULL, 221*12, 40, aout_iso_cb, NULL, 0);
	fill_aout_data(tr);
	libusb_submit_transfer(tr);

	tr = libusb_alloc_transfer(40);
	libusb_fill_iso_transfer(tr, d, AOUT_ENDPOINT, NULL, 221*12, 40, aout_iso_cb, NULL, 0);
	fill_aout_data(tr);
	libusb_submit_transfer(tr);

	tr = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(tr, d, AIN_ENDPOINT, ain, 4096, ain_bulk_cb, NULL, 0);
	libusb_submit_transfer(tr);

	tr = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(tr, d, AIN_ENDPOINT, ain, 4096, ain_bulk_cb, NULL, 0);
	libusb_submit_transfer(tr);

	struct timespec start;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &start);
	do {
		clock_gettime(CLOCK_REALTIME, &now);
		libusb_handle_events(NULL);
	} while (now.tv_sec - start.tv_sec < 20);

	libusb_release_interface(d, 1);
	libusb_release_interface(d, 0);

	return 0;
}

int prepare_output() {
	f = fopen("rec.raw", "wb");

	SF_INFO sfinfo;
	memset(&sfinfo, 0, sizeof (sfinfo));
	sfinfo.samplerate = 44100;
//	sfinfo.frames = SAMPLE_COUNT;
	sfinfo.channels = 4;
	sfinfo.format = (SF_FORMAT_AIFF | SF_FORMAT_PCM_24);
	file = sf_open("rec.aiff", SFM_WRITE, &sfinfo);
	if (!file) {
		return 1;
	}
	return 0;
}

int main(int argc, char const *argv[])
{
	init_audio_data();
	prepare_output();
	
	int err;
	
	err = libusb_init(NULL);
	if (err != 0) {
		return 1;
	}

	libusb_set_debug(NULL, 3);

	libusb_device_handle* dev;
	
	dev = libusb_open_device_with_vid_pid(NULL, 0x09e8, 0x0010);
	if (dev != NULL) {
		err = tryit(dev);
		if (err) {
			// printf("Error: %s\n", libusb_error_name(err));
			printf("Error: %d\n", err);
		}
		libusb_close(dev);
	} else {
		printf("Cannot find device.\n");
	}

	libusb_exit(NULL);

	sf_close(file);
	fclose(f);

	return 0;
}
