#include <arpa/inet.h>
#include <inttypes.h>
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
#define IRIS_PACKET_PAYLOAD_SIZE 1024U

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

static SDL_Window   *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture = NULL;
static int tex_w = 0;
static int tex_h = 0;

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
static int display_frame(AVFrame *frame, struct SwsContext **sws_ctx, AVFrame *yuv_frame)
{
	/* By default we render decoder output directly unless conversion is needed. */
	AVFrame *src = frame;

	if (frame->format != AV_PIX_FMT_YUV420P) {
		/* Cache/reuse scaler context to avoid reallocating every frame. */
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
			/* Drop old buffer and allocate a new YUV buffer matching this frame size. */
			av_frame_unref(yuv_frame);
			yuv_frame->format = AV_PIX_FMT_YUV420P;
			yuv_frame->width = frame->width;
			yuv_frame->height = frame->height;
			if (av_frame_get_buffer(yuv_frame, 0) < 0) {
				fprintf(stderr, "Cannot allocate conversion frame buffer\n");
				return -1;
			}
		}

		/* Convert arbitrary decoder output format into IYUV planes for SDL. */
		sws_scale(*sws_ctx,
				  (const uint8_t *const *)frame->data, frame->linesize,
				  0, frame->height,
				  yuv_frame->data, yuv_frame->linesize);
		src = yuv_frame;
	}

	if (!window) {
		/* First frame decides initial window/texture dimensions. */
		if (init_sdl(src->width, src->height) < 0) {
			return -1;
		}
	} else if (tex_w != src->width || tex_h != src->height) {
		/* Stream resolution changed; recreate texture/window to match. */
		if (resize_texture(src->width, src->height) < 0) {
			return -1;
		}
	}

	/* Upload Y, U, V planes and render fullscreen in current window. */
	SDL_UpdateYUVTexture(texture, NULL,
						 src->data[0], src->linesize[0],
						 src->data[1], src->linesize[1],
						 src->data[2], src->linesize[2]);

	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);

	return poll_events();
}

/* Send one compressed packet into FFmpeg and render all produced frames. */
static int decode_and_display(AVCodecContext *dec_ctx,
							  AVFrame *frame, AVPacket *pkt,
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

		printf("decoded frame %3" PRId64 " (%dx%d)\n",
			   dec_ctx->frame_num, frame->width, frame->height);

		int res = display_frame(frame, sws_ctx, yuv_frame);
		if (res != 0) {
			return res;
		}
	}
}

/*
 * Feed contiguous H.264 bytes into the parser, which may emit one or more
 * complete decoder packets. Each emitted packet is decoded and displayed.
 */
