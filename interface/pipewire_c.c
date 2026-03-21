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

    Usage: timidity -ip -OW   (PipeWire MIDI in + PipeWire audio out)
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
#include <unistd.h>
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
 * PipeWire connection retry.  The MIDI server always retries
 * indefinitely — only quit_flag (SIGINT/SIGTERM) stops it.
 */
#define PW_CONNECT_DELAY_US 1000000	/* 1 second */

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
#define PW_MIDI_MAX_SRC_PORTS  64
#define PW_MIDI_MAX_AUTO_LINKS 16

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

	/* auto-linking: auxiliary core connection for registry + link mgmt */
	struct pw_context   *aux_ctx;
	struct pw_core      *aux_core;
	struct pw_registry  *registry;
	struct spa_hook      aux_core_listener;
	struct spa_hook      registry_listener;
	struct spa_hook      filter_state_listener;

	uint32_t our_node_id;	/* node ID of our filter (0 = unknown) */
	uint32_t our_port_id;	/* global ID of our MIDI input port (0 = unknown) */

	/* all MIDI ports seen in the registry */
	struct {
		uint32_t id;       /* global object ID */
		uint32_t node_id;  /* owning node ID */
		int is_source;     /* 1 = direction "out" (sends MIDI) */
	} midi_ports[PW_MIDI_MAX_SRC_PORTS];
	int n_midi_ports;

	/* source port IDs we have linked (to avoid duplicates on hot-plug) */
	uint32_t linked_ports[PW_MIDI_MAX_AUTO_LINKS];
	int n_linked_ports;

	/* one-shot sync for initial registry dump */
	int init_sync_seq;
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

/*
 * quit_flag: set by SIGINT/SIGTERM — exit for real.
 * pw_disconnected: set by PipeWire disconnect callback — reconnect loop.
 */
static volatile sig_atomic_t quit_flag;
static volatile sig_atomic_t pw_disconnected;

static void on_filter_state(void *data,
			    enum pw_filter_state old,
			    enum pw_filter_state state,
			    const char *error)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)data;

	/* ignore state changes during intentional teardown */
	if (!ctx->running)
		return;

	if (state == PW_FILTER_STATE_ERROR ||
	    (old >= PW_FILTER_STATE_PAUSED &&
	     state == PW_FILTER_STATE_UNCONNECTED)) {
		fprintf(stderr, "PipeWire: daemon disconnected (%s)\n",
			error ? error : "connection lost");
		pw_disconnected = 1;
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
	.state_changed = on_filter_state,
};


/*
 * Check if we already linked a given source port (to avoid duplicates).
 */
static int is_port_linked(struct pw_midi_ctx *ctx, uint32_t src_port_id)
{
	int i;
	for (i = 0; i < ctx->n_linked_ports; i++) {
		if (ctx->linked_ports[i] == src_port_id)
			return 1;
	}
	return 0;
}

/*
 * Auto-linking: create a PipeWire link from a MIDI source port to our
 * filter's input port.  Must be called from the PW event thread
 * (i.e. while the thread-loop lock is held by the event thread).
 *
 * Uses object.linger=true so the link persists server-side after we
 * destroy the local proxy.  PipeWire automatically removes lingering
 * links when either endpoint port disappears (e.g. device unplugged).
 * This is the same approach used by pw-link(1).
 */
static void create_midi_link(struct pw_midi_ctx *ctx,
			     uint32_t src_port_id,
			     uint32_t dst_port_id)
{
	char src_str[16], dst_str[16];
	struct pw_proxy *proxy;

	if (is_port_linked(ctx, src_port_id))
		return;

	snprintf(src_str, sizeof(src_str), "%u", src_port_id);
	snprintf(dst_str, sizeof(dst_str), "%u", dst_port_id);

	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, src_str),
		SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT,  dst_str),
		SPA_DICT_ITEM_INIT("object.linger", "true"),
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);

	proxy = pw_core_create_object(ctx->aux_core,
				      "link-factory",
				      PW_TYPE_INTERFACE_Link,
				      PW_VERSION_LINK,
				      &dict, 0);
	if (!proxy) {
		fprintf(stderr,
			"PipeWire MIDI: failed to link port %u -> %u\n",
			src_port_id, dst_port_id);
		return;
	}
	fprintf(stderr,
		"PipeWire MIDI: auto-linked source port %u to TiMidity input\n",
		src_port_id);

	if (ctx->n_linked_ports < PW_MIDI_MAX_AUTO_LINKS)
		ctx->linked_ports[ctx->n_linked_ports++] = src_port_id;

	/* Drop the local proxy; the lingering link lives server-side. */
	pw_proxy_destroy(proxy);
}

