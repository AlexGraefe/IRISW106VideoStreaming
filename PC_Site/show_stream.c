#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>

#define PORT 8080
#define IRIS_PACKET_PAYLOAD_SIZE 1400U
#define PACKET_QUEUE_CAPACITY 512U
#define ENCODED_QUEUE_CAPACITY 64U
#define DECODED_QUEUE_CAPACITY 32U
#define MAX_PACKETS_PER_FRAME 2048U

/* Must match the packet layout sent by the embedded UDP server. */
typedef struct __attribute__((packed)) {
	uint32_t frame_nmbr;
	uint32_t packet_idx;
	uint32_t packet_nmbr;
	uint8_t payload[IRIS_PACKET_PAYLOAD_SIZE];
} iris_packet_t;

/* Tracks the currently reconstructed frame from multiple UDP packets. */
typedef struct {
	uint32_t frame_nmbr;
	uint32_t packet_nmbr;
	uint32_t received_count;
	uint8_t *data;
	uint8_t *received;
} frame_assembly_t;

typedef struct {
	iris_packet_t packet;
} packet_msg_t;

typedef struct {
	uint32_t frame_nmbr;
	size_t size;
	uint8_t *data;
} encoded_frame_msg_t;

typedef struct {
	uint32_t frame_nmbr;
	int width;
	int height;
	int y_stride;
	int u_stride;
	int v_stride;
	uint8_t *y;
	uint8_t *u;
	uint8_t *v;
} decoded_frame_msg_t;

typedef struct {
	void **items;
	size_t capacity;
	size_t head;
	size_t tail;
	size_t count;
	bool closed;
	pthread_mutex_t mutex;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
} ptr_queue_t;

typedef struct {
	int sock_fd;
	atomic_bool stop_requested;
	atomic_int fatal_error;
	ptr_queue_t packet_queue;
	ptr_queue_t encoded_queue;
	ptr_queue_t decoded_queue;
} app_ctx_t;

static SDL_Window   *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture = NULL;
static int tex_w = 0;
static int tex_h = 0;

static void frame_assembly_reset(frame_assembly_t *assembly);
static int frame_assembly_init(frame_assembly_t *assembly,
							   uint32_t frame_nmbr,
							   uint32_t packet_nmbr);

static int ptr_queue_init(ptr_queue_t *queue, size_t capacity)
{
	memset(queue, 0, sizeof(*queue));
	queue->items = calloc(capacity, sizeof(void *));
	if (!queue->items) {
		return -1;
	}
	queue->capacity = capacity;
	if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
		free(queue->items);
		return -1;
	}
	if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
		pthread_mutex_destroy(&queue->mutex);
		free(queue->items);
		return -1;
	}
	if (pthread_cond_init(&queue->not_full, NULL) != 0) {
		pthread_cond_destroy(&queue->not_empty);
		pthread_mutex_destroy(&queue->mutex);
		free(queue->items);
		return -1;
	}

	return 0;
}

static void ptr_queue_close(ptr_queue_t *queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->closed = true;
	pthread_cond_broadcast(&queue->not_empty);
	pthread_cond_broadcast(&queue->not_full);
	pthread_mutex_unlock(&queue->mutex);
}

static int ptr_queue_push(ptr_queue_t *queue, void *item)
{
	pthread_mutex_lock(&queue->mutex);
	while (!queue->closed && queue->count == queue->capacity) {
		pthread_cond_wait(&queue->not_full, &queue->mutex);
	}
	if (queue->closed) {
		pthread_mutex_unlock(&queue->mutex);
		return -1;
	}

	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1U) % queue->capacity;
	queue->count++;
	pthread_cond_signal(&queue->not_empty);
	pthread_mutex_unlock(&queue->mutex);

	return 0;
}

static void *ptr_queue_pop(ptr_queue_t *queue)
{
	void *item;

	pthread_mutex_lock(&queue->mutex);
	while (!queue->closed && queue->count == 0U) {
		pthread_cond_wait(&queue->not_empty, &queue->mutex);
	}
	if (queue->count == 0U) {
		pthread_mutex_unlock(&queue->mutex);
		return NULL;
	}

	item = queue->items[queue->head];
	queue->head = (queue->head + 1U) % queue->capacity;
	queue->count--;
	pthread_cond_signal(&queue->not_full);
	pthread_mutex_unlock(&queue->mutex);

	return item;
}

