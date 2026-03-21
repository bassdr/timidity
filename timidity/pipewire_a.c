/*
    TiMidity++ -- MIDI to WAVE converter and player
    Copyright (C) 1999-2002 Masanao Izumo <mo@goice.co.jp>
    Copyright (C) 1995 Tuukka Toivonen <tt@cgs.fi>

    pipewire_a.c - Copyright (C) 2026

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    pipewire_a.c

    Functions to play sound through PipeWire.

    Like JACK, PipeWire uses a "pull" model which doesn't match TiMidity's
    "push" model. We use an intermediate ring buffer (same approach as
    jack_a.c) to bridge the two. Unlike the JACK driver, we send S16 PCM
    directly to PipeWire (which handles format conversion server-side),
    avoiding the S16-to-float conversion overhead.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <unistd.h>

#include "timidity.h"
#include "common.h"
#include "output.h"
#include "controls.h"
#include "instrum.h"
#include "playmidi.h"
#include "miditrace.h"

/*
 * Retry delay for connecting to the PipeWire daemon.  System services
 * or autostart entries may launch before PipeWire is ready.
 * Retries indefinitely until connected or the process is killed.
 */
#define PW_CONNECT_DELAY_US 1000000	/* 1 second */

static int open_output(void);
static void close_output(void);
static int output_data(char *buf, int32 bytes);
static int acntl(int request, void *arg);
static int detect(void);

#define dpm pipewire_play_mode

PlayMode dpm = {
	DEFAULT_RATE,
	PE_16BIT | PE_SIGNED,
	PF_PCM_STREAM | PF_CAN_TRACE | PF_BUFF_FRAGM_OPT,
	-1,
	{0},
	"PipeWire", 'W',
	NULL,
	open_output,
	close_output,
	output_data,
	acntl,
	detect
};

/*
 * Ring buffer for bridging push (TiMidity) and pull (PipeWire) models.
 * Stores raw S16 interleaved PCM data.
 */

struct ringbuf {
	char *buf;
	int size;		/* total size in bytes */
	long rdptr, wrptr;	/* byte offsets (unbounded) */
};

static void ringbuf_init(struct ringbuf *rb, int size)
{
	rb->buf = (char *)safe_malloc(size);
	rb->size = size;
	rb->rdptr = rb->wrptr = 0;
}

static void ringbuf_destroy(struct ringbuf *rb)
{
	free(rb->buf);
	rb->buf = NULL;
	rb->rdptr = rb->wrptr = 0;
}

static inline int ringbuf_available(struct ringbuf *rb)
{
	return rb->wrptr - rb->rdptr;
}

static inline int ringbuf_empty(struct ringbuf *rb)
{
	return rb->size - ringbuf_available(rb);
}

static inline void ringbuf_clear(struct ringbuf *rb)
{
	rb->rdptr = rb->wrptr = 0;
}

/* Read up to 'bytes' from ring buffer into dst. Returns bytes read. */
static int ringbuf_read(struct ringbuf *rb, char *dst, int bytes)
{
	int avail = ringbuf_available(rb);
	int off, chunk;

	if (bytes > avail)
		bytes = avail;
	if (bytes <= 0)
		return 0;

	off = rb->rdptr % rb->size;
	chunk = rb->size - off;
	if (chunk > bytes)
		chunk = bytes;
	memcpy(dst, rb->buf + off, chunk);
	if (chunk < bytes)
		memcpy(dst + chunk, rb->buf, bytes - chunk);
	rb->rdptr += bytes;
	return bytes;
}

/* Write up to 'bytes' into ring buffer from src. Returns bytes written. */
static int ringbuf_write(struct ringbuf *rb, const char *src, int bytes)
{
	int space = ringbuf_empty(rb);
	int off, chunk;

	if (bytes > space)
		bytes = space;
	if (bytes <= 0)
		return 0;

	off = rb->wrptr % rb->size;
	chunk = rb->size - off;
	if (chunk > bytes)
		chunk = bytes;
	memcpy(rb->buf + off, src, chunk);
	if (chunk < bytes)
		memcpy(rb->buf, src + chunk, bytes - chunk);
	rb->wrptr += bytes;
	return bytes;
}


