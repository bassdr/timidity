/*
    TiMidity++ -- MIDI to WAVE converter and player
    Copyright (C) 1999-2004 Masanao Izumo <iz@onicos.co.jp>
    Copyright (C) 1995 Tuukka Toivonen <tt@cgs.fi>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    pipewire_c.c - PipeWire MIDI input interface

    This interface creates a PipeWire MIDI sink that receives MIDI events
    from the PipeWire graph and feeds them into TiMidity's synthesis engine
    in real-time. Works similarly to alsaseq_c.c but uses PipeWire's native
    MIDI transport instead of ALSA sequencer.

    Usage: timidity -iW -OW   (PipeWire MIDI in + PipeWire audio out)
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>
#include <spa/pod/iter.h>

#include "timidity.h"
#include "common.h"
#include "controls.h"
#include "instrum.h"
#include "playmidi.h"
#include "readmidi.h"
#include "recache.h"
#include "output.h"
#include "aq.h"
#include "timer.h"

void readmidi_read_init(void);

#define TICKTIME_HZ 100

/*
 * MIDI event buffer: lock-free queue from PipeWire RT thread to main thread.
 * Each entry stores a raw MIDI message (up to 256 bytes for sysex).
 */
#define MIDI_BUF_SIZE 1024
#define MIDI_MSG_MAX  256

struct midi_msg {
	uint8_t data[MIDI_MSG_MAX];
	uint32_t len;
};

struct midi_ringbuf {
	struct midi_msg msgs[MIDI_BUF_SIZE];
	volatile int rdptr;
	volatile int wrptr;
};

static inline int midibuf_available(struct midi_ringbuf *rb)
{
	return rb->wrptr - rb->rdptr;
}

static inline int midibuf_empty(struct midi_ringbuf *rb)
{
	return MIDI_BUF_SIZE - midibuf_available(rb);
}

static inline void midibuf_push(struct midi_ringbuf *rb,
				const uint8_t *data, uint32_t len)
{
	struct midi_msg *msg;

	if (midibuf_empty(rb) <= 0 || len == 0 || len > MIDI_MSG_MAX)
		return;
	msg = &rb->msgs[rb->wrptr % MIDI_BUF_SIZE];
	memcpy(msg->data, data, len);
	msg->len = len;
	__sync_synchronize();
	rb->wrptr++;
}

static inline struct midi_msg *midibuf_peek(struct midi_ringbuf *rb)
{
	if (midibuf_available(rb) <= 0)
		return NULL;
	return &rb->msgs[rb->rdptr % MIDI_BUF_SIZE];
}

static inline void midibuf_pop(struct midi_ringbuf *rb)
{
	__sync_synchronize();
	rb->rdptr++;
}

static inline void midibuf_clear(struct midi_ringbuf *rb)
{
	rb->rdptr = rb->wrptr = 0;
}


/*
 * PipeWire MIDI context
 */
struct pw_midi_ctx {
	struct pw_thread_loop *loop;
	struct pw_filter *filter;
	void *midi_port;

	int active;
	int running;

	struct midi_ringbuf midibuf;

	/* timing */
	double rate_frac;
	long start_time_base;
	long cur_time_offset;
	int buffer_time_advance;
	long buffer_time_offset;
};

static struct pw_midi_ctx pwctx;
static FILE *outfp;


/*
 * PipeWire filter process callback - runs on RT thread.
 * Reads MIDI events from the PipeWire graph and pushes them to the ring buffer.
 */
static void on_process(void *userdata, struct spa_io_position *position)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)userdata;
	struct pw_buffer *buf;
	struct spa_pod_sequence *seq;
	struct spa_pod_control *c;

	buf = pw_filter_dequeue_buffer(ctx->midi_port);
	if (!buf)
		return;

	if (buf->buffer->n_datas < 1 || !buf->buffer->datas[0].data)
		goto done;

	seq = buf->buffer->datas[0].data;
	if (!spa_pod_is_sequence(&seq->pod))
		goto done;

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		if (c->type == SPA_CONTROL_Midi) {
			uint8_t *data = SPA_POD_BODY(&c->value);
			uint32_t size = SPA_POD_BODY_SIZE(&c->value);
			midibuf_push(&ctx->midibuf, data, size);
		}
	}