static void ptr_queue_drain(ptr_queue_t *queue, void (*free_fn)(void *))
{
	pthread_mutex_lock(&queue->mutex);
	while (queue->count > 0U) {
		void *item = queue->items[queue->head];
		queue->head = (queue->head + 1U) % queue->capacity;
		queue->count--;
		if (free_fn) {
			free_fn(item);
		}
	}
	pthread_mutex_unlock(&queue->mutex);
}

static void ptr_queue_destroy(ptr_queue_t *queue)
{
	pthread_cond_destroy(&queue->not_full);
	pthread_cond_destroy(&queue->not_empty);
	pthread_mutex_destroy(&queue->mutex);
	free(queue->items);
	memset(queue, 0, sizeof(*queue));
}

static void free_packet_msg(void *item)
{
	free(item);
}

static void free_encoded_frame_msg(void *item)
{
	encoded_frame_msg_t *msg = item;
	if (!msg) {
		return;
	}
	free(msg->data);
	free(msg);
}

static void free_decoded_frame_msg(void *item)
{
	decoded_frame_msg_t *msg = item;
	if (!msg) {
		return;
	}
	free(msg->y);
	free(msg->u);
	free(msg->v);
	free(msg);
}

static void app_request_stop(app_ctx_t *ctx)
{
	bool expected = false;
	if (atomic_compare_exchange_strong(&ctx->stop_requested, &expected, true)) {
		ptr_queue_close(&ctx->packet_queue);
		ptr_queue_close(&ctx->encoded_queue);
		ptr_queue_close(&ctx->decoded_queue);
		if (ctx->sock_fd >= 0) {
			shutdown(ctx->sock_fd, SHUT_RDWR);
		}
	}
}

/* Create SDL window/renderer/texture the first time we can display a frame. */
static int init_sdl(int width, int height)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return -1;
	}

	window = SDL_CreateWindow("UDP H264 Stream",
							  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
							  width, height,
							  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return -1;
	}

	renderer = SDL_CreateRenderer(window, -1,
								  SDL_RENDERER_ACCELERATED |
								  SDL_RENDERER_PRESENTVSYNC);
	if (!renderer) {
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return -1;
	}

	texture = SDL_CreateTexture(renderer,
								SDL_PIXELFORMAT_IYUV,
								SDL_TEXTUREACCESS_STREAMING,
								width, height);
	if (!texture) {
		fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
		return -1;
	}

	tex_w = width;
	tex_h = height;
	return 0;
}

/* Recreate the texture when decoded frame resolution changes. */
static int resize_texture(int width, int height)
{
	SDL_DestroyTexture(texture);
	SDL_SetWindowSize(window, width, height);
	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	texture = SDL_CreateTexture(renderer,
								SDL_PIXELFORMAT_IYUV,
								SDL_TEXTUREACCESS_STREAMING,
								width, height);
	if (!texture) {
		fprintf(stderr, "SDL_CreateTexture (resize) failed: %s\n", SDL_GetError());
		return -1;
	}

	tex_w = width;
	tex_h = height;
	return 0;
}

/* Release SDL resources in reverse order of creation. */
static void cleanup_sdl(void)
{
	if (texture) {
		SDL_DestroyTexture(texture);
	}
	if (renderer) {
		SDL_DestroyRenderer(renderer);
	}
	if (window) {
		SDL_DestroyWindow(window);
	}
	SDL_Quit();
}

/* Poll events so ESC or window close can terminate the program gracefully. */
static int poll_events(void)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			return 1;
		}
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
			return 1;
		}
	}
	return 0;
}

/*
 * Display one decoded frame:
 * - Convert to YUV420P if the decoder output format differs.
 * - Lazily initialize SDL display resources.
 * - Upload YUV planes and present the frame.
 */