/*
 * Idle timeout: cork the stream after this many seconds of silence,
 * so the device is released and doesn't show up in pavucontrol.
 */
#define IDLE_TIMEOUT_SEC 3

/*
 * PipeWire context
 */
struct pw_ctx {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;

	int channels;
	int sample_size;	/* bytes per frame (channels * sizeof(int16_t)) */
	int frag_size;		/* fragment size in frames */
	int frags;		/* number of fragments */

	pthread_mutex_t lock;
	pthread_cond_t cond;
	int running;
	int draining;
	int corked;		/* stream is corked (idle) */
	int idle_frames;	/* consecutive empty frames from process cb */
	int idle_threshold;	/* frames of silence before corking */

	struct ringbuf rb;
	long samples_played;	/* frames consumed by PipeWire */
};

static struct pw_ctx ctx;


/*
 * PipeWire process callback - called when PipeWire needs audio data.
 * Runs on the PipeWire realtime thread.
 */
static void on_process(void *userdata)
{
	struct pw_ctx *c = (struct pw_ctx *)userdata;
	struct pw_buffer *pwbuf;
	struct spa_buffer *buf;
	char *dst;
	int n_bytes, n_frames, stride;
	int avail;

	pwbuf = pw_stream_dequeue_buffer(c->stream);
	if (!pwbuf)
		return;

	buf = pwbuf->buffer;
	dst = buf->datas[0].data;
	if (!dst)
		goto done;

	stride = c->sample_size;
	n_frames = buf->datas[0].maxsize / stride;
	if (pwbuf->requested && (uint64_t)n_frames > pwbuf->requested)
		n_frames = pwbuf->requested;

	n_bytes = n_frames * stride;

	pthread_mutex_lock(&c->lock);
	avail = ringbuf_available(&c->rb);

	if (avail >= n_bytes) {
		ringbuf_read(&c->rb, dst, n_bytes);
		c->samples_played += n_frames;
		c->idle_frames = 0;
	} else if (avail > 0) {
		/* partial: read what we have, zero the rest */
		int got = ringbuf_read(&c->rb, dst, avail);
		memset(dst + got, 0, n_bytes - got);
		c->samples_played += avail / stride;
		c->idle_frames = 0;
	} else {
		memset(dst, 0, n_bytes);
		c->idle_frames += n_frames;
		if (!c->corked && c->idle_frames >= c->idle_threshold) {
			c->corked = 1;
			pthread_mutex_unlock(&c->lock);
			buf->datas[0].chunk->offset = 0;
			buf->datas[0].chunk->stride = stride;
			buf->datas[0].chunk->size = n_bytes;
			pw_stream_queue_buffer(c->stream, pwbuf);
			pw_stream_set_active(c->stream, 0);
			return;
		}
	}

	/* wake up the writer thread */
	pthread_cond_signal(&c->cond);
	pthread_mutex_unlock(&c->lock);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_bytes;

done:
	pw_stream_queue_buffer(c->stream, pwbuf);
}

/*
 * Detect PipeWire daemon going away.  When the daemon shuts down the
 * stream transitions to ERROR or UNCONNECTED — wake up the writer
 * thread so output_data() can return instead of blocking forever.
 */
