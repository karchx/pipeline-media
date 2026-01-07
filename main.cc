#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <fstream>
#include <webp/decode.h>
#include <webp/demux.h>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
    #include <libavutil/dict.h>
    #include <libavcodec/packet.h>
}

struct Frame {
    std::vector<uint8_t> data;
    int width;
    int height;
    int duration_ms;
};

class PipeConversionManager {
private:
    std::string file;
    std::mutex mtx;
    std::queue<std::string> tasks;
    std::queue<Frame> frames;

    void log(const std::string &message) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "[Thread  " << std::this_thread::get_id() << "] " << message << std::endl;
    }

   void generate_output_filename(std::string &input_filename) {
        size_t last_dot_pos = input_filename.rfind('.');

        if (last_dot_pos != std::string::npos && last_dot_pos != 0) {
            input_filename.erase(last_dot_pos);
        }

        input_filename.append(".mp4");
    }

    void decode_video(
        AVCodecContext *codec_ctx,
        AVPacket *pkt,
        AVFrame *av_frame,
        AVFrame *rgba_frame,
        AVStream *video_stream,
        SwsContext *sws_ctx
    ) {
        AVRational time_base = video_stream->time_base;
        int64_t prev_pts = 0;
        bool first_frame = true;
        if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
            while (avcodec_receive_frame(codec_ctx, av_frame) == 0) {
                sws_scale(
                    sws_ctx,
                    av_frame->data,
                    av_frame->linesize,
                    0,
                    codec_ctx->height,
                    rgba_frame->data,
                    rgba_frame->linesize
                );

                int64_t current_pts = av_frame->pts;
                int duration_ms = 0;

                if (first_frame) {
                    if (video_stream->avg_frame_rate.num > 0) {
                        duration_ms = (1000 * video_stream->avg_frame_rate.den) / video_stream->avg_frame_rate.num;
                    } else {
                        duration_ms = 33;
                    }
                    first_frame = false;
                } else {
                    int64_t pts_diff = current_pts - prev_pts;
                    duration_ms = av_rescale_q(pts_diff, time_base, {1, 1000});
                    if (duration_ms <= 0) duration_ms = 33;
                }

                prev_pts = current_pts;

                Frame frame;
                frame.width = codec_ctx->width;
                frame.height = codec_ctx->height;
                frame.duration_ms = duration_ms;

                size_t data_size = codec_ctx->width * codec_ctx->height * 4;
                frame.data.resize(data_size);

                for (int y = 0; y < codec_ctx->height; ++y) {
                    std::memcpy(
                        frame.data.data() + y * codec_ctx->width * 4,
                        rgba_frame->data[0] + y * rgba_frame->linesize[0],
                        codec_ctx->width * 4
                    );
                }

                frames.push(frame);
            }
        }
    }

