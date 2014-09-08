#include <linux/init.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/usb.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

MODULE_DESCRIPTION("Akai EIE pro driver");
MODULE_AUTHOR("Michal Rydlo <michal.rydlo@gmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_SUPPORTED_DEVICE("{{Akai,EIE pro}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static DEFINE_MUTEX(devices_mutex);
static unsigned int devices_used;
static struct usb_driver eie_driver;

#define SYNC_URB_CNT 2

#define PLAY_URB_CNT 2
#define PLAY_PKT_CNT 40

#define CAP_URB_CNT 2

#define BYTES_PER_FRAME 12
#define BYTES_PER_FRAME_CAP 16

enum {
	PLAYBACK_RUNNING,
	CAPTURE_RUNNING,
	URBS_FLOWING,
	DISCONNECTED /* TODO use */
};

struct eie_playback_urb {
	struct eie *eie;
	struct urb *urb;
	bool silent;
	unsigned int len; /* in frames */
};

struct eie {
	struct usb_device *udev;
	struct usb_interface *ifa;
	struct usb_interface *ifb;

	struct snd_card *card;
	unsigned int card_index;

	struct snd_pcm *pcm;

	unsigned int rate;

	__u8 sync_endpointAddr;
	size_t sync_packet_size;
	struct urb *sync_urbs[SYNC_URB_CNT];

	__u8 play_endpointAddr;
	size_t play_packet_size;

	struct eie_playback_urb play_urbs[PLAY_URB_CNT];
	struct snd_pcm_substream *play_substream;
	wait_queue_head_t urbs_flow_wait;

	unsigned int play_buf_pos;
	unsigned int played_frames;
	unsigned char wanted_idx;

	__u8 cap_endpointAddr;
	struct urb *cap_urbs[CAP_URB_CNT];
	struct snd_pcm_substream *cap_substream;
	unsigned int cap_buf_pos;
	unsigned int cap_frames;

	atomic_t frames_elapsed; /**< frames elapsed as reported by EIE */

	spinlock_t lock;

	unsigned long states;
};

static struct snd_pcm_hardware eie_playback_hw = {
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES),
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = (SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000),
	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = 4,
	.channels_max = 4,
	.buffer_bytes_max = 45000 * 1024, /* TODO: clarify, copied from ua101 */
	.period_bytes_min = 64*BYTES_PER_FRAME,
	.period_bytes_max = UINT_MAX,
	.periods_min = 2,
	.periods_max = UINT_MAX,
};

static void kill_all_urbs(struct eie *eie);
static int submit_init_play_urbs(struct eie *eie);

static int eie_set_alt_setting(struct eie *eie)
{
	int err;
	err = usb_set_interface(eie->udev, 0, 1);
	if (err == 0)
		err = usb_set_interface(eie->udev, 1, 1);
	return err;
}

static int eie_setup_hw(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;

	/* TODO: determine possible HW params from runnnig streams. */
	substream->runtime->hw = eie_playback_hw;
	err = snd_pcm_hw_constraint_minmax(substream->runtime,
		SNDRV_PCM_HW_PARAM_BUFFER_TIME, 10*1000, UINT_MAX);
	return err;
}

static int eie_ppcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;
	err = eie_setup_hw(substream);
	if (err < 0)
		return err;
	eie->play_substream = substream;
	return 0;
}

static int eie_cpcm_open(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	int err;
	err = eie_setup_hw(substream);
	if (err < 0)
		return err;
	eie->cap_substream = substream;
	return 0;
}


static int eie_ppcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	eie->play_substream = NULL;

	return 0;
}

static int eie_cpcm_close(struct snd_pcm_substream *substream)
{
	struct eie *eie = substream->private_data;
	eie->cap_substream = NULL;

	return 0;
}


static int eie_pcm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
		params_buffer_bytes(hw_params));
}

