/**
 * visialize_h264.c
 *
 * Decodes an H.264 raw bitstream using FFmpeg (libavcodec) and displays each
 * frame in a live SDL2 window.
 *
 * Decoding approach taken from h264_decode.c (FFmpeg avcodec API).
 * Visualization approach taken from edge264_test.c (SDL2 YUV texture).
 *
 * Usage:  visialize_h264 <input.h264>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#define INBUF_SIZE 16384

/* -------------------------------------------------------------------------
 * SDL2 state
 * ---------------------------------------------------------------------- */
static SDL_Window   *window   = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture  = NULL;
static int           tex_w    = 0;
static int           tex_h    = 0;

/* -------------------------------------------------------------------------
 * init_sdl  – create window, renderer and YUV texture
 * ---------------------------------------------------------------------- */
static int init_sdl(int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow("H264 Visualizer",
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

/* -------------------------------------------------------------------------
 * resize_texture  – recreate texture when the frame dimensions change
 * ---------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * cleanup_sdl  – release all SDL2 resources
 * ---------------------------------------------------------------------- */
static void cleanup_sdl(void)
{
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();
}

/* -------------------------------------------------------------------------
 * poll_events  – process pending SDL2 events
 * Returns 1 if the user requested to quit, 0 otherwise.
 * ---------------------------------------------------------------------- */
static int poll_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return 1;
        if (event.type == SDL_KEYDOWN &&
            event.key.keysym.sym == SDLK_ESCAPE)
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * display_frame  – upload one decoded AVFrame to SDL2 and present it.
 *
 * Handles any pixel format by converting to YUV420P via libswscale when
 * needed.  Returns 1 if the user asked to quit, 0 on success, -1 on error.
 * ---------------------------------------------------------------------- */
static int display_frame(AVFrame *frame, struct SwsContext **sws_ctx,
                         AVFrame *yuv_frame)
{
    /* ----- pixel-format conversion if the decoder does not give YUV420P -- */
    AVFrame *src = frame;

    if (frame->format != AV_PIX_FMT_YUV420P) {
        /* (Re-)create sws context when dimensions or format change */
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

        /* Allocate conversion buffer on first use / when size changes */
        if (yuv_frame->width != frame->width ||
            yuv_frame->height != frame->height) {
            av_frame_unref(yuv_frame);
            yuv_frame->format = AV_PIX_FMT_YUV420P;
            yuv_frame->width  = frame->width;
            yuv_frame->height = frame->height;
            if (av_frame_get_buffer(yuv_frame, 0) < 0) {
                fprintf(stderr, "Cannot allocate conversion frame buffer\n");
                return -1;
            }
        }

        sws_scale(*sws_ctx,
                  (const uint8_t * const *)frame->data, frame->linesize,
                  0, frame->height,
                  yuv_frame->data, yuv_frame->linesize);
        src = yuv_frame;
    }

    /* ----- create/resize SDL window and texture as needed ---------------- */
    if (!window) {
        if (init_sdl(src->width, src->height) < 0)
            return -1;
    } else if (tex_w != src->width || tex_h != src->height) {
        if (resize_texture(src->width, src->height) < 0)
            return -1;
    }

    /* ----- upload YUV planes and render ---------------------------------- */
    SDL_UpdateYUVTexture(texture, NULL,
                         src->data[0], src->linesize[0],
                         src->data[1], src->linesize[1],
                         src->data[2], src->linesize[2]);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    return poll_events();
}

/* -------------------------------------------------------------------------
 * decode_and_display  – send one packet to the decoder and display all
 * resulting frames.
 *
 * Pass pkt == NULL to flush the decoder at end-of-stream.
 * Returns 1 if the user quit, 0 on success, -1 on error.
 * ---------------------------------------------------------------------- */
static int decode_and_display(AVCodecContext *dec_ctx,
                               AVFrame *frame, AVPacket *pkt,
                               struct SwsContext **sws_ctx,
                               AVFrame *yuv_frame)
{
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        /* EAGAIN should not happen here, treat everything else as fatal */
        fprintf(stderr, "Error sending packet to decoder: %d\n", ret);
        return -1;
    }

    while (1) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "Error receiving frame from decoder: %d\n", ret);
            return -1;
        }

        printf("decoded frame %3" PRId64 "  (%dx%d)\n",
               dec_ctx->frame_num, frame->width, frame->height);
        fflush(stdout);

        int res = display_frame(frame, sws_ctx, yuv_frame);
        if (res != 0)
            return res; /* 1 = quit, -1 = error */
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.h264>\n", argv[0]);
        return 1;
    }
    const char *filename = argv[1];

    /* ----- allocate packet ------------------------------------------------ */
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return 1;
    }

    /* ----- find H.264 decoder --------------------------------------------- */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H264 codec not found\n");
        return 1;
    }

    /* ----- create bitstream parser ---------------------------------------- */
    AVCodecParserContext *parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "H264 parser not found\n");
        return 1;
    }

    /* ----- allocate and open codec context --------------------------------- */
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate codec context\n");
        return 1;
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 1;
    }

    /* ----- open input file ------------------------------------------------- */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        return 1;
    }

    /* ----- allocate frame buffers ------------------------------------------ */
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate AVFrame\n");
        return 1;
    }

    /* Optional frame used for pixel-format conversion */
    AVFrame *yuv_frame = av_frame_alloc();
    if (!yuv_frame) {
        fprintf(stderr, "Could not allocate yuv_frame\n");
        return 1;
    }

    /* ----- main decode / display loop -------------------------------------- */
    struct SwsContext *sws_ctx = NULL;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    int quit = 0;
    int eof;

    do {
        size_t data_size = fread(inbuf, 1, INBUF_SIZE, f);
        if (ferror(f)) {
            fprintf(stderr, "Error reading file\n");
            break;
        }
        eof = (data_size == 0);

        uint8_t *data = inbuf;
        while ((data_size > 0 || eof) && !quit) {
            int ret = av_parser_parse2(parser, c,
                                       &pkt->data, &pkt->size,
                                       data, (int)data_size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "Error while parsing\n");
                quit = 1;
                break;
            }
            data      += ret;
            data_size -= (size_t)ret;
            printf("parser output: packet size = %d\n", pkt->size);

            if (pkt->size) {
                int res = decode_and_display(c, frame, pkt,
                                             &sws_ctx, yuv_frame);
                if (res != 0)
                    quit = 1;
            } else if (eof) {
                break;
            }
        }
    } while (!eof && !quit);

    /* ----- flush the decoder ----------------------------------------------- */
    if (!quit)
        decode_and_display(c, frame, NULL, &sws_ctx, yuv_frame);

    /* ----- keep window open until the user closes it ----------------------- */
    if (window && !quit) {
        printf("Decoding complete – press ESC or close the window to exit.\n");
        SDL_Event event;
        int done = 0;
        while (!done) {
            if (SDL_WaitEvent(&event)) {
                if (event.type == SDL_QUIT)
                    done = 1;
                if (event.type == SDL_KEYDOWN &&
                    event.key.keysym.sym == SDLK_ESCAPE)
                    done = 1;
            }
        }
    }

    /* ----- cleanup --------------------------------------------------------- */
    fclose(f);
    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_frame_free(&yuv_frame);
    av_packet_free(&pkt);
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    cleanup_sdl();

    return 0;
}