static void on_state_changed(void *userdata,
			     enum pw_stream_state old,
			     enum pw_stream_state state,
			     const char *error)
{
	struct pw_ctx *c = (struct pw_ctx *)userdata;

	/* ignore state changes during intentional teardown */
	if (!c->running)
		return;

	if (state == PW_STREAM_STATE_ERROR ||
	    (old >= PW_STREAM_STATE_PAUSED &&
	     state == PW_STREAM_STATE_UNCONNECTED)) {
		ctl->cmsg(CMSG_ERROR, VERB_NORMAL,
			  "PipeWire: daemon disconnected (%s)",
			  error ? error : "connection lost");
		pthread_mutex_lock(&c->lock);
		c->running = 0;
		pthread_cond_signal(&c->cond);
		pthread_mutex_unlock(&c->lock);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.state_changed = on_state_changed,
};


static int detect(void)
{
	/* Try to initialize PipeWire; if it works, we're available */
	pw_init(NULL, NULL);
	pw_deinit();
	return 1;
}

static int open_output(void)
{
	const struct spa_pod *params[1];
	uint8_t pod_buffer[1024];
	struct spa_pod_builder b;
	enum spa_audio_format spa_fmt;
	int ret_val = 0;

	memset(&ctx, 0, sizeof(ctx));

	pw_init(NULL, NULL);

	/* encoding setup */
	dpm.encoding &= ~(PE_ULAW | PE_ALAW | PE_BYTESWAP);
	if (dpm.encoding & PE_MONO)
		ctx.channels = 1;
	else
		ctx.channels = 2;

	if (dpm.encoding & PE_24BIT) {
		spa_fmt = SPA_AUDIO_FORMAT_S24_LE;
		dpm.encoding |= PE_SIGNED;
		ctx.sample_size = 3 * ctx.channels;
	} else if (dpm.encoding & PE_16BIT) {
		spa_fmt = SPA_AUDIO_FORMAT_S16_LE;
		dpm.encoding |= PE_SIGNED;
		ctx.sample_size = 2 * ctx.channels;
	} else {
		spa_fmt = SPA_AUDIO_FORMAT_U8;
		dpm.encoding &= ~PE_SIGNED;
		ctx.sample_size = ctx.channels;
	}

	/*
	 * Buffer sizing.  PipeWire is a pull-model backend like JACK, so
	 * the global audio_buffer_size default (2048 on Linux) is far too
	 * large for responsive real-time MIDI — it alone adds ~47 ms of
	 * latency at 44.1 kHz.
	 *
	 * When the user hasn't tuned -B, use a PipeWire-specific default
	 * of 256 frames / 2 fragments (~5.8 ms at 44.1 kHz).  If the user
	 * explicitly set the buffer bits via -B n,m we honour that.
	 *
	 * For interactive interfaces (-iA, -ip, etc.) we also shrink the
	 * global audio_buffer_size so the synthesis engine computes in
	 * matching small batches — otherwise the engine's 2048-frame
	 * default adds ~47 ms on top of the PipeWire buffer latency.
	 */
	/*
	 * Ignore extra_param[1] from aq_calc_fragsize() — it is in
	 * bytes, not frames, and would set a wildly oversized fragment.
	 * Only honour the user's explicit -B n,m setting.
	 */
	if (audio_buffer_bits != DEFAULT_AUDIO_BUFFER_BITS)
		ctx.frag_size = audio_buffer_size; /* user set -B n,m */
	else
		ctx.frag_size = 256;               /* PipeWire default */
	if (dpm.extra_param[0] == 0)
		ctx.frags = 2;
	else
		ctx.frags = dpm.extra_param[0];

	/* In interactive mode, shrink the synthesis batch size to match
	 * the PipeWire fragment size, unless the user set -B explicitly. */
	if (audio_buffer_bits == DEFAULT_AUDIO_BUFFER_BITS &&
	    strchr("ApmNP", ctl->id_character)) {
		int bits = 0, s = ctx.frag_size;
		while (s > 1) { s >>= 1; bits++; }
		if (bits > AUDIO_BUFFER_BITS)
			bits = AUDIO_BUFFER_BITS;
		audio_buffer_bits = bits;	/* e.g. 256 -> 8 */
	}

	/*
	 * The ring buffer must be large enough for PipeWire's quantum.
	 * PipeWire's default quantum is 1024 frames; the minimum safe
	 * ring buffer is 2× that.  With -B8,2 the user-requested buffer
	 * would be only 512 frames — far too small.  Clamp to a sane
	 * minimum without overriding the user's fragment geometry.
	 */
	{
		int rb_frames = ctx.frag_size * ctx.frags;
		int min_frames = 4096;		/* safe for quantums up to 2048 */

		if (rb_frames < min_frames) {
			ctl->cmsg(CMSG_WARNING, VERB_NORMAL,
				  "PipeWire: buffer too small (%d frames), "
				  "clamped to %d frames",
				  rb_frames, min_frames);
			rb_frames = min_frames;
		}

		pthread_mutex_init(&ctx.lock, NULL);
		pthread_cond_init(&ctx.cond, NULL);
		ringbuf_init(&ctx.rb, rb_frames * ctx.sample_size);
	}

	/* create threaded loop */
	ctx.loop = pw_thread_loop_new("timidity-pw", NULL);
	if (!ctx.loop) {
		ctl->cmsg(CMSG_ERROR, VERB_NORMAL,
			  "PipeWire: cannot create thread loop");
		return -1;
	}

	/*
	 * Create and connect the PipeWire stream.  pw_stream_new_simple
	 * succeeds even when the daemon is down (it queues internally),
	 * but pw_stream_connect will fail with EHOSTDOWN.  Retry both
	 * together so autostart races are handled gracefully.
	 */
	{
		char latency_str[64];
		int logged = 0;
		snprintf(latency_str, sizeof(latency_str),
			 "%d/%d", ctx.frag_size, dpm.rate);

		for (;;) {
			ctx.stream = pw_stream_new_simple(
				pw_thread_loop_get_loop(ctx.loop),
				"TiMidity++",
				pw_properties_new(
					PW_KEY_MEDIA_TYPE, "Audio",
					PW_KEY_MEDIA_CATEGORY, "Playback",
					PW_KEY_MEDIA_ROLE, "Music",
					PW_KEY_NODE_LATENCY, latency_str,
					NULL),
				&stream_events, &ctx);
			if (!ctx.stream)
				goto retry;

			b = SPA_POD_BUILDER_INIT(pod_buffer,
						 sizeof(pod_buffer));
			params[0] = spa_format_audio_raw_build(&b,
				SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_RAW_INIT(
					.format = spa_fmt,
					.channels = ctx.channels,
					.rate = dpm.rate));

			if (pw_stream_connect(ctx.stream,
					PW_DIRECTION_OUTPUT, PW_ID_ANY,
					PW_STREAM_FLAG_AUTOCONNECT |
					PW_STREAM_FLAG_MAP_BUFFERS,
					params, 1) == 0)
				break;

			pw_stream_destroy(ctx.stream);
			ctx.stream = NULL;
retry:
			if (!logged) {
				ctl->cmsg(CMSG_WARNING, VERB_NORMAL,
					  "PipeWire: daemon not ready, "
					  "retrying...");
				logged = 1;
			}
			usleep(PW_CONNECT_DELAY_US);
		}
	}

	/* start the loop */
	if (pw_thread_loop_start(ctx.loop) < 0) {
		ctl->cmsg(CMSG_ERROR, VERB_NORMAL,
			  "PipeWire: cannot start thread loop");
		pw_stream_destroy(ctx.stream);
		pw_thread_loop_destroy(ctx.loop);
		return -1;
	}

	ctx.running = 1;
	ctx.corked = 0;
	ctx.idle_frames = 0;
	ctx.idle_threshold = dpm.rate * IDLE_TIMEOUT_SEC;
	dpm.fd = 0; /* mark as open */
	return ret_val;
}

static void close_output(void)
{
	if (!ctx.loop)
		return;

	/*
	 * Signal the writer thread to stop, then wake it in case it's
	 * blocked in pthread_cond_wait inside output_data().
	 * This must happen before pw_thread_loop_stop() which joins the
	 * PipeWire thread — otherwise we can deadlock if on_process holds
	 * ctx.lock while output_data is waiting on ctx.cond.
	 */
	pthread_mutex_lock(&ctx.lock);
	ctx.running = 0;
	pthread_cond_signal(&ctx.cond);
	pthread_mutex_unlock(&ctx.lock);

	/* Disconnect the stream first — stops process callbacks */
	pw_thread_loop_lock(ctx.loop);
	pw_stream_disconnect(ctx.stream);
	pw_thread_loop_unlock(ctx.loop);

	pw_thread_loop_stop(ctx.loop);
	pw_stream_destroy(ctx.stream);
	ctx.stream = NULL;
	pw_thread_loop_destroy(ctx.loop);
	ctx.loop = NULL;

	ringbuf_destroy(&ctx.rb);
	pthread_mutex_destroy(&ctx.lock);
	pthread_cond_destroy(&ctx.cond);

	pw_deinit();
	dpm.fd = -1;
}

static int output_data(char *buf, int32 bytes)
{
	int written;

	/* uncork the stream if it was idle-suspended */
	if (ctx.corked) {
		pw_thread_loop_lock(ctx.loop);
		pw_stream_set_active(ctx.stream, 1);
		pw_thread_loop_unlock(ctx.loop);
		pthread_mutex_lock(&ctx.lock);
		ctx.corked = 0;
		ctx.idle_frames = 0;
		pthread_mutex_unlock(&ctx.lock);
	}

	while (bytes > 0) {
		pthread_mutex_lock(&ctx.lock);
		while (ctx.running && ringbuf_empty(&ctx.rb) == 0)
			pthread_cond_wait(&ctx.cond, &ctx.lock);

		if (!ctx.running) {
			pthread_mutex_unlock(&ctx.lock);
			return -1;
		}

		written = ringbuf_write(&ctx.rb, buf, bytes);
		pthread_mutex_unlock(&ctx.lock);

		buf += written;
		bytes -= written;
	}
	return 0;
}

static int acntl(int request, void *arg)
{
	switch (request) {
	case PM_REQ_GETFRAGSIZ:
		if (ctx.frag_size == 0)
			return -1;
		*((int *)arg) = ctx.frag_size * ctx.sample_size;
		return 0;

	case PM_REQ_GETQSIZ:
		*((int *)arg) = ctx.frag_size * ctx.frags * ctx.sample_size;
		return 0;

	case PM_REQ_GETFILLABLE:
		pthread_mutex_lock(&ctx.lock);
		*((int *)arg) = ringbuf_empty(&ctx.rb) / ctx.sample_size;
		pthread_mutex_unlock(&ctx.lock);
		return 0;

	case PM_REQ_GETFILLED:
		pthread_mutex_lock(&ctx.lock);
		*((int *)arg) = ringbuf_available(&ctx.rb) / ctx.sample_size;
		pthread_mutex_unlock(&ctx.lock);
		return 0;

	case PM_REQ_GETSAMPLES:
		pthread_mutex_lock(&ctx.lock);
		*((int *)arg) = ctx.samples_played;
		pthread_mutex_unlock(&ctx.lock);
		return 0;

	case PM_REQ_FLUSH:
		pthread_mutex_lock(&ctx.lock);
		while (ctx.running && ringbuf_available(&ctx.rb) > 0)
			pthread_cond_wait(&ctx.cond, &ctx.lock);
		pthread_mutex_unlock(&ctx.lock);
		/* fall through */
	case PM_REQ_DISCARD:
		pthread_mutex_lock(&ctx.lock);
		ringbuf_clear(&ctx.rb);
		ctx.samples_played = 0;
		pthread_mutex_unlock(&ctx.lock);
		return 0;

	case PM_REQ_PLAY_START:
		pthread_mutex_lock(&ctx.lock);
		ctx.samples_played = 0;
		ringbuf_clear(&ctx.rb);
		pthread_mutex_unlock(&ctx.lock);
		return 0;

	case PM_REQ_PLAY_END:
		return 0;
	}
	return -1;
}