static int eie_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static const char *usb_error_string(int err)
{
	switch (err) {
	case -ENODEV:
		return "no device";
	case -ENOENT:
		return "endpoint not enabled";
	case -EPIPE:
		return "endpoint stalled";
	case -ENOSPC:
		return "not enough bandwidth";
	case -ESHUTDOWN:
		return "device disabled";
	case -EHOSTUNREACH:
		return "device suspended";
	case -ETIMEDOUT:
		return "operation timed out";
	case -EINVAL:
	case -EAGAIN:
	case -EFBIG:
	case -EMSGSIZE:
		return "internal error";
	default:
		return "unknown error";
	}
}

static int submit_init_sync_urbs(struct eie *eie)
{
	int err;
	int i;

	for (i = 0; i < SYNC_URB_CNT; i++) {
		err = usb_submit_urb(eie->sync_urbs[i], GFP_KERNEL);
		if (err < 0) {
			dev_err(&eie->udev->dev, "USB request error %d %s",
				err, usb_error_string(err));
			return err;
		}
	}

	dev_dbg(&eie->udev->dev, "Submitted sync urbs.");

	return 0;
}

struct magic_seq {
	__u8 type;
	__u8 request;
	__u16 value;
	__u16 index;
	__u16 size;
};

static struct magic_seq magic_seq1[] = {
	{0xc0, 86, 0, 0, 3},
	{0xc0, 86, 0, 0, 5},
	{0xc0, 73, 0, 0, 1},
	{0xa2, 129, 0x0100, 0, 3},
	{0, 0, 0, 0, 0}
};

static struct magic_seq magic_seq2[] = {
	{0x22, 1, 0x0100, 134, 3},
	{0x22, 1, 0x0100, 2, 3},
	{0x22, 1, 0x0100, 134, 3},

	{0xa2, 129, 0x0100, 134, 3},
	{0xc0, 73, 0, 0, 1},
	{0x40, 73, 0x0032, 0, 0},
	{0, 0, 0, 0, 0}
};

static int send_magic_sequence(struct eie *eie, struct magic_seq *m, void *data)
{
	int err;

	while (m->type != 0) {
		unsigned int p = m->type & 0x80 ? usb_rcvctrlpipe(eie->udev, 0)
			: usb_sndctrlpipe(eie->udev, 0);

		dev_dbg(&eie->udev->dev, "Sending control transfer. %hhx %hhu %hx %hu",
			m->type, m->request, m->value, m->index);
		err = usb_control_msg(eie->udev, p, m->request, m->type,
			m->value, m->index, data, m->size, 1000);
		if (err < 0) {
			dev_dbg(&eie->udev->dev, "Result of control transfer. %d %s",
				err, usb_error_string(err));
			return err;
		}

		m++;
	}

	return 0;
}

static int reset_eie(struct eie *eie, unsigned int rate)
{
	int err;
	unsigned char data[6];


	dev_dbg(&eie->udev->dev, "Resetting device");
	kill_all_urbs(eie);
	eie_set_alt_setting(eie);

	dev_dbg(&eie->udev->dev, "Starting magic initialization sequence.");

	err = send_magic_sequence(eie, &magic_seq1[0], data);
	if (err < 0)
		return err;

	*((__le32 *) data) = __cpu_to_le32(rate);
	dev_dbg(&eie->udev->dev, "rate: %d data: %02hhx %02hhx %02hhx",
		rate, data[0], data[1], data[2]);

	err = send_magic_sequence(eie, &magic_seq2[0], data);
	if (err < 0)
		return err;

	dev_dbg(&eie->udev->dev, "Completed magic initialization sequence.");

	eie->rate = rate;

	err = submit_init_sync_urbs(eie);
	if (err < 0)
		return err;

	err = submit_init_play_urbs(eie);
	if (err < 0)
		return err;

	wait_event(eie->urbs_flow_wait, test_bit(URBS_FLOWING, &eie->states));

	return 0;
}