static void try_link_sources(struct pw_midi_ctx *ctx)
{
	int i;
	if (ctx->our_port_id == 0)
		return;
	for (i = 0; i < ctx->n_midi_ports; i++) {
		if (ctx->midi_ports[i].is_source &&
		    ctx->midi_ports[i].node_id != ctx->our_node_id)
			create_midi_link(ctx,
					 ctx->midi_ports[i].id,
					 ctx->our_port_id);
	}
}

/*
 * Registry event: fired for every global object (node, port, link…).
 * We track MIDI ports here and create links when both sides are known.
 */
static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, const char *type,
				  uint32_t version,
				  const struct spa_dict *props)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)data;
	const char *direction, *format, *node_id_str;
	uint32_t node_id;
	int is_source;

	if (strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
		return;

	direction   = spa_dict_lookup(props, "port.direction");
	format      = spa_dict_lookup(props, "format.dsp");
	node_id_str = spa_dict_lookup(props, "node.id");

	if (!direction || !format || !node_id_str)
		return;
	if (strcmp(format, "8 bit raw midi") != 0)
		return;

	is_source = (strcmp(direction, "out") == 0);
	node_id   = (uint32_t)atoi(node_id_str);

	/* store for later use */
	if (ctx->n_midi_ports < PW_MIDI_MAX_SRC_PORTS) {
		ctx->midi_ports[ctx->n_midi_ports].id        = id;
		ctx->midi_ports[ctx->n_midi_ports].node_id   = node_id;
		ctx->midi_ports[ctx->n_midi_ports].is_source = is_source;
		ctx->n_midi_ports++;
	}

	if (is_source) {
		/* New MIDI source: link it now if we already know our port */
		if (ctx->our_port_id != 0 && node_id != ctx->our_node_id)
			create_midi_link(ctx, id, ctx->our_port_id);
	} else {
		/* "in" direction port: check if this is ours */
		if (ctx->our_node_id != 0 && node_id == ctx->our_node_id
		    && ctx->our_port_id == 0) {
			ctx->our_port_id = id;
			try_link_sources(ctx);
		}
	}
}

/*
 * Registry remove: a global object was destroyed (e.g. MIDI controller
 * unplugged).  Clean up our tracking arrays so slots can be reused
 * when the device reappears with new IDs.
 */
static void registry_event_global_remove(void *data, uint32_t id)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)data;
	int i;

	/* remove from midi_ports[] */
	for (i = 0; i < ctx->n_midi_ports; i++) {
		if (ctx->midi_ports[i].id == id) {
			ctx->midi_ports[i] =
				ctx->midi_ports[--ctx->n_midi_ports];
			break;
		}
	}

	/* remove from linked_ports[] so hot-plug re-creates the link */
	for (i = 0; i < ctx->n_linked_ports; i++) {
		if (ctx->linked_ports[i] == id) {
			ctx->linked_ports[i] =
				ctx->linked_ports[--ctx->n_linked_ports];
			break;
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

/*
 * Core done callback: signals the main thread that the initial registry
 * enumeration is complete.
 */
static void aux_core_done(void *data, uint32_t id, int seq)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)data;
	if (seq == ctx->init_sync_seq)
		pw_thread_loop_signal(ctx->loop, false);
}

static const struct pw_core_events aux_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = aux_core_done,
};

/*
 * Filter state-changed callback (added as an extra listener after creation).
 * Once the filter transitions to PAUSED (i.e. it is bound to the graph),
 * we can read our node ID and start linking.
 */
static void filter_state_changed(void *data,
				 enum pw_filter_state old,
				 enum pw_filter_state state,
				 const char *error)
{
	struct pw_midi_ctx *ctx = (struct pw_midi_ctx *)data;
	int i;

	if (state != PW_FILTER_STATE_PAUSED || ctx->our_node_id != 0)
		return;

	ctx->our_node_id = pw_filter_get_node_id(ctx->filter);
	if (ctx->our_node_id == 0)
		return;

	/* Scan already-registered ports for our own input port */
	for (i = 0; i < ctx->n_midi_ports; i++) {
		if (!ctx->midi_ports[i].is_source &&
		    ctx->midi_ports[i].node_id == ctx->our_node_id) {
			ctx->our_port_id = ctx->midi_ports[i].id;
			break;
		}
	}

	if (ctx->our_port_id != 0)
		try_link_sources(ctx);
}

