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
    jack_a.c) to bridge the two. By default we send F32 (float) samples
    directly, which is PipeWire's native processing format — avoiding a
    final integer-to-float conversion server-side.
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

#include <signal.h>
#include <unistd.h>

#include "timidity.h"
#include "common.h"
#include "output.h"
#include "controls.h"
#include "instrum.h"
#include "playmidi.h"
#include "miditrace.h"

extern VOLATILE int intr;

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
	PE_F32BIT | PE_SIGNED,
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
 * Stores raw interleaved PCM data (F32, S24, S16, or U8).
 */

struct ringbuf {
	char *buf;
	int size;		/* total size in bytes, always power of 2 */
	int mask;		/* size - 1, for fast modulo */
	long rdptr, wrptr;	/* byte offsets (unbounded) */
};

/* Round up to the next power of 2 (no-op if already power of 2). */
static int next_power_of_2(int v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v + 1;
}

static void ringbuf_init(struct ringbuf *rb, int size)
{
	size = next_power_of_2(size);
	rb->buf = (char *)safe_malloc(size);
	rb->size = size;
	rb->mask = size - 1;
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

	off = rb->rdptr & rb->mask;
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

	off = rb->wrptr & rb->mask;
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
	int sample_size;	/* bytes per frame (channels * bytes_per_sample) */
	int frag_size;		/* fragment size in frames */
	int frags;		/* number of fragments */

	pthread_mutex_t lock;
	pthread_cond_t cond;
	int running;		/* accessed via __atomic builtins */
	int draining;
	int corked;		/* accessed via __atomic builtins */
	int idle_frames;	/* consecutive empty frames from process cb */
	int idle_threshold;	/* frames of silence before corking */

	struct ringbuf rb;
	long samples_played;	/* frames consumed by PipeWire */

	/*
	 * Underrun recovery: store the last successfully read audio chunk.
	 * On underrun, replay it with a fade-out instead of outputting
	 * silence.  A faded repeat of ~5ms audio is perceptually invisible;
	 * a silence gap produces an audible click.
	 */
	char *last_chunk;	/* last_chunk_size bytes, allocated at open */
	int last_chunk_size;	/* bytes (frag_size * sample_size) */
	int last_chunk_valid;	/* 1 if last_chunk contains real audio */
	int underrun_faded;	/* 1 if we already faded on this underrun */
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

	/*
	 * Use trylock: this is a realtime callback and must never block.
	 * If the main thread holds the lock (e.g. inside output_data)
	 * and a signal fires, close_output needs pw_thread_loop_lock
	 * which waits for this callback to return.  Blocking here on
	 * ctx.lock would deadlock.  Output silence instead.
	 */
	if (pthread_mutex_trylock(&c->lock) != 0) {
		memset(dst, 0, n_bytes);
		goto fill_done;
	}

	avail = ringbuf_available(&c->rb);

	if (avail >= n_bytes) {
		ringbuf_read(&c->rb, dst, n_bytes);
		c->samples_played += n_frames;
		c->idle_frames = 0;
		c->underrun_faded = 0;
		/* save for underrun recovery */
		if (c->last_chunk && n_bytes <= c->last_chunk_size) {
			memcpy(c->last_chunk, dst, n_bytes);
			c->last_chunk_valid = 1;
		}
	} else if (avail > 0) {
		/* partial: read what we have, zero the rest */
		int got = ringbuf_read(&c->rb, dst, avail);
		memset(dst + got, 0, n_bytes - got);
		c->samples_played += avail / stride;
		c->idle_frames = 0;
		c->underrun_faded = 0;
	} else if (c->last_chunk_valid && !c->underrun_faded) {
		/*
		 * Underrun: replay last chunk with a linear fade-out.
		 * This masks the gap perceptually — a faded repeat of
		 * the previous quantum sounds like natural decay rather
		 * than a harsh click from silence insertion.
		 */
		int copy = (n_bytes <= c->last_chunk_size)
			   ? n_bytes : c->last_chunk_size;
		int bps = stride / c->channels;  /* bytes per sample */
		int i;

		memcpy(dst, c->last_chunk, copy);
		if (copy < n_bytes)
			memset(dst + copy, 0, n_bytes - copy);

		/* apply linear fade-out across the chunk */
		for (i = 0; i < n_frames; i++) {
			float gain = 1.0f - (float)i / n_frames;
			int ch;
			for (ch = 0; ch < c->channels; ch++) {
				int off = (i * c->channels + ch) * bps;
				if (bps == 2) {
					int16_t *s = (int16_t *)(dst + off);
					*s = (int16_t)(*s * gain);
				} else if (bps == 4) {
					int32_t *s = (int32_t *)(dst + off);
					*s = (int32_t)(*s * gain);
				}
				/* U8: rare, skip fade for simplicity */
			}
		}
		c->underrun_faded = 1;
		c->idle_frames = 0;
	} else {
		memset(dst, 0, n_bytes);
		c->idle_frames += n_frames;
		if (!__atomic_load_n(&c->corked, __ATOMIC_ACQUIRE) &&
		    c->idle_frames >= c->idle_threshold) {
			__atomic_store_n(&c->corked, 1, __ATOMIC_RELEASE);
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

fill_done:
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
	if (!__atomic_load_n(&c->running, __ATOMIC_ACQUIRE))
		return;

	if (state == PW_STREAM_STATE_ERROR ||
	    (old >= PW_STREAM_STATE_PAUSED &&
	     state == PW_STREAM_STATE_UNCONNECTED)) {
		ctl->cmsg(CMSG_ERROR, VERB_NORMAL,
			  "PipeWire: daemon disconnected (%s)",
			  error ? error : "connection lost");
		pthread_mutex_lock(&c->lock);
		__atomic_store_n(&c->running, 0, __ATOMIC_RELEASE);
		pthread_cond_signal(&c->cond);
		pthread_mutex_unlock(&c->lock);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.state_changed = on_state_changed,
};


/*
 * Signal handler for SIGINT/SIGTERM.  Instead of calling safe_exit()
 * (which does full teardown from signal context and deadlocks on our
 * condvar/mutex), just set the existing `intr` flag and wake up
 * output_data().  The playback loop checks `intr` in compute_data()
 * and unwinds normally, reaching close_output() on the main thread.
 */
static void pw_sigterm_exit(int sig)
{
	char s[4];
	ssize_t dummy;

	dummy = write(2, "Terminated sig=0x", 17);
	s[0] = "0123456789abcdef"[(sig >> 4) & 0xf];
	s[1] = "0123456789abcdef"[sig & 0xf];
	s[2] = '\n';
	dummy += write(2, s, 3);
	(void)dummy;

	__atomic_store_n(&intr, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&ctx.running, 0, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&ctx.cond);
}


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

	if (dpm.encoding & PE_F32BIT) {
		spa_fmt = SPA_AUDIO_FORMAT_F32_LE;
		dpm.encoding |= PE_SIGNED;
		ctx.sample_size = 4 * ctx.channels;
	} else if (dpm.encoding & PE_24BIT) {
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
	 * Buffer sizing for interactive (real-time MIDI) mode.
	 *
	 * Latency is controlled entirely by PipeWire's graph quantum,
	 * configured in pipewire.conf (default.clock.quantum).  The -B
	 * flag is ignored — it was a workaround for the old push-model
	 * architecture where the synthesis loop and audio output ran
	 * on independent timers.  With demand-driven synthesis the
	 * main loop paces itself to PipeWire's pull callbacks via
	 * PM_REQ_OUTPUT_READY, so a user-tunable buffer knob adds
	 * nothing except confusion.
	 *
	 * Fragment size (frag_size) sets the PipeWire latency hint
	 * and the synthesis batch size.  256 frames (~5.3 ms at 48 kHz)
	 * is a good default — small enough for responsive MIDI, large
	 * enough that synthesis overhead stays low.
	 *
	 * The ring buffer is sized to 3× frag_size: one quantum being
	 * read by the RT callback, one being written by synthesis, and
	 * one spare for scheduling jitter.  With SCHED_FIFO + mlockall
	 * this is generous; without RT scheduling, the underrun
	 * recovery (last-chunk fade) masks occasional glitches.
	 *
	 * For non-interactive use (file playback to PipeWire), -B
	 * still works as before.
	 */
	if (strchr("ApmNP", ctl->id_character)) {
		/* interactive mode: ignore -B, auto-compute everything */
		int bits, s;

		if (audio_buffer_bits != DEFAULT_AUDIO_BUFFER_BITS ||
		    dpm.extra_param[0] != 0)
			ctl->cmsg(CMSG_WARNING, VERB_NORMAL,
				  "PipeWire: -B ignored in interactive mode; "
				  "set latency via PipeWire quantum "
				  "(default.clock.quantum in pipewire.conf)");

		ctx.frag_size = 256;
		ctx.frags = 3;

		/* shrink synthesis batch to match fragment size */
		s = ctx.frag_size;
		bits = 0;
		while (s > 1) { s >>= 1; bits++; }
		if (bits > AUDIO_BUFFER_BITS)
			bits = AUDIO_BUFFER_BITS;
		audio_buffer_bits = bits;	/* 256 -> 8 */
	} else {
		/* non-interactive (file playback): honour -B as before */
		if (audio_buffer_bits != DEFAULT_AUDIO_BUFFER_BITS)
			ctx.frag_size = audio_buffer_size;
		else
			ctx.frag_size = 256;
		if (dpm.extra_param[0] == 0)
			ctx.frags = 2;
		else
			ctx.frags = dpm.extra_param[0];
	}

	{
		int rb_frames = ctx.frag_size * ctx.frags;

		pthread_mutex_init(&ctx.lock, NULL);
		pthread_cond_init(&ctx.cond, NULL);
		ringbuf_init(&ctx.rb, rb_frames * ctx.sample_size);

		ctx.last_chunk_size = ctx.frag_size * ctx.sample_size;
		ctx.last_chunk = (char *)safe_malloc(ctx.last_chunk_size);
		ctx.last_chunk_valid = 0;
		ctx.underrun_faded = 0;
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

	__atomic_store_n(&ctx.running, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&ctx.corked, 0, __ATOMIC_RELEASE);
	ctx.idle_frames = 0;
	ctx.idle_threshold = dpm.rate * IDLE_TIMEOUT_SEC;

	/*
	 * Override the default signal handler.  The default sigterm_exit
	 * calls safe_exit() which does full teardown from signal context
	 * and deadlocks on our condvar.  Our handler just sets `intr`
	 * and wakes output_data(); the playback loop notices and unwinds
	 * normally through close_output() on the main thread.
	 */
	signal(SIGINT, pw_sigterm_exit);
	signal(SIGTERM, pw_sigterm_exit);

	dpm.fd = 0; /* mark as open */
	return ret_val;
}

static void close_output(void)
{
	if (!ctx.loop)
		return;

	/*
	 * close_output is now only called from the normal shutdown path
	 * (not from a signal handler), so regular locking is safe.
	 */
	pthread_mutex_lock(&ctx.lock);
	__atomic_store_n(&ctx.running, 0, __ATOMIC_RELEASE);
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
	free(ctx.last_chunk);
	ctx.last_chunk = NULL;
	ctx.last_chunk_valid = 0;
	pthread_mutex_destroy(&ctx.lock);
	pthread_cond_destroy(&ctx.cond);

	pw_deinit();
	dpm.fd = -1;
}

static int output_data(char *buf, int32 bytes)
{
	int written;

	/* uncork the stream if it was idle-suspended */
	if (__atomic_load_n(&ctx.corked, __ATOMIC_ACQUIRE)) {
		pw_thread_loop_lock(ctx.loop);
		pw_stream_set_active(ctx.stream, 1);
		pw_thread_loop_unlock(ctx.loop);
		pthread_mutex_lock(&ctx.lock);
		__atomic_store_n(&ctx.corked, 0, __ATOMIC_RELEASE);
		ctx.idle_frames = 0;
		pthread_mutex_unlock(&ctx.lock);
	}

	while (bytes > 0) {
		pthread_mutex_lock(&ctx.lock);
		while (__atomic_load_n(&ctx.running, __ATOMIC_ACQUIRE) &&
		       ringbuf_empty(&ctx.rb) == 0)
			pthread_cond_wait(&ctx.cond, &ctx.lock);

		if (!__atomic_load_n(&ctx.running, __ATOMIC_ACQUIRE)) {
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
		while (__atomic_load_n(&ctx.running, __ATOMIC_ACQUIRE) &&
		       ringbuf_available(&ctx.rb) > 0)
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

	case PM_REQ_OUTPUT_READY:
		/*
		 * Block until the ring buffer has room for at least one
		 * fragment.  This lets the synthesis loop pace itself to
		 * the audio callback without busy-waiting or fixed sleeps.
		 */
		{
			int frag_bytes = ctx.frag_size * ctx.sample_size;
			struct timespec ts;

			pthread_mutex_lock(&ctx.lock);
			while (__atomic_load_n(&ctx.running, __ATOMIC_ACQUIRE) &&
			       ringbuf_empty(&ctx.rb) < frag_bytes) {
				/*
				 * Use timedwait with a 50ms ceiling so we
				 * don't hang forever if the audio callback
				 * stops firing (e.g. PipeWire disconnect).
				 */
				clock_gettime(CLOCK_REALTIME, &ts);
				ts.tv_nsec += 50000000;  /* 50ms */
				if (ts.tv_nsec >= 1000000000) {
					ts.tv_nsec -= 1000000000;
					ts.tv_sec++;
				}
				pthread_cond_timedwait(&ctx.cond, &ctx.lock, &ts);
			}
			pthread_mutex_unlock(&ctx.lock);
			return __atomic_load_n(&ctx.running, __ATOMIC_ACQUIRE) ? 0 : -1;
		}
	}
	return -1;
}