static int eie_ppcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	struct eie *eie = substream->private_data;

	if (substream->runtime->rate != eie->rate)
		err = reset_eie(eie, substream->runtime->rate);

	eie->played_frames = 0;
	eie->play_buf_pos = 0;
	substream->runtime->delay = 0;

	return err;
}

static int eie_cpcm_prepare(struct snd_pcm_substream *substream)
{
	int err = 0;
	struct eie *eie = substream->private_data;

	if (substream->runtime->rate != eie->rate)
		err = reset_eie(eie, substream->runtime->rate);

	eie->cap_frames = 0;
	eie->cap_buf_pos = 0;
	substream->runtime->delay = 0; // TODO

	return err;
}


static unsigned int calc_frames_wanted(struct eie *eie)
{
	eie->wanted_idx = 1 - eie->wanted_idx;
	if (eie->rate == 44100)
		return 220 + eie->wanted_idx;
	else
		return 5 * eie->rate / 1000;
}

/** Returns the number of filled frames or negative val for error */
static __must_check int fill_playback_urb(struct eie_playback_urb *epu)
{
	struct snd_pcm_runtime *runtime;
	struct eie *eie = epu->eie;
	struct urb *urb = epu->urb;

	unsigned int frames_wanted = calc_frames_wanted(eie);
	unsigned int frames_elapsed = atomic_xchg(&eie->frames_elapsed, 0);
	unsigned int frames_filled = 0;
	unsigned int bytes_wanted;
	unsigned char *start;

	int i;

	/* adjust frames_wanted by the frames_elapsed from EIE */
	if ((frames_elapsed > frames_wanted - 10)
		&& (frames_elapsed < frames_wanted + 10)) {
		frames_wanted = frames_elapsed;
	}
	bytes_wanted = BYTES_PER_FRAME * frames_wanted;

	if (bytes_wanted > urb->transfer_buffer_length)
		return -EINVAL;

	if (test_bit(PLAYBACK_RUNNING, &eie->states)) {
		runtime = eie->play_substream->runtime;

		if (frames_wanted > runtime->buffer_size)
			return -EINVAL;

		/* copy from ALSA's buffer to urb */
		start = runtime->dma_area + eie->play_buf_pos * BYTES_PER_FRAME;
		if (eie->play_buf_pos + frames_wanted <= runtime->buffer_size) {
			memcpy(urb->transfer_buffer, start, bytes_wanted);
			eie->play_buf_pos += frames_wanted;
		} else {
			unsigned int part_bytes =
				(runtime->buffer_size - eie->play_buf_pos) * BYTES_PER_FRAME;
			memcpy(urb->transfer_buffer, start, part_bytes);
			memcpy(urb->transfer_buffer + part_bytes,
				runtime->dma_area,
				bytes_wanted - part_bytes);
			eie->play_buf_pos += frames_wanted;
		}
		eie->play_buf_pos %= runtime->buffer_size;
		eie->played_frames += frames_wanted;
		runtime->delay += frames_wanted;
		epu->silent = false;
		epu->len = frames_wanted;
	} else {
		if (!epu->silent && frames_wanted <= epu->len) {
			memset(urb->transfer_buffer, 0, bytes_wanted);
			epu->len = frames_wanted;
			epu->silent = true;
		}
	}

	/* adjust iso frame sizes */
	for (i = 0; i < PLAY_PKT_CNT; i++) {
		int len =  frames_wanted * (i+1) / PLAY_PKT_CNT - frames_filled;
		urb->iso_frame_desc[i].offset = frames_filled * BYTES_PER_FRAME;
		urb->iso_frame_desc[i].length = len * BYTES_PER_FRAME;
		frames_filled += len;
	}

	return 0;
}

static bool check_period_elapsed(struct eie *eie)
{
	struct snd_pcm_substream *substream = eie->play_substream;
	if (substream != NULL
		&& eie->played_frames >= substream->runtime->period_size) {
		eie->played_frames %= substream->runtime->period_size;
		return true;
	}
	return false;
}