static int display_frame(const decoded_frame_msg_t *msg)
{
	if (!window) {
		/* First frame decides initial window/texture dimensions. */
		if (init_sdl(msg->width, msg->height) < 0) {
			return -1;
		}
	} else if (tex_w != msg->width || tex_h != msg->height) {
		/* Stream resolution changed; recreate texture/window to match. */
		if (resize_texture(msg->width, msg->height) < 0) {
			return -1;
		}
	}

	/* Upload Y, U, V planes and render fullscreen in current window. */
	SDL_UpdateYUVTexture(texture, NULL,
						 msg->y, msg->y_stride,
						 msg->u, msg->u_stride,
						 msg->v, msg->v_stride);

	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);

	return 0;
}

static void copy_plane(uint8_t *dst, int dst_stride,
				   const uint8_t *src, int src_stride,
				   int width, int height)
{
	for (int row = 0; row < height; row++) {
		memcpy(dst + (size_t)row * (size_t)dst_stride,
			   src + (size_t)row * (size_t)src_stride,
			   (size_t)width);
	}
}

static decoded_frame_msg_t *alloc_decoded_frame_msg(const AVFrame *src, uint32_t frame_nmbr)
{
	decoded_frame_msg_t *msg = calloc(1, sizeof(*msg));
	if (!msg) {
		return NULL;
	}

	msg->frame_nmbr = frame_nmbr;
	msg->width = src->width;
	msg->height = src->height;
	msg->y_stride = src->width;
	msg->u_stride = src->width / 2;
	msg->v_stride = src->width / 2;

	size_t y_size = (size_t)msg->y_stride * (size_t)msg->height;
	size_t u_size = (size_t)msg->u_stride * (size_t)(msg->height / 2);
	size_t v_size = (size_t)msg->v_stride * (size_t)(msg->height / 2);

	msg->y = malloc(y_size);
	msg->u = malloc(u_size);
	msg->v = malloc(v_size);
	if (!msg->y || !msg->u || !msg->v) {
		free_decoded_frame_msg(msg);
		return NULL;
	}

	copy_plane(msg->y, msg->y_stride,
		   src->data[0], src->linesize[0],
		   src->width, src->height);
	copy_plane(msg->u, msg->u_stride,
		   src->data[1], src->linesize[1],
		   src->width / 2, src->height / 2);
	copy_plane(msg->v, msg->v_stride,
		   src->data[2], src->linesize[2],
		   src->width / 2, src->height / 2);

	return msg;
}

/* Send one compressed packet into FFmpeg and enqueue all produced frames. */
static int decode_and_enqueue(app_ctx_t *app,
				      AVCodecContext *dec_ctx,
				      AVFrame *frame,
				      AVPacket *pkt,
				      struct SwsContext **sws_ctx,
				      AVFrame *yuv_frame)
{
	/* pkt can be NULL during flush, which FFmpeg treats as end-of-stream signal. */
	int ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		fprintf(stderr, "Error sending packet to decoder: %d\n", ret);
		return -1;
	}

	while (1) {
		/* Drain all frames currently available for this one packet input. */
		ret = avcodec_receive_frame(dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			/* EAGAIN: decoder needs more input; EOF: fully flushed. */
			return 0;
		}
		if (ret < 0) {
			fprintf(stderr, "Error receiving frame from decoder: %d\n", ret);
			return -1;
		}

		AVFrame *src = frame;
		if (frame->format != AV_PIX_FMT_YUV420P) {
			*sws_ctx = sws_getCachedContext(*sws_ctx,
										frame->width, frame->height,
										(enum AVPixelFormat)frame->format,
										frame->width, frame->height,
										AV_PIX_FMT_YUV420P,
										SWS_BILINEAR, NULL, NULL, NULL);
			if (!*sws_ctx) {
				fprintf(stderr, "sws_getCachedContext failed\n");
				return -1;
			}

			if (yuv_frame->width != frame->width || yuv_frame->height != frame->height) {
				av_frame_unref(yuv_frame);
				yuv_frame->format = AV_PIX_FMT_YUV420P;
				yuv_frame->width = frame->width;
				yuv_frame->height = frame->height;
				if (av_frame_get_buffer(yuv_frame, 0) < 0) {
					fprintf(stderr, "Cannot allocate conversion frame buffer\n");
					return -1;
				}
			}

			sws_scale(*sws_ctx,
				  (const uint8_t *const *)frame->data, frame->linesize,
				  0, frame->height,
				  yuv_frame->data, yuv_frame->linesize);
			src = yuv_frame;
		}

		decoded_frame_msg_t *out = alloc_decoded_frame_msg(src, (uint32_t)dec_ctx->frame_num);
		if (!out) {
			fprintf(stderr, "Failed to allocate decoded frame message\n");
			return -1;
		}

		if (ptr_queue_push(&app->decoded_queue, out) != 0) {
			free_decoded_frame_msg(out);
			return 1;
		}

		printf("decoded frame %3" PRId64 " (%dx%d)\n",
			   dec_ctx->frame_num, frame->width, frame->height);
	}
}