done:
	pw_filter_queue_buffer(ctx->midi_port, buf);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};


/*
 * ControlMode callbacks
 */

static int ctl_open(int using_stdin, int using_stdout);
static void ctl_close(void);
static int ctl_read(int32 *valp);
static int cmsg(int type, int verbosity_level, char *fmt, ...);
static void ctl_event(CtlEvent *e);
static int ctl_pass_playing_list(int n, char *args[]);

#define ctl pipewire_control_mode

ControlMode ctl = {
	"PipeWire MIDI interface", 'p',
	"pipewire",
	1, 0, 0,
	0,
	ctl_open,
	ctl_close,
	ctl_pass_playing_list,
	ctl_read,
	NULL,
	cmsg,
	ctl_event
};

static int ctl_open(int using_stdin, int using_stdout)
{
	ctl.opened = 1;
	ctl.flags &= ~(CTLF_LIST_RANDOM | CTLF_LIST_SORT);
	if (using_stdout)
		outfp = stderr;
	else
		outfp = stdout;
	return 0;
}

static void ctl_close(void)
{
	if (!ctl.opened)
		return;

	pwctx.running = 0;

	if (pwctx.filter) {
		pw_thread_loop_lock(pwctx.loop);
		pw_filter_disconnect(pwctx.filter);
		pw_thread_loop_unlock(pwctx.loop);
	}

	if (pwctx.loop) {
		pw_thread_loop_stop(pwctx.loop);
		if (pwctx.filter)
			pw_filter_destroy(pwctx.filter);
		pw_thread_loop_destroy(pwctx.loop);
		pwctx.filter = NULL;
		pwctx.loop = NULL;
	}

	pw_deinit();
	ctl.opened = 0;
}

static int ctl_read(int32 *valp)
{
	return RC_NONE;
}

static int cmsg(int type, int verbosity_level, char *fmt, ...)
{
	va_list ap;

	if ((type == CMSG_TEXT || type == CMSG_INFO || type == CMSG_WARNING) &&
	    ctl.verbosity < verbosity_level)
		return 0;

	if (outfp == NULL)
		outfp = stderr;

	va_start(ap, fmt);
	vfprintf(outfp, fmt, ap);
	fputs(NLS, outfp);
	fflush(outfp);
	va_end(ap);

	return 0;
}

static void ctl_event(CtlEvent *e)
{
}


/*
 * Timing helpers
 */
static long get_current_time(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000L + tv.tv_usec - pwctx.start_time_base;
}

static void update_timestamp(void)
{
	pwctx.cur_time_offset = (long)(get_current_time() * pwctx.rate_frac);
}


/*
 * Process a raw MIDI message and feed it to the synthesis engine
 */
static void seq_play_event(MidiEvent *ev)
{
	int gch = GLOBAL_CHANNEL_EVENT_TYPE(ev->type);
	if (gch || !IS_SET_CHANNELMASK(quietchannels, ev->channel)) {
		ev->time = pwctx.cur_time_offset;
		play_event(ev);
	}
}