static int submit_init_play_urbs(struct eie *eie)
{
	unsigned long flags;
	int err;
	int i;

	spin_lock_irqsave(&eie->lock, flags);

	for (i = 0; i < PLAY_URB_CNT; i++) {
		/* init the urb state */
		eie->play_urbs[i].silent = true;
		eie->play_urbs[i].len = 0;

		err = fill_playback_urb(&eie->play_urbs[i]);
		if (err < 0)
			goto out;
		err = usb_submit_urb(eie->play_urbs[i].urb, GFP_ATOMIC);
	}

out:
	spin_unlock_irqrestore(&eie->lock, flags);

	return err;
}

static int eie_ppcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dev_dbg(&eie->udev->dev, "play: SNDRV_PCM_TRIGGER_START");
		set_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		dev_dbg(&eie->udev->dev, "play: SNDRV_PCM_TRIGGER_STOP");
		clear_bit(PLAYBACK_RUNNING, &eie->states);
		return 0;
	default:
		return -EINVAL;
	}
}

static int eie_cpcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct eie *eie = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		dev_dbg(&eie->udev->dev, "cap: SNDRV_PCM_TRIGGER_START");
		set_bit(CAPTURE_RUNNING, &eie->states);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		dev_dbg(&eie->udev->dev, "cap: SNDRV_PCM_TRIGGER_STOP");
		clear_bit(CAPTURE_RUNNING, &eie->states);
		return 0;
	default:
		return -EINVAL;
	}
}


static snd_pcm_uframes_t eie_ppcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->play_buf_pos;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}

static snd_pcm_uframes_t eie_cpcm_pointer(struct snd_pcm_substream *substream)
{
	unsigned long flags;
	struct eie *eie = substream->private_data;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&eie->lock, flags);
	pos = eie->cap_buf_pos;
	spin_unlock_irqrestore(&eie->lock, flags);

	return pos;
}

static struct snd_pcm_ops eie_playback_pcm_ops = {
	.open = eie_ppcm_open,
	.close = eie_ppcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_ppcm_prepare,
	.trigger = eie_ppcm_trigger,
	.pointer = eie_ppcm_pointer,
	.page = snd_pcm_lib_get_vmalloc_page,
	.mmap = snd_pcm_lib_mmap_vmalloc,
};

static struct snd_pcm_ops eie_capture_pcm_ops = {
	.open = eie_cpcm_open,
	.close = eie_cpcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = eie_pcm_hw_params,
	.hw_free = eie_pcm_hw_free,
	.prepare = eie_cpcm_prepare,
	.trigger = eie_cpcm_trigger,
	.pointer = eie_cpcm_pointer,
	.page = snd_pcm_lib_get_vmalloc_page,
	.mmap = snd_pcm_lib_mmap_vmalloc,
};



static void abort_playback(struct eie *eie)
{
	unsigned long flags;

	if (test_bit(PLAYBACK_RUNNING, &eie->states)
		&& eie->play_substream != NULL) {
		snd_pcm_stream_lock_irqsave(eie->play_substream, flags);
		snd_pcm_stop(eie->play_substream, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(eie->play_substream, flags);
	}

	if (test_bit(CAPTURE_RUNNING, &eie->states)
		&& eie->cap_substream != NULL) {
		snd_pcm_stream_lock_irqsave(eie->cap_substream, flags);
		snd_pcm_stop(eie->cap_substream, SNDRV_PCM_STATE_XRUN);
		snd_pcm_stream_unlock_irqrestore(eie->cap_substream, flags);
	}


	spin_lock_irqsave(&eie->lock, flags);
	eie->rate = 0;
	spin_unlock_irqrestore(&eie->lock, flags);
}

static void play_urb_complete(struct urb *urb)
{
	struct eie_playback_urb *epu = urb->context;
	struct eie *eie = epu->eie;
	unsigned long flags;
	int err;
	bool elapsed = false;
	bool abort = false;

	/* for ISO this means that we have been killed or unlinked */
	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Play urb complete. %d", urb->status);
		return;
	}

	/* first URB */
	if (!test_and_set_bit(URBS_FLOWING, &eie->states))
		wake_up(&eie->urbs_flow_wait);

	spin_lock_irqsave(&eie->lock, flags);
	if (!epu->silent
		&& eie->play_substream && eie->play_substream->runtime)
		eie->play_substream->runtime->delay -= epu->len;

	err = fill_playback_urb(epu);
	if (err < 0) {
		abort = true;
		goto err;
	}

	elapsed = test_bit(PLAYBACK_RUNNING, &eie->states)
		&& check_period_elapsed(eie);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		dev_err(&eie->udev->dev, "Cannot resubmit play urb.");
		abort = true;
	}
