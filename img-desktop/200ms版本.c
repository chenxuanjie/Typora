#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include "new.h"

int main()
{
    // 初始化网络和设备
    avformat_network_init();
    avdevice_register_all();

    // 创建全局结构体
    struct push push;
    int ret;
    push.ifmt_ctx = NULL;
    push.ifmt_ctx = avformat_alloc_context();
    push.iformat = av_find_input_format("v4l2");

    if ((ret = avformat_open_input(&push.ifmt_ctx, "/dev/video0", push.iformat, NULL)) < 0)
    {
        printf("Could not open input file.\n");
        return -1;
    }
    if ((ret = avformat_find_stream_info(push.ifmt_ctx, 0)) < 0)
    {
        printf("Failed to retrieve input stream information\n");
        return -1;
    }

    // 找到视频流
    push.videoindex = -1;
    for (int i = 0; i < push.ifmt_ctx->nb_streams; i++)
    {
        if (push.ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            push.videoindex = i;
            break;
        }
    }
    if (push.videoindex == -1)
    {
        printf("Didn't find a video stream.\n");
        return -1;
    }
    printf("---------------- File Information ---------------\n");
    av_dump_format(push.ifmt_ctx, 0, "/dev/video4", 0);
    printf("-------------------------------------------------\n");

    // 找到解码器并打开
    push.pCodecCtx = push.ifmt_ctx->streams[push.videoindex]->codecpar;
    push.pCodec = avcodec_find_decoder(push.pCodecCtx->codec_id);
    push.inputcodec = avcodec_alloc_context3(push.pCodec);
    push.in_stream = push.ifmt_ctx->streams[push.videoindex];

    avcodec_parameters_to_context(push.inputcodec, push.ifmt_ctx->streams[push.videoindex]->codecpar);
    if (push.pCodec == NULL)
    {
        printf("Codec not found.\n");
        return -1;
    }
    if ((ret = avcodec_open2(push.inputcodec, push.pCodec, NULL)) < 0)
    {
        printf("Could not open codec.\n");
        return -1;
    }

    // 编码器：这里使用H.264编码器
    push.pCodec1 = NULL;
    push.pCodec1 = avcodec_find_encoder(AV_CODEC_ID_H264);
    push.outputcodec = avcodec_alloc_context3(push.pCodec1);

    // 配置编码器参数，修改输出分辨率为720p（1280×720）
    push.outputcodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    push.outputcodec->codec_id = push.pCodec1->id;
    push.outputcodec->thread_count = 8;
    push.outputcodec->bit_rate = 1280 * 720 * 3; // 根据分辨率调整码率
    push.outputcodec->width = 1280;
    push.outputcodec->height = 720;
    push.outputcodec->time_base = (AVRational){1, 25};
    push.outputcodec->framerate = (AVRational){25, 1};
    push.outputcodec->gop_size = 15;
    push.outputcodec->max_b_frames = 0;
    push.outputcodec->pix_fmt = AV_PIX_FMT_YUV420P;

    // 设置编码质量和速度
    av_opt_set(push.outputcodec->priv_data, "preset", "ultrafast", 0);
    av_opt_set(push.outputcodec->priv_data, "tune", "zerolatency", 0);
    push.params = NULL;
    av_dict_set(&push.params, "profile", "baseline", 0);

    // 打开编码器
    ret = avcodec_open2(push.outputcodec, push.pCodec1, &push.params);
    if (ret < 0)
    {
        fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
        return -1;
    }

    // 初始化输出封装器
    push.out_filename = "rtsp://120.77.0.8/live/livestream";
    push.ofmt_ctx = NULL;
    avformat_alloc_output_context2(&push.ofmt_ctx, NULL, "rtsp", push.out_filename);
    if (!push.ofmt_ctx)
    {
        printf("Could not create output context\n");
        return -1;
    }
    // 在输出上下文中创建一个流
    push.out_stream = avformat_new_stream(push.ofmt_ctx, push.pCodec1);
    push.out_stream->codecpar->codec_tag = 0;
    // 复制编码器参数到流中
    avcodec_parameters_from_context(push.out_stream->codecpar, push.outputcodec);

    // 写入输出文件头部信息
    ret = avformat_write_header(push.ofmt_ctx, NULL);
    if (ret < 0)
    {
        printf("Error occurred when opening output URL\n");
        return -1;
    }
    printf("---------------- File Information ---------------\n");
    av_dump_format(push.ofmt_ctx, 0, "/dev/video4", 1);
    printf("-------------------------------------------------\n");

    push.packet = av_packet_alloc();
    push.outPkt = av_packet_alloc();
    // 初始化像素格式转换上下文，将输入转换为YUV420P
    struct SwsContext *img_convert_ctx  = NULL;
    // 原始帧
    push.pFrame = av_frame_alloc();
    // 输出帧（转换后的720p图像）
    push.pFrameYUV = av_frame_alloc();
    push.pFrameYUV->format = AV_PIX_FMT_YUV420P;
    push.pFrameYUV->width = push.outputcodec->width;
    push.pFrameYUV->height = push.outputcodec->height;
    push.pFrameYUV->pts = 0;

    push.outBuffer = NULL;
    // 根据720p分辨率分配缓冲区
    push.outBuffer = (uint8_t *) av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                 push.pFrameYUV->width, push.pFrameYUV->height, 1));
    av_image_fill_arrays(push.pFrameYUV->data, push.pFrameYUV->linesize, push.outBuffer,
                         AV_PIX_FMT_YUV420P, push.pFrameYUV->width, push.pFrameYUV->height, 1);

    img_convert_ctx = sws_getContext(push.pCodecCtx->width, push.pCodecCtx->height, push.pCodecCtx->format,
                                     push.pFrameYUV->width, push.pFrameYUV->height, AV_PIX_FMT_YUV420P,
                                     SWS_BICUBIC, NULL, NULL, NULL);
    double numtimebase = 0;
    // 开始计时
    int64_t start_time = av_gettime();
    int got_picture = 0, enc_got_frame = 0;
    int vpts = 0;
    push.pFrameYUV->pts = 0;

    while(1)
    {	
        if ((ret = av_read_frame(push.ifmt_ctx, push.packet)) < 0)
            break;
        // 解码
        if ((ret = avcodec_send_packet(push.inputcodec, push.packet)) >= 0)
        {
            if (push.packet->stream_index == push.videoindex)
            {
                if (avcodec_receive_frame(push.inputcodec, push.pFrame) >= 0)
                {
                    sws_scale(img_convert_ctx, (const unsigned char* const*)push.pFrame->data, push.pFrame->linesize, 0,
                              push.pCodecCtx->height, push.pFrameYUV->data, push.pFrameYUV->linesize);
                    // 设置PTS
                    push.pFrameYUV->pts = numtimebase;
                    numtimebase += 1;
                }
                if ((ret = avcodec_send_frame(push.outputcodec, push.pFrameYUV)) >= 0)
                {
                    if ((ret = avcodec_receive_packet(push.outputcodec, push.outPkt)) >= 0)
                    {	
                        // 重设PTS和DTS（时间戳转换到输出流时间基）
                        push.outPkt->pts = av_rescale_q_rnd(push.outPkt->pts, push.outputcodec->time_base, push.out_stream->time_base, AV_ROUND_NEAR_INF);
                        push.outPkt->dts = push.outPkt->pts;
                        push.outPkt->duration = 40;
                        push.outPkt->pos = -1;

                        if (push.outPkt->pts == AV_NOPTS_VALUE)
                        {
                            AVRational time_base1 = push.ofmt_ctx->streams[push.videoindex]->time_base;
                            printf("输出流时间基的分子 = %d     输出时间基的分母 = %d\n", time_base1.den, time_base1.num);
                            int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(push.ifmt_ctx->streams[push.videoindex]->r_frame_rate);
                            push.outPkt->pts = (double)(numtimebase * calc_duration) / (double)(av_q2d(time_base1) * AV_TIME_BASE);
                            push.outPkt->dts = push.outPkt->pts;
                            push.outPkt->duration = (double)calc_duration / (double)(av_q2d(time_base1) * AV_TIME_BASE);
                        }

                        if (push.outPkt->stream_index == push.videoindex)
                        {
                            printf("outPkt->dts = %d\toutPkt->pts = %d\n", push.outPkt->dts, push.outPkt->pts);
                            AVRational time_base = push.ofmt_ctx->streams[push.videoindex]->time_base;
                            AVRational time_base_q = {1, AV_TIME_BASE};
                            int64_t pts_time = av_rescale_q(push.outPkt->pts, time_base, time_base_q);
                            printf("转换后的值 = %ld", pts_time);
                            int64_t now_time = av_gettime() - start_time;
                            printf("      时间差 = %d", now_time);
                            printf("      两帧之间的时间差 = %ld\n", pts_time - now_time);
                        }     
                        ret = av_interleaved_write_frame(push.ofmt_ctx, push.outPkt);
                        if (ret < 0)
                        {
                            printf("Error muxing packet\n");
                            break;
                        }
                    }
                    else
                    {
                        printf("失败3  ret=%d\n", ret);
                    }
                    av_packet_unref(push.outPkt);
                }
                else
                {
                    printf("失败2\n");
                    printf("ret=%d\n", ret);
                }
            }
        }
        av_packet_unref(push.packet);
    }
    avio_close(push.ofmt_ctx->pb);
    avformat_close_input(&push.ifmt_ctx);
    avformat_close_input(&push.ofmt_ctx);

    return 0;
}