static void process_midi_message(const uint8_t *data, uint32_t len)
{
	MidiEvent ev, evm[260];
	int ne, i;

	if (len == 0)
		return;

	ev.type = ME_NONE;
	ev.channel = data[0] & 0x0f;
	ev.a = (len > 1) ? data[1] : 0;
	ev.b = (len > 2) ? data[2] : 0;

	switch (data[0] & 0xf0) {
	case 0x80:
		ev.type = ME_NOTEOFF;
		break;
	case 0x90:
		ev.type = (ev.b) ? ME_NOTEON : ME_NOTEOFF;
		break;
	case 0xa0:
		ev.type = ME_KEYPRESSURE;
		break;
	case 0xb0:
		if (!convert_midi_control_change(ev.channel, ev.a, ev.b, &ev))
			ev.type = ME_NONE;
		break;
	case 0xc0:
		ev.type = ME_PROGRAM;
		break;
	case 0xd0:
		ev.type = ME_CHANNEL_PRESSURE;
		break;
	case 0xe0:
		ev.type = ME_PITCHWHEEL;
		break;
	case 0xf0:
		if (data[0] == 0xf0 && len > 1) {
			/* SysEx - cast away const; parse functions don't modify data */
			if (parse_sysex_event((uint8 *)(data + 1), len - 1, &ev))
				seq_play_event(&ev);
			ne = parse_sysex_event_multi((uint8 *)(data + 1), len - 1, evm);
			if (ne > 0)
				for (i = 0; i < ne; i++)
					seq_play_event(&evm[i]);
			return; /* already handled */
		}
		return; /* ignore other system messages */
	default:
		return;
	}

	if (ev.type != ME_NONE)
		seq_play_event(&ev);
}


/*
 * Server reset and sequencer start/stop
 */
static void server_reset(void)
{
	readmidi_read_init();
	playmidi_stream_init();
	if (free_instruments_afterwards)
		free_instruments(0);
	reduce_voice_threshold = 0;
	pwctx.buffer_time_offset = 0;
}

static int start_sequencer(void)
{
	if (play_mode->acntl(PM_REQ_PLAY_START, NULL) < 0) {
		ctl.cmsg(CMSG_FATAL, VERB_NORMAL,
			 "Couldn't start %s (`%c')",
			 play_mode->id_name, play_mode->id_character);
		return 0;
	}
	pwctx.active = 1;
	pwctx.buffer_time_offset = 0;
	pwctx.cur_time_offset = 0;
	pwctx.start_time_base = 0;
	pwctx.start_time_base = get_current_time();
	return 1;
}

static void stop_playing(void)
{
	if (upper_voices) {
		MidiEvent ev;
		ev.type = ME_EOT;
		ev.a = 0;
		ev.b = 0;
		seq_play_event(&ev);
		aq_flush(0);
	}
}

static void stop_sequencer(void)
{
	stop_playing();
	play_mode->acntl(PM_REQ_PLAY_END, NULL);
	free_instruments(0);
	free_global_mblock();
	pwctx.active = 0;
}


/*
 * Main event loop
 */
static void doit(void)
{
	struct midi_msg *msg;

	for (;;) {
		/* drain all pending MIDI messages */
		while ((msg = midibuf_peek(&pwctx.midibuf)) != NULL) {
			if (!pwctx.active) {
				if (!start_sequencer())
					return;
			}
			update_timestamp();
			process_midi_message(msg->data, msg->len);
			midibuf_pop(&pwctx.midibuf);
		}

		if (pwctx.active) {
			MidiEvent ev;
			update_timestamp();
			ev.time = pwctx.cur_time_offset;
			ev.type = ME_NONE;
			play_event(&ev);
			aq_fill_nonblocking();
		}

		/* small sleep to avoid busy-waiting */
		usleep(pwctx.active ? 1000 : 10000);
	}
}


static RETSIGTYPE sig_timeout(int sig)
{
	signal(SIGALRM, sig_timeout);
}

static RETSIGTYPE sig_reset(int sig)
{
	if (pwctx.active) {
		stop_sequencer();
		server_reset();
	}
	signal(SIGHUP, sig_reset);
}