err:
	spin_unlock_irqrestore(&eie->lock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(eie->play_substream);
	if (abort)
		abort_playback(eie);
}

static void sync_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int i;
	int err;

	/* for ISO this means that we have been killed or unlinked */
	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Sync urb complete. status = %d, packets = %d",
			urb->status, urb->number_of_packets);
		return;
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		if (urb->iso_frame_desc[i].actual_length > 0) {
			unsigned char *buf = urb->transfer_buffer;
			unsigned int offset = urb->iso_frame_desc[i].offset;
			unsigned char d = buf[offset];
			if (d == 0) {
				/* the device did not advance clock */
				abort_playback(eie);
			}
			atomic_add(d, &eie->frames_elapsed);
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0)
		abort_playback(eie);
}

static void cap_urb_complete(struct urb *urb)
{
	struct eie *eie = urb->context;
	int err;

	if (urb->status != 0) {
		dev_dbg(&eie->udev->dev, "Capture urb complete. %d", urb->status);
		return;
	}


	/* TODO: write ALSA part & implement (spin)locking */
	if (test_bit(CAPTURE_RUNNING, &eie->states)) {

		unsigned int i, j;
		unsigned char *buf = urb->transfer_buffer;
		unsigned int frames_rcvd = urb->actual_length / 64;
		bool elapsed;
		struct snd_pcm_runtime *runtime = eie->cap_substream->runtime;

		for (i = 0; i < frames_rcvd; i++) {
			__le32 *out = (__le32 *) (runtime->dma_area + eie->cap_buf_pos * BYTES_PER_FRAME_CAP);
			eie->cap_buf_pos++;
			eie->cap_buf_pos %= runtime->buffer_size;

			out[0] = 0;
			out[1] = 0;
			out[2] = 0;
			out[3] = 0;
			for (j = 0; j < 24; j++) {
				out[0] |= __cpu_to_le32((buf[64*i + j +  0] & 1) << (31-j));
				out[1] |= __cpu_to_le32((buf[64*i + j + 32] & 1) << (31-j));
				out[2] |= __cpu_to_le32((buf[64*i + j +  0] & 2) << (30-j));
				out[3] |= __cpu_to_le32((buf[64*i + j + 32] & 2) << (30-j));
			}
		}

		eie->cap_frames += frames_rcvd;
		elapsed = eie->cap_frames > runtime->period_size;
		if (elapsed) {
			eie->cap_frames = 0;
			snd_pcm_period_elapsed(eie->cap_substream);
		}
	}

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0)
		abort_playback(eie);
}

static void kill_all_urbs(struct eie *eie)
{
	struct urb *urb;
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++) {
		urb = eie->play_urbs[i].urb;
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < SYNC_URB_CNT; i++) {
		urb = eie->sync_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}

	for (i = 0; i < CAP_URB_CNT; i++) {
		urb = eie->cap_urbs[i];
		if (urb)
			usb_kill_urb(urb);
	}

}

static void kill_and_free_urb(struct eie *eie, struct urb **urbp)
{
	struct urb *urb = *urbp;

	if (urb) {
		usb_kill_urb(urb);
		if (urb->transfer_buffer != NULL)
			usb_free_coherent(eie->udev,
				urb->transfer_buffer_length,
				urb->transfer_buffer,
				urb->transfer_dma);
		usb_free_urb(urb);
		(*urbp) = NULL;
	}
}