/*
 * Feed contiguous H.264 bytes into the parser, which may emit one or more
 * complete decoder packets. Each emitted packet is decoded and displayed.
 */
static int process_stream_bytes(AVCodecParserContext *parser,
								app_ctx_t *app,
								AVCodecContext *codec_ctx,
								AVPacket *pkt,
								AVFrame *frame,
								struct SwsContext **sws_ctx,
								AVFrame *yuv_frame,
								const uint8_t *data,
								size_t size)
{
	while (size > 0) {
		/*
		 * Parser may consume only part of input and may or may not output a packet.
		 * We keep looping until this assembled frame buffer is fully consumed.
		 */
		int ret = av_parser_parse2(parser, codec_ctx,
								   &pkt->data, &pkt->size,
								   data, (int)size,
								   AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		if (ret < 0) {
			fprintf(stderr, "Error while parsing bitstream\n");
			return -1;
		}

		data += ret;
		size -= (size_t)ret;

		if (pkt->size > 0) {
			/* A complete bitstream packet is ready: decode and push to display queue. */
			int res = decode_and_enqueue(app, codec_ctx, frame, pkt, sws_ctx, yuv_frame);
			if (res != 0) {
				return res;
			}
		}
	}

	return 0;
}

static void *receiver_thread_main(void *arg)
{
	app_ctx_t *app = arg;

	while (!atomic_load(&app->stop_requested)) {
		iris_packet_t pkt;
		ssize_t bytes = recv(app->sock_fd, &pkt, sizeof(pkt), 0);
		if (bytes < 0) {
			if (atomic_load(&app->stop_requested)) {
				break;
			}
			if (errno == EINTR) {
				continue;
			}
			perror("recv");
			atomic_store(&app->fatal_error, 1);
			app_request_stop(app);
			break;
		}

		if ((size_t)bytes != sizeof(pkt)) {
			continue;
		}

		packet_msg_t *msg = malloc(sizeof(*msg));
		if (!msg) {
			fprintf(stderr, "Failed to allocate packet message\n");
			atomic_store(&app->fatal_error, 1);
			app_request_stop(app);
			break;
		}
		msg->packet = pkt;

		if (ptr_queue_push(&app->packet_queue, msg) != 0) {
			free(msg);
			break;
		}
	}

	ptr_queue_close(&app->packet_queue);
	return NULL;
}

static void *assembler_thread_main(void *arg)
{
	app_ctx_t *app = arg;
	frame_assembly_t assembly = {0};

	while (!atomic_load(&app->stop_requested)) {
		packet_msg_t *msg = ptr_queue_pop(&app->packet_queue);
		if (!msg) {
			break;
		}

		const iris_packet_t *pkt = &msg->packet;

		if (assembly.data == NULL || pkt->frame_nmbr != assembly.frame_nmbr) {
			if (frame_assembly_init(&assembly, pkt->frame_nmbr, pkt->packet_nmbr) != 0) {
				fprintf(stderr, "Failed to initialize frame assembly\n");
				free(msg);
				atomic_store(&app->fatal_error, 1);
				app_request_stop(app);
				break;
			}
		}

		if (pkt->packet_nmbr != assembly.packet_nmbr) {
			if (frame_assembly_init(&assembly, pkt->frame_nmbr, pkt->packet_nmbr) != 0) {
				fprintf(stderr, "Failed to reinitialize frame assembly\n");
				free(msg);
				atomic_store(&app->fatal_error, 1);
				app_request_stop(app);
				break;
			}
		}

		if (pkt->packet_idx < assembly.packet_nmbr && !assembly.received[pkt->packet_idx]) {
			memcpy(&assembly.data[(size_t)pkt->packet_idx * IRIS_PACKET_PAYLOAD_SIZE],
				   pkt->payload,
				   IRIS_PACKET_PAYLOAD_SIZE);
			assembly.received[pkt->packet_idx] = 1;
			assembly.received_count++;
		}

		if (assembly.received_count == assembly.packet_nmbr) {
			size_t assembled_size = (size_t)assembly.packet_nmbr * IRIS_PACKET_PAYLOAD_SIZE;
			encoded_frame_msg_t *frame_msg = calloc(1, sizeof(*frame_msg));
			if (!frame_msg) {
				fprintf(stderr, "Failed to allocate assembled frame message\n");
				free(msg);
				atomic_store(&app->fatal_error, 1);
				app_request_stop(app);
				break;
			}

			if (frame_msg->frame_nmbr + 1 != assembly.frame_nmbr) {
				printf("Warning: non-sequential frame numbers (got %u, expected %u)\n",
					   assembly.frame_nmbr, frame_msg->frame_nmbr + 1);
			}

			frame_msg->frame_nmbr = assembly.frame_nmbr;
			frame_msg->size = assembled_size;
			frame_msg->data = malloc(assembled_size);
			if (!frame_msg->data) {
				free(frame_msg);
				fprintf(stderr, "Failed to allocate assembled frame bytes\n");
				free(msg);
				atomic_store(&app->fatal_error, 1);
				app_request_stop(app);
				break;
			}

			memcpy(frame_msg->data, assembly.data, assembled_size);
			printf("frame %u assembled (%u packets, %zu bytes)\n",
				   assembly.frame_nmbr,
				   assembly.packet_nmbr,
				   assembled_size);

			if (ptr_queue_push(&app->encoded_queue, frame_msg) != 0) {
				free_encoded_frame_msg(frame_msg);
				free(msg);
				break;
			}

			frame_assembly_reset(&assembly);
		}

		free(msg);
	}

	frame_assembly_reset(&assembly);
	ptr_queue_close(&app->encoded_queue);
	return NULL;
}

static void *decoder_thread_main(void *arg)
{
	app_ctx_t *app = arg;
	AVPacket *pkt = NULL;
	AVCodecParserContext *parser = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVFrame *frame = NULL;
	AVFrame *yuv_frame = NULL;
	struct SwsContext *sws_ctx = NULL;

	pkt = av_packet_alloc();
	if (!pkt) {
		fprintf(stderr, "Could not allocate AVPacket\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		fprintf(stderr, "H264 decoder not found\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	parser = av_parser_init(codec->id);
	if (!parser) {
		fprintf(stderr, "H264 parser not found\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		fprintf(stderr, "Could not allocate codec context\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	frame = av_frame_alloc();
	yuv_frame = av_frame_alloc();
	if (!frame || !yuv_frame) {
		fprintf(stderr, "Could not allocate AVFrame(s)\n");
		atomic_store(&app->fatal_error, 1);
		app_request_stop(app);
		goto out;
	}

	while (!atomic_load(&app->stop_requested)) {
		encoded_frame_msg_t *msg = ptr_queue_pop(&app->encoded_queue);
		if (!msg) {
			break;
		}

		int res = process_stream_bytes(parser, app, codec_ctx, pkt, frame,
							   &sws_ctx, yuv_frame,
							   msg->data, msg->size);
		free_encoded_frame_msg(msg);
		if (res == 1) {
			break;
		}
		if (res < 0) {
			atomic_store(&app->fatal_error, 1);
			app_request_stop(app);
			break;
		}
	}

	if (codec_ctx && pkt && !atomic_load(&app->stop_requested)) {
		int flush_res = decode_and_enqueue(app, codec_ctx, frame, NULL, &sws_ctx, yuv_frame);
		if (flush_res < 0) {
			atomic_store(&app->fatal_error, 1);
			app_request_stop(app);
		}
	}

out:
	if (parser) {
		av_parser_close(parser);
	}
	if (codec_ctx) {
		avcodec_free_context(&codec_ctx);
	}
	if (frame) {
		av_frame_free(&frame);
	}
	if (yuv_frame) {
		av_frame_free(&yuv_frame);
	}
	if (pkt) {
		av_packet_free(&pkt);
	}
	if (sws_ctx) {
		sws_freeContext(sws_ctx);
	}

	ptr_queue_close(&app->decoded_queue);
	return NULL;
}

static void *display_thread_main(void *arg)
{
	app_ctx_t *app = arg;

	while (!atomic_load(&app->stop_requested)) {
		decoded_frame_msg_t *msg = ptr_queue_pop(&app->decoded_queue);
		if (!msg) {
			break;
		}

		if (display_frame(msg) < 0) {
			fprintf(stderr, "Display failed\n");
			free_decoded_frame_msg(msg);
			atomic_store(&app->fatal_error, 1);
			app_request_stop(app);
			break;
		}

		if (poll_events() != 0) {
			free_decoded_frame_msg(msg);
			app_request_stop(app);
			break;
		}

		free_decoded_frame_msg(msg);
	}

	cleanup_sdl();
	return NULL;
}

/* Drop and free current in-progress frame assembly buffers. */
static void frame_assembly_reset(frame_assembly_t *assembly)
{
	free(assembly->data);
	free(assembly->received);
	memset(assembly, 0, sizeof(*assembly));
}

/* Allocate buffers for a new frame being reconstructed from UDP packets. */
static int frame_assembly_init(frame_assembly_t *assembly,
							   uint32_t frame_nmbr,
							   uint32_t packet_nmbr)
{
	/* Always reset first to avoid leaks if we are restarting assembly. */
	frame_assembly_reset(assembly);

	if (packet_nmbr == 0U) {
		fprintf(stderr,
				"Invalid packet count for frame %u: packet_nmbr=0\n",
				frame_nmbr);
		return -1;
	}

	if (packet_nmbr > MAX_PACKETS_PER_FRAME) {
		fprintf(stderr,
				"Invalid packet count for frame %u: packet_nmbr=%u (max %u)\n",
				frame_nmbr, packet_nmbr, MAX_PACKETS_PER_FRAME);
		return -1;
	}

	if ((size_t)packet_nmbr > (SIZE_MAX / IRIS_PACKET_PAYLOAD_SIZE)) {
		fprintf(stderr,
				"packet_nmbr overflow for frame %u: packet_nmbr=%u payload=%u\n",
				frame_nmbr, packet_nmbr, IRIS_PACKET_PAYLOAD_SIZE);
		return -1;
	}

	assembly->frame_nmbr = frame_nmbr;
	assembly->packet_nmbr = packet_nmbr;
	/* Allocate contiguous storage for packet_nmbr chunks of 1400 bytes each. */
	assembly->data = calloc((size_t)packet_nmbr, IRIS_PACKET_PAYLOAD_SIZE);
	/* received[i] marks whether chunk i is already copied (dedupe support). */
	assembly->received = calloc((size_t)packet_nmbr, sizeof(*assembly->received));
	if (!assembly->data || !assembly->received) {
		fprintf(stderr,
				"Allocation failed for frame %u: packet_nmbr=%u (%zu bytes data + %zu bytes bitmap)\n",
				frame_nmbr,
				packet_nmbr,
				(size_t)packet_nmbr * IRIS_PACKET_PAYLOAD_SIZE,
				(size_t)packet_nmbr * sizeof(*assembly->received));
		frame_assembly_reset(assembly);
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <server-ip> [port]\n", argv[0]);
		return 1;
	}

	const char *server_ip = argv[1];
	int port = PORT;
	if (argc >= 3) {
		/* Optional UDP port override from command line. */
		port = atoi(argv[2]);
		if (port <= 0 || port > 65535) {
			fprintf(stderr, "Invalid port: %s\n", argv[2]);
			return 1;
		}
	}

	int rc = 1;
	int sock_fd = -1;
	struct sockaddr_in server_addr;
	app_ctx_t app;
	pthread_t receiver_thread;
	pthread_t assembler_thread;
	pthread_t decoder_thread;
	pthread_t display_thread;
	bool receiver_started = false;
	bool assembler_started = false;
	bool decoder_started = false;
	bool display_started = false;
	bool packet_queue_ready = false;
	bool encoded_queue_ready = false;
	bool decoded_queue_ready = false;

	memset(&app, 0, sizeof(app));
	app.sock_fd = -1;
	atomic_init(&app.stop_requested, false);
	atomic_init(&app.fatal_error, 0);

	if (ptr_queue_init(&app.packet_queue, PACKET_QUEUE_CAPACITY) != 0) {
		fprintf(stderr, "Failed to initialize packet queue\n");
		goto out;
	}
	packet_queue_ready = true;

	if (ptr_queue_init(&app.encoded_queue, ENCODED_QUEUE_CAPACITY) != 0) {
		fprintf(stderr, "Failed to initialize encoded queue\n");
		goto out;
	}
	encoded_queue_ready = true;

	if (ptr_queue_init(&app.decoded_queue, DECODED_QUEUE_CAPACITY) != 0) {
		fprintf(stderr, "Failed to initialize worker queues\n");
		goto out;
	}
	decoded_queue_ready = true;

	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		perror("socket");
		goto out;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
		perror("inet_pton");
		goto out;
	}

	if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("connect");
		goto out;
	}
	app.sock_fd = sock_fd;

	/* Tell the server we are ready so it starts transmitting. */
	const char *start_msg = "START";
	if (send(sock_fd, start_msg, strlen(start_msg), 0) < 0) {
		perror("send START");
		goto out;
	}

	printf("Listening for stream from %s:%d...\n", server_ip, port);

	if (pthread_create(&receiver_thread, NULL, receiver_thread_main, &app) != 0) {
		fprintf(stderr, "Failed to start receiver thread\n");
		goto out;
	}
	receiver_started = true;

	if (pthread_create(&assembler_thread, NULL, assembler_thread_main, &app) != 0) {
		fprintf(stderr, "Failed to start assembler thread\n");
		goto out;
	}
	assembler_started = true;

	if (pthread_create(&decoder_thread, NULL, decoder_thread_main, &app) != 0) {
		fprintf(stderr, "Failed to start decoder thread\n");
		goto out;
	}
	decoder_started = true;

	if (pthread_create(&display_thread, NULL, display_thread_main, &app) != 0) {
		fprintf(stderr, "Failed to start display thread\n");
		goto out;
	}
	display_started = true;

	if (display_started) {
		pthread_join(display_thread, NULL);
		display_started = false;
	}
	app_request_stop(&app);
	if (receiver_started) {
		pthread_join(receiver_thread, NULL);
		receiver_started = false;
	}
	if (assembler_started) {
		pthread_join(assembler_thread, NULL);
		assembler_started = false;
	}
	if (decoder_started) {
		pthread_join(decoder_thread, NULL);
		decoder_started = false;
	}

	rc = atomic_load(&app.fatal_error) ? 1 : 0;

out:
	if (packet_queue_ready && encoded_queue_ready && decoded_queue_ready) {
		app_request_stop(&app);
	}
	if (display_started) {
		pthread_join(display_thread, NULL);
	}
	if (receiver_started) {
		pthread_join(receiver_thread, NULL);
	}
	if (assembler_started) {
		pthread_join(assembler_thread, NULL);
	}
	if (decoder_started) {
		pthread_join(decoder_thread, NULL);
	}

	if (packet_queue_ready) {
		ptr_queue_drain(&app.packet_queue, free_packet_msg);
		ptr_queue_destroy(&app.packet_queue);
	}
	if (encoded_queue_ready) {
		ptr_queue_drain(&app.encoded_queue, free_encoded_frame_msg);
		ptr_queue_destroy(&app.encoded_queue);
	}
	if (decoded_queue_ready) {
		ptr_queue_drain(&app.decoded_queue, free_decoded_frame_msg);
		ptr_queue_destroy(&app.decoded_queue);
	}

	if (sock_fd >= 0) {
		close(sock_fd);
	}

	return rc;
}