static int ctl_pass_playing_list(int n, char *args[])
{
	int i, j;

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	printf("TiMidity starting in PipeWire MIDI server mode\n");

	memset(&pwctx, 0, sizeof(pwctx));

	pw_init(NULL, NULL);

	/* create threaded loop */
	pwctx.loop = pw_thread_loop_new("timidity-midi", NULL);
	if (!pwctx.loop) {
		fprintf(stderr, "PipeWire: cannot create thread loop\n");
		return 1;
	}

	/* create filter for MIDI input */
	pwctx.filter = pw_filter_new_simple(
		pw_thread_loop_get_loop(pwctx.loop),
		"TiMidity++",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Midi",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_CLASS, "Midi/Sink",
			NULL),
		&filter_events, &pwctx);

	if (!pwctx.filter) {
		fprintf(stderr, "PipeWire: cannot create filter\n");
		pw_thread_loop_destroy(pwctx.loop);
		return 1;
	}

	/* add MIDI input port */
	pwctx.midi_port = pw_filter_add_port(pwctx.filter,
		PW_DIRECTION_INPUT,
		PW_FILTER_PORT_FLAG_MAP_BUFFERS,
		0,
		pw_properties_new(
			PW_KEY_FORMAT_DSP, "8 bit raw midi",
			PW_KEY_PORT_NAME, "input",
			NULL),
		NULL, 0);

	if (!pwctx.midi_port) {
		fprintf(stderr, "PipeWire: cannot create MIDI port\n");
		pw_filter_destroy(pwctx.filter);
		pw_thread_loop_destroy(pwctx.loop);
		return 1;
	}

	/* connect filter */
	if (pw_filter_connect(pwctx.filter,
			PW_FILTER_FLAG_RT_PROCESS,
			NULL, 0) < 0) {
		fprintf(stderr, "PipeWire: cannot connect filter\n");
		pw_filter_destroy(pwctx.filter);
		pw_thread_loop_destroy(pwctx.loop);
		return 1;
	}

	/* start the PipeWire loop */
	if (pw_thread_loop_start(pwctx.loop) < 0) {
		fprintf(stderr, "PipeWire: cannot start thread loop\n");
		pw_filter_destroy(pwctx.filter);
		pw_thread_loop_destroy(pwctx.loop);
		return 1;
	}

	pwctx.running = 1;
	printf("PipeWire MIDI sink ready, waiting for connections...\n");

	/* initialize synthesis */
	opt_realtime_playing = 1;
	allocate_cache_size = 0;
	current_keysig = (opt_init_keysig == 8) ? 0 : opt_init_keysig;
	note_key_offset = key_adjust;

	if (IS_STREAM_TRACE) {
		play_mode->acntl(PM_REQ_GETFRAGSIZ, &pwctx.buffer_time_advance);
		if (!(play_mode->encoding & PE_MONO))
			pwctx.buffer_time_advance >>= 1;
		if (play_mode->encoding & PE_16BIT)
			pwctx.buffer_time_advance >>= 1;
		aq_set_soft_queue(
			(double)pwctx.buffer_time_advance / play_mode->rate * 1.01,
			0.0);
	} else {
		pwctx.buffer_time_advance = 0;
	}
	pwctx.rate_frac = (double)play_mode->rate / 1000000.0;

	alarm(0);
	signal(SIGALRM, sig_timeout);
	signal(SIGINT, safe_exit);
	signal(SIGTERM, safe_exit);
	signal(SIGHUP, sig_reset);

	i = current_keysig + ((current_keysig < 8) ? 7 : -9), j = 0;
	while (i != 7)
		i += (i < 7) ? 5 : -7, j++;
	j += note_key_offset, j -= floor(j / 12.0) * 12;
	current_freq_table = j;

	for (;;) {
		server_reset();
		doit();
	}

	/* cleanup (unreachable, but for completeness) */
	pwctx.running = 0;
	pw_thread_loop_stop(pwctx.loop);
	pw_filter_destroy(pwctx.filter);
	pw_thread_loop_destroy(pwctx.loop);
	pw_deinit();

	return 0;
}

/*
 * interface_<id>_loader();
 */
ControlMode *interface_p_loader(void)
{
	return &ctl;
}