static void free_usb_related_resources(struct eie *eie)
{
	int i;

	for (i = 0; i < PLAY_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->play_urbs[i].urb);

	for (i = 0; i < SYNC_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->sync_urbs[i]);

	for (i = 0; i < CAP_URB_CNT; i++)
		kill_and_free_urb(eie, &eie->cap_urbs[i]);

	if (eie->ifb) {
		usb_set_intfdata(eie->ifb, NULL);
		usb_driver_release_interface(&eie_driver, eie->ifb);
	}

	if (eie->ifa)
		usb_set_intfdata(eie->ifa, NULL);
}

static int eie_probe(struct usb_interface *interface,
	const struct usb_device_id *usb_id)
{
	char *name = "EIE pro";

	unsigned int card_index;
	struct snd_card *card;
	struct eie *eie;

	char usb_path[32];
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct urb *urb;
	unsigned char *buf;

	int err;
	int i, j;

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index)
		if (enable[card_index] && !(devices_used & (1 << card_index)))
			break;
	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}
	err = snd_card_create(index[card_index], id[card_index], THIS_MODULE,
			      sizeof(*eie), &card);
	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}

	/* card->private_free = eie_card_free; */
	eie = card->private_data;
	eie->udev = interface_to_usbdev(interface);
	eie->card = card;
	eie->card_index = card_index;

	spin_lock_init(&eie->lock);
	init_waitqueue_head(&eie->urbs_flow_wait);

	eie->ifa = interface;
	eie->ifb = usb_ifnum_to_if(eie->udev, 1);
	if (!eie->ifb) {
		err = -ENXIO;
		goto probe_err;
	}
	err = usb_driver_claim_interface(&eie_driver, eie->ifb, eie);
	if (err < 0) {
		eie->ifb = NULL;
		err = -EBUSY;
		goto probe_err;
	}

	err = eie_set_alt_setting(eie);
	if (err < 0)
		goto probe_err;

	iface_desc = eie->ifa->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->play_endpointAddr && usb_endpoint_is_isoc_out(endpoint)) {
			eie->play_packet_size = usb_endpoint_maxp(endpoint);
			eie->play_endpointAddr = endpoint->bEndpointAddress;
			for (j = 0; j < PLAY_URB_CNT; j++) {
				urb = usb_alloc_urb(PLAY_PKT_CNT, GFP_KERNEL);
				if (urb == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}

				eie->play_urbs[j].urb = urb;
				eie->play_urbs[j].eie = eie;

				buf = usb_alloc_coherent(eie->udev,
					PLAY_PKT_CNT * eie->play_packet_size,
					GFP_KERNEL, &urb->transfer_dma);
				if (buf == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}

				urb->dev = eie->udev;
				urb->pipe = usb_sndisocpipe(eie->udev, eie->play_endpointAddr);
				urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
				urb->transfer_buffer = buf;
				/* urb->transfer_dma - set from usb_alloc_coherent */
				urb->transfer_buffer_length = PLAY_PKT_CNT * eie->play_packet_size;
				urb->number_of_packets = PLAY_PKT_CNT;
				urb->interval = 1;
				urb->context = &eie->play_urbs[i];
				urb->complete = play_urb_complete;
			}
		}
	}

	iface_desc = eie->ifb->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!eie->sync_endpointAddr && usb_endpoint_is_isoc_in(endpoint)) {
			eie->sync_packet_size = usb_endpoint_maxp(endpoint);
			eie->sync_endpointAddr = endpoint->bEndpointAddress;
			for (j = 0; j < SYNC_URB_CNT; j++) {
				urb = usb_alloc_urb(1, GFP_KERNEL);
				if (urb == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}
				eie->sync_urbs[j] = urb;

				buf = usb_alloc_coherent(eie->udev, eie->sync_packet_size,
					GFP_KERNEL, &urb->transfer_dma);
				if (buf == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}

				urb->dev = eie->udev;
				urb->pipe = usb_rcvisocpipe(eie->udev, eie->sync_endpointAddr);
				urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
				urb->transfer_buffer = buf;
				/* urb->transfer_dma - set from usb_alloc_coherent */
				urb->transfer_buffer_length = eie->sync_packet_size;
				urb->number_of_packets = 1;
				urb->interval = 1;
				urb->context = eie;
				urb->complete = sync_urb_complete;
				urb->iso_frame_desc[0].offset = 0;
				urb->iso_frame_desc[0].length = eie->sync_packet_size;
			}
		}

		if (!eie->cap_endpointAddr && usb_endpoint_is_bulk_in(endpoint)) {
			eie->cap_endpointAddr = endpoint->bEndpointAddress;
			for (j = 0; j < CAP_URB_CNT; j++) {
				urb = usb_alloc_urb(0, GFP_KERNEL);
				if (urb == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}
				eie->cap_urbs[j] = urb;

				buf = usb_alloc_coherent(eie->udev, usb_endpoint_maxp(endpoint),
					GFP_KERNEL, &urb->transfer_dma);
				if (buf == NULL) {
					err = -ENOMEM;
					goto probe_err;
				}

				usb_fill_bulk_urb(urb, eie->udev, usb_rcvbulkpipe(eie->udev, eie->cap_endpointAddr), buf, usb_endpoint_maxp(endpoint), cap_urb_complete, eie);
			}

		}
	}

	if (!eie->sync_endpointAddr || !eie->play_endpointAddr) {
		dev_err(&eie->udev->dev, "Cannot find expected endpoints.");
		goto probe_err;
	}

	/* prepare the card struct */
	snd_card_set_dev(card, &interface->dev);

	strcpy(card->driver, "EIE");
	strcpy(card->shortname, name);
	usb_make_path(eie->udev, usb_path, sizeof(usb_path));
	snprintf(card->longname, sizeof(card->longname),
		 "Akai EIE pro, at %s, %s speed", usb_path,
		 eie->udev->speed == USB_SPEED_HIGH ? "high" : "full");

	/* TODO: 0 for device index? what is that? */
	err = snd_pcm_new(card, name, 0, 1, 1, &eie->pcm);
	if (err < 0)
		goto probe_err;
	eie->pcm->private_data = eie;
	strcpy(eie->pcm->name, name);
	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_PLAYBACK, &eie_playback_pcm_ops);
	snd_pcm_set_ops(eie->pcm, SNDRV_PCM_STREAM_CAPTURE, &eie_capture_pcm_ops);

	err = snd_card_register(card);
	if (err < 0)
		goto probe_err;

	usb_set_intfdata(interface, eie);
	devices_used |= 1 << card_index;

	mutex_unlock(&devices_mutex);
	return 0;

probe_err:
	free_usb_related_resources(eie);
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void eie_disconnect(struct usb_interface *interface)
{
	struct eie *eie = usb_get_intfdata(interface);
	if (!eie)
		return;

	mutex_lock(&devices_mutex);

	set_bit(DISCONNECTED, &eie->states);
	wake_up(&eie->urbs_flow_wait);

	free_usb_related_resources(eie);
	snd_card_free_when_closed(eie->card);

	mutex_unlock(&devices_mutex);
}


static struct usb_device_id eie_ids[] = {
	{ USB_DEVICE(0x09e8, 0x0010) }, /* EIE pro */
	{ }
};
MODULE_DEVICE_TABLE(usb, eie_ids);

static struct usb_driver eie_driver = {
	.name = "snd-eie",
	.id_table = eie_ids,
	.probe = eie_probe,
	.disconnect = eie_disconnect,
#if 0
	.suspend = eie_suspend,
	.resume = eie_resume,
#endif
};

module_usb_driver(eie_driver);