static int process_stream_bytes(AVCodecParserContext *parser,
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
			/* A complete bitstream packet is ready: decode and present it. */
			int res = decode_and_display(codec_ctx, frame, pkt, sws_ctx, yuv_frame);
			if (res != 0) {
				return res;
			}
		}
	}

	return 0;
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
		return -1;
	}

	assembly->frame_nmbr = frame_nmbr;
	assembly->packet_nmbr = packet_nmbr;
	/* Allocate contiguous storage for packet_nmbr chunks of 1024 bytes each. */
	assembly->data = calloc((size_t)packet_nmbr, IRIS_PACKET_PAYLOAD_SIZE);
	/* received[i] marks whether chunk i is already copied (dedupe support). */
	assembly->received = calloc(packet_nmbr, sizeof(uint8_t));
	if (!assembly->data || !assembly->received) {
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
	frame_assembly_t assembly = {0};

	AVPacket *pkt = NULL;
	AVCodecParserContext *parser = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVFrame *frame = NULL;
	AVFrame *yuv_frame = NULL;
	struct SwsContext *sws_ctx = NULL;

	pkt = av_packet_alloc();
	if (!pkt) {
		fprintf(stderr, "Could not allocate AVPacket\n");
		goto out;
	}

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		fprintf(stderr, "H264 decoder not found\n");
		goto out;
	}

	parser = av_parser_init(codec->id);
	if (!parser) {
		fprintf(stderr, "H264 parser not found\n");
		goto out;
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		fprintf(stderr, "Could not allocate codec context\n");
		goto out;
	}

	if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		goto out;
	}

	frame = av_frame_alloc();
	yuv_frame = av_frame_alloc();
	if (!frame || !yuv_frame) {
		fprintf(stderr, "Could not allocate AVFrame(s)\n");
		goto out;
	}

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

	/* Tell the server we are ready so it starts transmitting. */
	const char *start_msg = "START";
	if (send(sock_fd, start_msg, strlen(start_msg), 0) < 0) {
		perror("send START");
		goto out;
	}

	printf("Listening for stream from %s:%d...\n", server_ip, port);

	while (1) {
		iris_packet_t pkt_in;

		/* Receive one UDP packet containing header + 1024-byte payload chunk. */
		ssize_t bytes = recv(sock_fd, &pkt_in, sizeof(pkt_in), 0);
		if (bytes < 0) {
			perror("recv");
			goto out;
		}

		/* Ignore malformed datagrams that do not match the expected packet size. */
		if ((size_t)bytes != sizeof(pkt_in)) {
			continue;
		}

		/* Start a new assembly whenever frame number changes (or on first packet). */
		if (assembly.data == NULL || pkt_in.frame_nmbr != assembly.frame_nmbr) {
			/* Any incomplete previous frame is discarded here. */
			if (frame_assembly_init(&assembly, pkt_in.frame_nmbr, pkt_in.packet_nmbr) != 0) {
				fprintf(stderr, "Failed to initialize frame assembly\n");
				goto out;
			}
		}

		/* If metadata changed unexpectedly, reinitialize for consistency. */
		if (pkt_in.packet_nmbr != assembly.packet_nmbr) {
			/* Protect against sender-side metadata changes mid-frame. */
			if (frame_assembly_init(&assembly, pkt_in.frame_nmbr, pkt_in.packet_nmbr) != 0) {
				fprintf(stderr, "Failed to reinitialize frame assembly\n");
				goto out;
			}
		}

		/* Ignore out-of-range packet indexes. */
		if (pkt_in.packet_idx >= assembly.packet_nmbr) {
			continue;
		}

		/* Copy each packet only once; duplicates can occur with UDP. */
		if (!assembly.received[pkt_in.packet_idx]) {
			/* Place packet payload at its exact frame offset. */
			memcpy(&assembly.data[(size_t)pkt_in.packet_idx * IRIS_PACKET_PAYLOAD_SIZE],
				   pkt_in.payload,
				   IRIS_PACKET_PAYLOAD_SIZE);
			assembly.received[pkt_in.packet_idx] = 1;
			assembly.received_count++;
		}

		/* When all chunks are collected, parse/decode/display the full frame bytes. */
		if (assembly.received_count == assembly.packet_nmbr) {
			/* Current protocol sends fixed payload chunks, so assembled size is exact. */
			size_t assembled_size = (size_t)assembly.packet_nmbr * IRIS_PACKET_PAYLOAD_SIZE;
			int res = process_stream_bytes(parser, codec_ctx, pkt, frame,
										   &sws_ctx, yuv_frame,
										   assembly.data, assembled_size);
			if (res == 1) {
				rc = 0;
				goto out;
			}
			if (res < 0) {
				goto out;
			}

			printf("frame %u assembled (%u packets, %zu bytes)\n",
				   assembly.frame_nmbr,
				   assembly.packet_nmbr,
				   assembled_size);

			/* Ready for next frame number arriving over UDP. */
			frame_assembly_reset(&assembly);
		}
	}

out:
	/* Flush delayed frames in decoder before cleanup. */
	if (codec_ctx && pkt) {
		decode_and_display(codec_ctx, frame, NULL, &sws_ctx, yuv_frame);
	}
	frame_assembly_reset(&assembly);
	if (sock_fd >= 0) {
		close(sock_fd);
	}
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
	cleanup_sdl();

	return rc;
}