static const struct pw_filter_events filter_state_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = filter_state_changed,
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

/*
 * Tear down the PipeWire filter, autolink, and thread loop.
 * Leaves pw_init/pw_deinit to the caller.
 */
static void pw_teardown(void)
{
	pwctx.running = 0;

	if (!pwctx.loop)
		return;

	pw_thread_loop_lock(pwctx.loop);

	pwctx.n_linked_ports = 0;
	pwctx.n_midi_ports = 0;
	pwctx.our_node_id = 0;
	pwctx.our_port_id = 0;

	/* disconnect the auxiliary core */
	if (pwctx.registry) {
		pw_proxy_destroy((struct pw_proxy *)pwctx.registry);
		pwctx.registry = NULL;
	}
	if (pwctx.aux_core) {
		pw_core_disconnect(pwctx.aux_core);
		pwctx.aux_core = NULL;
	}
	if (pwctx.aux_ctx) {
		pw_context_destroy(pwctx.aux_ctx);
		pwctx.aux_ctx = NULL;
	}

	if (pwctx.filter)
		pw_filter_disconnect(pwctx.filter);

	pw_thread_loop_unlock(pwctx.loop);

	pw_thread_loop_stop(pwctx.loop);
	if (pwctx.filter)
		pw_filter_destroy(pwctx.filter);
	pw_thread_loop_destroy(pwctx.loop);
	pwctx.filter = NULL;
	pwctx.loop   = NULL;
}

/*
 * Connect to PipeWire: create thread loop, filter, MIDI port, and
 * optionally set up auto-linking.
 *
 * Retries indefinitely until connected or quit_flag is set.
 *
 * Returns 0 on success, -1 on failure or quit_flag.
 */
static int pw_setup(void)
{
	/* create threaded loop */
	pwctx.loop = pw_thread_loop_new("timidity-midi", NULL);
	if (!pwctx.loop) {
		fprintf(stderr, "PipeWire: cannot create thread loop\n");
		return -1;
	}

	/* create and connect filter (retry until daemon is up) */
	{
		int logged = 0;

		for (;;) {
			if (quit_flag)
				goto fail;

			pwctx.filter = pw_filter_new_simple(
				pw_thread_loop_get_loop(pwctx.loop),
				"TiMidity++",
				pw_properties_new(
					PW_KEY_MEDIA_TYPE, "Midi",
					PW_KEY_MEDIA_CATEGORY, "Capture",
					PW_KEY_MEDIA_CLASS, "Midi/Sink",
					PW_KEY_NODE_LATENCY, "256/48000",
					NULL),
				&filter_events, &pwctx);
			if (!pwctx.filter)
				goto retry_filter;

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
				pw_filter_destroy(pwctx.filter);
				pwctx.filter = NULL;
				goto retry_filter;
			}

			if (pw_filter_connect(pwctx.filter,
					PW_FILTER_FLAG_RT_PROCESS,
					NULL, 0) == 0)
				break;

			pw_filter_destroy(pwctx.filter);
			pwctx.filter = NULL;
retry_filter:
			if (!logged) {
				fprintf(stderr,
					"PipeWire: waiting for daemon...\n");
				logged = 1;
			}
			usleep(PW_CONNECT_DELAY_US);
		}
	}

	/* start the PipeWire loop */
	if (pw_thread_loop_start(pwctx.loop) < 0) {
		fprintf(stderr, "PipeWire: cannot start thread loop\n");
		goto fail;
	}

	/* set up auto-linking if requested */
	if (ctl.flags & CTLF_MIDI_AUTOLINK) {
		pw_thread_loop_lock(pwctx.loop);

		pwctx.aux_ctx = pw_context_new(
			pw_thread_loop_get_loop(pwctx.loop), NULL, 0);

		if (pwctx.aux_ctx)
			pwctx.aux_core = pw_context_connect(
				pwctx.aux_ctx, NULL, 0);

		if (pwctx.aux_core) {
			pw_core_add_listener(pwctx.aux_core,
					     &pwctx.aux_core_listener,
					     &aux_core_events, &pwctx);

			pwctx.registry = pw_core_get_registry(
				pwctx.aux_core, PW_VERSION_REGISTRY, 0);
			pw_registry_add_listener(pwctx.registry,
						 &pwctx.registry_listener,
						 &registry_events, &pwctx);

			pw_filter_add_listener(pwctx.filter,
					       &pwctx.filter_state_listener,
					       &filter_state_events, &pwctx);

			/* wait for initial registry enumeration */
			pwctx.init_sync_seq = pw_core_sync(
				pwctx.aux_core, PW_ID_CORE, 0);
			pw_thread_loop_wait(pwctx.loop);
		} else {
			fprintf(stderr,
				"PipeWire MIDI: no auxiliary core, "
				"auto-link disabled\n");
		}

		pw_thread_loop_unlock(pwctx.loop);
	}

	pwctx.running = 1;
	pw_disconnected = 0;
	return 0;