public:
    void add_task(const std::string &filename) {
        tasks.push(filename);
    }

    void worker() {
        while (true) {
            std::string current_file;
            // get file from queue
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (tasks.empty()) return;
                current_file = tasks.front();
                tasks.pop();
            }
            process_webp_to_mp4(current_file);
        }
    }

    std::vector<uint8_t> read_file(const char *path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            log("Failed to open file: " + std::string(path));
        }

        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        };
    }

    bool decoder_webm(std::string input_file) {
        AVFormatContext *fmt_ctx = nullptr;
        AVCodecContext *codec_ctx = nullptr;
        AVFrame *av_frame = nullptr;
        AVFrame *rgba_frame = nullptr;
        AVPacket *pkt = nullptr;
        SwsContext *sws_ctx = nullptr;
        int video_stream_index = -1;
        int audio_stream_index = -1;
        // added input audio stream

        if (avformat_open_input(&fmt_ctx, input_file.c_str(), nullptr, nullptr) < 0) {
            log("Could not open input file: " + input_file);
            return false;
        }

        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            log("Could not find stream information");
            avformat_close_input(&fmt_ctx);
            return false;
        }

        for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = i;
                break;
            } else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = i;
            }
        }

        if (video_stream_index == -1) {
            log("Could not find video stream in the input file");
            avformat_close_input(&fmt_ctx);
            return false;
        }

        AVStream *video_stream = fmt_ctx->streams[video_stream_index];

        const AVCodec *codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!codec) {
            log("Could not find decoder");
            avformat_close_input(&fmt_ctx);
            return false;
        }

        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            log("Could not allocate codec context");
            avformat_close_input(&fmt_ctx);
            return false;
        }

        // Copy codec parameters to context
        if (avcodec_parameters_to_context(codec_ctx, video_stream->codecpar) < 0) {
            log("Could not copy codec parameters to context");
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            log("Could not open codec");
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }

        av_frame = av_frame_alloc();
        rgba_frame = av_frame_alloc();
        pkt = av_packet_alloc();

        if (!av_frame || !rgba_frame || !pkt) {
            log("Could not allocate frames or packet");
            return false;
        }

        rgba_frame->format = AV_PIX_FMT_RGBA;
        rgba_frame->width = codec_ctx->width;
        rgba_frame->height = codec_ctx->height;
        rgba_frame->sample_rate = codec_ctx->sample_rate;

        if (av_image_alloc(rgba_frame->data, rgba_frame->linesize, rgba_frame->width, rgba_frame->height, AV_PIX_FMT_RGBA, 32) < 0) {
            log("Could not allocate RGBA image");
            return false;
        }

        sws_ctx = sws_getContext(
            codec_ctx->width,
            codec_ctx->height,
            codec_ctx->pix_fmt,
            codec_ctx->width,
            codec_ctx->height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );

        if (!sws_ctx) {
            log("Could not initialize SwsContext");
            return false;
        }

        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == video_stream_index) {
                decode_video(codec_ctx, pkt, av_frame, rgba_frame, video_stream, sws_ctx);
            }
            av_packet_unref(pkt);
        }

        avcodec_send_packet(codec_ctx, nullptr);
        while (avcodec_receive_frame(codec_ctx, av_frame) == 0) {
            sws_scale(
                sws_ctx,
                av_frame->data,
                av_frame->linesize,
                0,
                codec_ctx->height,
                rgba_frame->data,
                rgba_frame->linesize
            );

            Frame frame;
            frame.width = codec_ctx->width;
            frame.height = codec_ctx->height;
            frame.duration_ms = 33;

            size_t data_size = codec_ctx->width * codec_ctx->height * 4;
            frame.data.resize(data_size);

            for (int y = 0; y < codec_ctx->width; ++y) {
                std::memcpy(
                    frame.data.data() + y * codec_ctx->width * 4,
                    rgba_frame->data[0] + y * rgba_frame->linesize[0],
                    codec_ctx->width * 4
                );
            }

            frames.push(frame);
        }

        if (rgba_frame) {
            av_freep(&rgba_frame->data[0]);
            av_frame_free(&rgba_frame);
        }
        if (av_frame) av_frame_free(&av_frame);
        if (pkt) av_packet_free(&pkt);
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);

        return !frames.empty();
    }

    void decoder(WebPData web_data) {
        Frame frame;
        WebPAnimDecoderOptions dec_options;

        if (!WebPAnimDecoderOptionsInit(&dec_options)) {
            log("Failed to initialize WebPAnimDecoderOptions");
            return;
        }
        dec_options.color_mode = MODE_RGBA;

        // create decoder
        WebPAnimDecoder *dec = WebPAnimDecoderNew(&web_data, &dec_options);

        if (!dec) {
            log("Failed to create WebPAnimDecoder");
            return;
        }

        WebPAnimInfo anim_info;
        if (!WebPAnimDecoderGetInfo(dec, &anim_info)) {
            log("Failed to get animation info");
            WebPAnimDecoderDelete(dec);
            return;
        }
        uint8_t *buf = nullptr;
        int prev_timestamp = 0;
        while(WebPAnimDecoderHasMoreFrames(dec)) {
            int timestamp_ms = 0;
            if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp_ms)) {
                log("Failed to get next frame");
                break;
            }
            int duration = timestamp_ms - prev_timestamp;
            prev_timestamp = timestamp_ms;

            frame.width = anim_info.canvas_width;
            frame.height = anim_info.canvas_height;
            frame.duration_ms = duration;
            frame.data.resize(frame.width * frame.height * 4);
            std::memcpy(frame.data.data(), buf, frame.data.size());
            {
                std::lock_guard<std::mutex> lock(mtx);
                frames.push(frame);
            }
        }

        WebPAnimDecoderDelete(dec);
    }

    void encoder_mp4(std::queue<Frame> &frames, std::string output_file) {
        if (frames.empty()) return;
        Frame first_frame = frames.front();
        int canvas_width = first_frame.width;
        int canvas_height = first_frame.height;
        generate_output_filename(output_file);

        AVFormatContext *fmt_ctx = nullptr;
        if (avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, output_file.c_str()) < 0) {
            log("Could not allocate output context");
            return;
        }

        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            log("H.264 codec not found");
            avformat_free_context(fmt_ctx);
            return;
        }

        AVStream *stream = avformat_new_stream(fmt_ctx, codec);
        if (!stream) {
            log("Could not create new stream");
            avformat_free_context(fmt_ctx);
            return;
        }

        AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            log("Could not allocate codec context");
            avformat_free_context(fmt_ctx);
            return;
        }

        codec_ctx->bit_rate = 400000;
        codec_ctx->width = canvas_width;
        codec_ctx->height = canvas_height;
        codec_ctx->time_base = {1, 1000};
        codec_ctx->gop_size = 10;
        codec_ctx->max_b_frames = 1;
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

        if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "pt", "slow", 0);
        av_dict_set(&opts, "crf", "23", 0);

        if (avcodec_open2(codec_ctx, codec, &opts) < 0) {
            log("Could not open codec");
            avcodec_free_context(&codec_ctx);
            avformat_free_context(fmt_ctx);
            return;
        }

        av_dict_free(&opts);

        if (avcodec_parameters_from_context(stream->codecpar, codec_ctx) < 0) {
            log("Could not copy codec parameters");
            avcodec_free_context(&codec_ctx);
            avformat_free_context(fmt_ctx);
            return;
        }

        if (avio_open(&fmt_ctx->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
            log("Could not open output file");
            avcodec_free_context(&codec_ctx);
            avformat_free_context(fmt_ctx);
            return;
        }

        if (avformat_write_header(fmt_ctx, nullptr) < 0) {
            log("Could not write header");
            avio_closep(&fmt_ctx->pb);
            avcodec_free_context(&codec_ctx);
            avformat_free_context(fmt_ctx);
            return;
        }

        AVPacket *pkt = av_packet_alloc();
        int current_pts = 0;
        SwsContext *sws_ctx = sws_getContext(
            canvas_width, canvas_height, AV_PIX_FMT_RGBA,
            canvas_width, canvas_height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        while (!frames.empty()) {
            Frame frame = frames.front();
            frames.pop();

            if (!sws_ctx) {
                log("Could not initialize SwsContext");
                av_write_trailer(fmt_ctx);
                avio_closep(&fmt_ctx->pb);
                avcodec_free_context(&codec_ctx);
                avformat_free_context(fmt_ctx);
                return;
            }

            // Create AVFrame for YUV
            AVFrame *av_frame = av_frame_alloc();
            if (!av_frame) {
                log("Could not allocate AVFrame");
                return;
            }
            av_frame->format = AV_PIX_FMT_YUV420P;
            av_frame->width = frame.width;
            av_frame->height = frame.height;
            av_frame->pts = current_pts;

            if (av_image_alloc(
                av_frame->data, av_frame->linesize,
                frame.width, frame.height, AV_PIX_FMT_YUV420P, 32
            ) < 0) {
                log("Could not allocate image");
                av_frame_free(&av_frame);
                return;
            }

            // convert RGBA to YUV420P
            uint8_t *in_data[1] = { frame.data.data() };
            int in_linesize[1] = { 4 * frame.width };

            sws_scale(
                sws_ctx,
                in_data,
                in_linesize,
                0,
                frame.height,
                av_frame->data,
                av_frame->linesize
            );

            current_pts += frame.duration_ms;

            // send encoder
            if(avcodec_send_frame(codec_ctx, av_frame) >= 0) {
                while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                    av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
                    pkt->stream_index = stream->index;
                    av_interleaved_write_frame(fmt_ctx, pkt);
                    av_packet_unref(pkt);
                }
            }

            av_freep(&av_frame->data[0]);
            av_frame_free(&av_frame);
        }

        // flush
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }

        av_write_trailer(fmt_ctx);
        av_packet_free(&pkt);

        sws_freeContext(sws_ctx);
        avio_closep(&fmt_ctx->pb);
        avcodec_free_context(&codec_ctx);
        avformat_free_context(fmt_ctx);
    }

    void remuxing_to_mp4(const std::string &input_file) {
        AVFormatContext *input_ctx = nullptr, *output_ctx = nullptr;
        std::string output_file = input_file;
        generate_output_filename(output_file);
        encoder_mp4(frames, input_file);
    }

    bool process_webp_to_mp4(const std::string &input_file) {
        // auto data = read_file(input_file.c_str());
        // if (data.empty()) {
        //     log("Failed to read file: " + input_file);
        //     return false;
        // }
        // WebPData webp_data = { data.data(), static_cast<size_t>(data.size()) };
        // remuxing_to_mp4(input_file);
        // decoder(webp_data);
        // encoder(frames, input_file);
        //
        if (decoder_webm(input_file)) {
            encoder_mp4(frames, input_file);
        } else {
            log("Failed to decode webm file: " + input_file);
            return false;
        }

        return true;
    }
};

int main() {
    PipeConversionManager manager;
    // manager.add_task("input.webp");
    // manager.add_task("input2.webp");
    manager.add_task("test.webm");

    const int num_threads = std::thread::hardware_concurrency();
    std::vector<std::thread> pool;

    for (int i = 0; i < num_threads; ++i) {
        pool.emplace_back(&PipeConversionManager::worker, &manager);
    }

    for (auto &t : pool) {
        t.join();
    }
    return 0;
}