fail:
	if (pwctx.filter)
		pw_filter_destroy(pwctx.filter);
	if (pwctx.loop)
		pw_thread_loop_destroy(pwctx.loop);
	pwctx.filter = NULL;
	pwctx.loop = NULL;
	return -1;
}

static void ctl_close(void)
{
	if (!ctl.opened)
		return;

	pw_teardown();
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
 * Graceful shutdown: the signal handler only sets a flag; the main
 * loop checks it and exits cleanly so that all PipeWire teardown
 * happens in normal (non-signal) context.  Calling pw_thread_loop_lock,
 * pthread_mutex_lock, etc. from a signal handler is undefined behaviour
 * and can deadlock.
 *
 * quit_flag is declared earlier (near on_filter_state) so that both
 * the PipeWire disconnect callback and signal handlers can use it.
 */

static RETSIGTYPE sig_quit(int sig)
{
	quit_flag = 1;
}

/*
 * Main event loop
 */
static void doit(void)
{
	struct midi_msg *msg;

	for (;;) {
		if (quit_flag || pw_disconnected)
			return;

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

	/* initialize synthesis (one-time) */
	opt_realtime_playing = 1;
	allocate_cache_size = 0;
	current_keysig = (opt_init_keysig == 8) ? 0 : opt_init_keysig;
	note_key_offset = key_adjust;

	alarm(0);
	signal(SIGALRM, sig_timeout);
	signal(SIGINT, sig_quit);
	signal(SIGTERM, sig_quit);
	signal(SIGHUP, sig_reset);

	i = current_keysig + ((current_keysig < 8) ? 7 : -9), j = 0;
	while (i != 7)
		i += (i < 7) ? 5 : -7, j++;
	j += note_key_offset, j -= floor(j / 12.0) * 12;
	current_freq_table = j;

	/*
	 * Main loop.  On PipeWire daemon restart we tear down and
	 * reconnect instead of exiting, so init scripts and autostart
	 * environments work reliably without requiring -ipD.
	 * Only quit_flag (SIGINT/SIGTERM) causes a clean exit.
	 */
	for (;;) {
		if (pw_setup() < 0)
			break;

		/*
		 * (Re)open the audio output.  On the first iteration the
		 * framework has already opened it, but after a daemon-mode
		 * reconnect we closed it (its PipeWire stream was dead)
		 * and need to reopen.
		 */
		if (play_mode->fd == -1) {
			if (play_mode->open_output() < 0) {
				ctl.cmsg(CMSG_FATAL, VERB_NORMAL,
					 "Couldn't open %s (`%c')",
					 play_mode->id_name,
					 play_mode->id_character);
				pw_teardown();
				break;
			}
		}

		printf("PipeWire MIDI synthesizer ready\n");

		if (IS_STREAM_TRACE) {
			play_mode->acntl(PM_REQ_GETFRAGSIZ,
					 &pwctx.buffer_time_advance);
			if (!(play_mode->encoding & PE_MONO))
				pwctx.buffer_time_advance >>= 1;
			if (play_mode->encoding & PE_16BIT)
				pwctx.buffer_time_advance >>= 1;
			aq_set_soft_queue(
				(double)pwctx.buffer_time_advance /
				play_mode->rate * 1.01, 0.0);
		} else {
			pwctx.buffer_time_offset = 0;
		}
		pwctx.rate_frac = (double)play_mode->rate / 1000000.0;

		for (;;) {
			server_reset();
			doit();
			if (quit_flag || pw_disconnected)
				break;
		}

		/* stop synthesis and close audio output (its PW stream
		 * is dead too) before tearing down the MIDI filter */
		if (pwctx.active)
			stop_sequencer();
		play_mode->close_output();

		pw_teardown();

		if (quit_flag)
			break;

		/* wait and reconnect */
		fprintf(stderr,
			"PipeWire: reconnecting...\n");
		pw_disconnected = 0;
	}

	/* clean shutdown from normal context */
	safe_exit(0);
	return 0;
}

/*
 * interface_<id>_loader();
 */
ControlMode *interface_p_loader(void)
{
	return &ctl;
}
