#include "FFmpegMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// ─── Impl
// ─────────────────────────────────────────────────────────────────────

struct FFmpegMuxer::Impl {
  AVFormatContext *fmtCtx = nullptr;

  // Video
  AVCodecContext *videoCtx = nullptr;
  AVStream *videoStream = nullptr;
  AVFrame *videoFrame = nullptr;
  SwsContext *swsCtx = nullptr;
  int swsSrcWidth = 0; // swsCtx 生成時の入力寸法（変わったら再生成）
  int swsSrcHeight = 0;
  int64_t videoFrameIdx = 0;

  // 音声・映像共通の時刻原点（秒）。
  // 映像・音声のどちらかが先に到着した方を原点とする。
  // 両者が同じ mach_absolute_time スケールの値を使うため、
  // 映像 PTSseconds - firstTs と 音声 PTS_seconds - firstTs が常に一致する。
  double firstTs = -1.0;

  // Audio
  AVCodecContext *audioCtx = nullptr;
  AVStream *audioStream = nullptr;
  AVFrame *audioFrame = nullptr;
  SwrContext *swrCtx = nullptr;
  AVAudioFifo *audioFifo = nullptr;
  int64_t audioPts = 0; // FIFO から emit した累積サンプル数
  int64_t audioStartSamples =
      0; // 映像 firstTs を 0 とした時の音声開始サンプル位置
  bool audioStartAligned = false; // 上記オフセットを確定したか

  FFmpegMuxer::Settings settings;
  bool opened = false;
  bool headerWritten = false;
};

// ─── helpers
// ──────────────────────────────────────────────────────────────────

/** エンコーダにフレームを送り、出てきた AVPacket を全部書き出す。
 *  frame == nullptr でフラッシュ（ドレイン）になる。 */
static void drainEncoder(AVCodecContext *ctx, AVStream *stream,
                         AVFormatContext *fmt, AVFrame *frame) {
  if (!ctx || !stream || !fmt)
    return;
  if (avcodec_send_frame(ctx, frame) < 0)
    return;

  AVPacket *pkt = av_packet_alloc();
  if (!pkt)
    return;

  while (avcodec_receive_packet(ctx, pkt) == 0) {
    av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
    pkt->stream_index = stream->index;
    av_interleaved_write_frame(fmt, pkt);
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
}

// ─── FFmpegMuxer ─────────────────────────────────────────────────────────────

FFmpegMuxer::FFmpegMuxer() : impl(std::make_unique<Impl>()) {}
FFmpegMuxer::~FFmpegMuxer() { close(); }

bool FFmpegMuxer::open(const Settings &settings) {
  close(); // 前回の状態をリセット
  impl->settings = settings;

  const std::string path = settings.outputFile.getFullPathName().toStdString();

  // ── format context ────────────────────────────────────────────
  if (avformat_alloc_output_context2(&impl->fmtCtx, nullptr, nullptr,
                                     path.c_str()) < 0)
    return false;
  AVFormatContext *fmt = impl->fmtCtx;

  // ── video stream ──────────────────────────────────────────────
  {
    // H.264: VideoToolbox HW encoder（fallback: libx264）
    // ProRes: VideoToolbox HW encoder（fallback: prores_ks SW）
    const char *hwName =
        settings.useProRes ? "prores_videotoolbox" : "h264_videotoolbox";
    const char *swName = settings.useProRes ? "prores_ks" : "libx264";
    const AVCodecID swId =
        settings.useProRes ? AV_CODEC_ID_PRORES : AV_CODEC_ID_H264;

    const AVCodec *codec = avcodec_find_encoder_by_name(hwName);
    const bool usingHWCodec = (codec != nullptr);
    if (!codec)
      codec = avcodec_find_encoder_by_name(swName);
    if (!codec)
      codec = avcodec_find_encoder(swId);
    if (!codec) {
      close();
      return false;
    }

    impl->videoStream = avformat_new_stream(fmt, nullptr);
    if (!impl->videoStream) {
      close();
      return false;
    }

    impl->videoCtx = avcodec_alloc_context3(codec);
    if (!impl->videoCtx) {
      close();
      return false;
    }

    AVCodecContext *vc = impl->videoCtx;
    vc->width = settings.width;
    vc->height = settings.height;
    vc->time_base = {1, settings.fps};
    vc->framerate = {settings.fps, 1};

    if (settings.useProRes) {
      // prores_videotoolbox: uyvy422 (packed 4:2:2, BGRA→UYVYはswscale対応済み)
      // prores_ks (SW):      yuv422p10le (planar 10-bit 4:2:2)
      vc->pix_fmt = usingHWCodec ? AV_PIX_FMT_UYVY422 : AV_PIX_FMT_YUV422P10LE;
      av_opt_set(vc->priv_data, "profile", "3", 0); // ProRes 422 HQ
    } else {
      vc->pix_fmt = AV_PIX_FMT_YUV420P;
      av_opt_set(vc->priv_data, "realtime", "1", 0); // VT: リアルタイムモード
    }

    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
      vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(vc, codec, nullptr) < 0) {
      close();
      return false;
    }
    if (avcodec_parameters_from_context(impl->videoStream->codecpar, vc) < 0) {
      close();
      return false;
    }
    impl->videoStream->time_base = vc->time_base;

    // ビデオフレームバッファ確保
    impl->videoFrame = av_frame_alloc();
    if (!impl->videoFrame) {
      close();
      return false;
    }
    impl->videoFrame->format = vc->pix_fmt;
    impl->videoFrame->width = vc->width;
    impl->videoFrame->height = vc->height;
    if (av_frame_get_buffer(impl->videoFrame, 0) < 0) {
      close();
      return false;
    }

    // swscale は初回フレームで実キャプチャ寸法が判明してから
    // 遅延生成する（writeVideoFrame() 内）。キャプチャ寸法は Retina /
    // 非 Retina / DAW ウィンドウサイズにより不定のため。
  }

  // ── audio stream ──────────────────────────────────────────────
  {
    // libfdk_aac (高品質) → 組み込み aac の順で試す
    const AVCodec *acodec = avcodec_find_encoder_by_name("libfdk_aac");
    if (!acodec)
      acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!acodec) {
      close();
      return false;
    }

    impl->audioStream = avformat_new_stream(fmt, nullptr);
    if (!impl->audioStream) {
      close();
      return false;
    }

    impl->audioCtx = avcodec_alloc_context3(acodec);
    if (!impl->audioCtx) {
      close();
      return false;
    }

    AVCodecContext *ac = impl->audioCtx;
    ac->sample_fmt =
        acodec->sample_fmts ? acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    ac->sample_rate = static_cast<int>(settings.sampleRate);
    ac->bit_rate = 192000;
    ac->time_base = {1, ac->sample_rate};
    av_channel_layout_default(&ac->ch_layout, settings.numChannels);

    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
      ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(ac, acodec, nullptr) < 0) {
      close();
      return false;
    }
    if (avcodec_parameters_from_context(impl->audioStream->codecpar, ac) < 0) {
      close();
      return false;
    }
    impl->audioStream->time_base = ac->time_base;

    // フレームバッファ（AAC: 1024 samples / frame）
    const int frameSize = ac->frame_size > 0 ? ac->frame_size : 1024;
    impl->audioFrame = av_frame_alloc();
    if (!impl->audioFrame) {
      close();
      return false;
    }
    impl->audioFrame->nb_samples = frameSize;
    impl->audioFrame->format = ac->sample_fmt;
    impl->audioFrame->sample_rate = ac->sample_rate;
    av_channel_layout_copy(&impl->audioFrame->ch_layout, &ac->ch_layout);
    if (av_frame_get_buffer(impl->audioFrame, 0) < 0) {
      close();
      return false;
    }

    // swresample: float planar → エンコーダの sample format
    {
      AVChannelLayout layout{};
      av_channel_layout_default(&layout, settings.numChannels);
      if (swr_alloc_set_opts2(&impl->swrCtx, &layout, ac->sample_fmt,
                              ac->sample_rate, &layout, AV_SAMPLE_FMT_FLTP,
                              ac->sample_rate, 0, nullptr) < 0) {
        av_channel_layout_uninit(&layout);
        close();
        return false;
      }
      av_channel_layout_uninit(&layout);
    }
    if (swr_init(impl->swrCtx) < 0) {
      close();
      return false;
    }

    // audio FIFO（サイズ可変入力を frame_size 単位でエンコードするため）
    impl->audioFifo = av_audio_fifo_alloc(ac->sample_fmt, settings.numChannels,
                                          frameSize * 4);
    if (!impl->audioFifo) {
      close();
      return false;
    }
  }

  // ── ファイルオープン & ヘッダ書き出し ─────────────────────────
  if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
      close();
      return false;
    }
  }
  if (avformat_write_header(fmt, nullptr) < 0) {
    close();
    return false;
  }

  impl->opened = true;
  impl->headerWritten = true;
  impl->videoFrameIdx = 0;
  impl->firstTs = -1.0;
  impl->audioPts = 0;
  return true;
}

void FFmpegMuxer::close() {
  if (!impl)
    return;

  if (impl->opened) {
    // 残留音声データをフラッシュ
    if (impl->audioFifo && impl->audioCtx && impl->audioFrame) {
      const int frameSize =
          impl->audioCtx->frame_size > 0 ? impl->audioCtx->frame_size : 1024;
      const int remaining = av_audio_fifo_size(impl->audioFifo);
      if (remaining > 0) {
        const bool canSmall = (impl->audioCtx->codec->capabilities &
                               AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
        const int toEncode = canSmall ? remaining : frameSize;

        // 新しいサイズでフレームを再確保
        av_frame_unref(impl->audioFrame);
        impl->audioFrame->nb_samples = toEncode;
        impl->audioFrame->format = impl->audioCtx->sample_fmt;
        impl->audioFrame->sample_rate = impl->audioCtx->sample_rate;
        av_channel_layout_copy(&impl->audioFrame->ch_layout,
                               &impl->audioCtx->ch_layout);
        av_frame_get_buffer(impl->audioFrame, 0);

        // FIFO から読み出し（残り < toEncode の場合は後続ゼロ埋め）
        av_audio_fifo_read(impl->audioFifo,
                           reinterpret_cast<void **>(impl->audioFrame->data),
                           remaining);
        impl->audioFrame->pts = impl->audioPts;
        impl->audioPts += toEncode;
        drainEncoder(impl->audioCtx, impl->audioStream, impl->fmtCtx,
                     impl->audioFrame);
      }
    }

    // エンコーダをドレイン（nullptr でフラッシュ）
    drainEncoder(impl->videoCtx, impl->videoStream, impl->fmtCtx, nullptr);
    drainEncoder(impl->audioCtx, impl->audioStream, impl->fmtCtx, nullptr);

    if (impl->headerWritten)
      av_write_trailer(impl->fmtCtx);
  }

  // ── リソース解放 ──────────────────────────────────────────────
  if (impl->swsCtx) {
    sws_freeContext(impl->swsCtx);
    impl->swsCtx = nullptr;
  }
  impl->swsSrcWidth = 0;
  impl->swsSrcHeight = 0;
  if (impl->swrCtx) {
    swr_free(&impl->swrCtx);
  }
  if (impl->audioFifo) {
    av_audio_fifo_free(impl->audioFifo);
    impl->audioFifo = nullptr;
  }
  if (impl->videoFrame) {
    av_frame_free(&impl->videoFrame);
  }
  if (impl->audioFrame) {
    av_frame_free(&impl->audioFrame);
  }
  if (impl->videoCtx) {
    avcodec_free_context(&impl->videoCtx);
  }
  if (impl->audioCtx) {
    avcodec_free_context(&impl->audioCtx);
  }

  if (impl->fmtCtx) {
    if (!(impl->fmtCtx->oformat->flags & AVFMT_NOFILE) && impl->fmtCtx->pb)
      avio_closep(&impl->fmtCtx->pb);
    avformat_free_context(impl->fmtCtx);
    impl->fmtCtx = nullptr;
  }

  impl->videoStream = nullptr;
  impl->audioStream = nullptr;
  impl->videoFrameIdx = 0;
  impl->firstTs = -1.0;
  impl->audioPts = 0;
  impl->audioStartSamples = 0;
  impl->audioStartAligned = false;
  impl->opened = false;
  impl->headerWritten = false;
}

void FFmpegMuxer::writeVideoFrame(const uint8_t *pixelData, int stride,
                                  int srcWidth, int srcHeight,
                                  double timestampSeconds) {
  if (!impl->opened || !impl->videoCtx || !impl->videoFrame)
    return;

  if (srcWidth <= 0 || srcHeight <= 0)
    return;

  // 初回 or キャプチャ寸法が変わったときに swscale コンテキストを作る。
  // エンコーダ出力寸法（settings.width/height）は一定。
  // キャプチャ == 出力 なら 1:1 コピー、それ以外は BICUBIC リサイズ。
  if (impl->swsCtx == nullptr || impl->swsSrcWidth != srcWidth ||
      impl->swsSrcHeight != srcHeight) {
    if (impl->swsCtx) {
      sws_freeContext(impl->swsCtx);
      impl->swsCtx = nullptr;
    }
    impl->swsCtx = sws_getContext(srcWidth, srcHeight, AV_PIX_FMT_BGRA,
                                  impl->settings.width, impl->settings.height,
                                  impl->videoCtx->pix_fmt, SWS_BICUBIC, nullptr,
                                  nullptr, nullptr);
    if (!impl->swsCtx)
      return;
    impl->swsSrcWidth = srcWidth;
    impl->swsSrcHeight = srcHeight;
  }

  if (av_frame_make_writable(impl->videoFrame) < 0)
    return;

  // BGRA → エンコーダの入力フォーマットに変換
  const uint8_t *srcData[4] = {pixelData, nullptr, nullptr, nullptr};
  const int srcLinesize[4] = {stride, 0, 0, 0};

  sws_scale(impl->swsCtx, srcData, srcLinesize, 0, srcHeight,
            impl->videoFrame->data, impl->videoFrame->linesize);

  // 実タイムスタンプ（CMSampleBufferGetPresentationTimeStamp）を使って PTS
  // を計算。 フレームカウンタ（++）では SCK の可変間隔配信による誤差が蓄積し
  // A/V ズレが起きる。
  // firstTs は必ず「映像の最初のフレーム」で決定する。
  // そうしないとトラック先頭の映像 PTS が 0 より大きなり、
  // プレイヤーがその間を黒画面で埋めてしまう。
  if (impl->firstTs < 0.0)
    impl->firstTs = timestampSeconds;

  const double relativeTs = timestampSeconds - impl->firstTs;
  impl->videoFrame->pts =
      static_cast<int64_t>(std::round(relativeTs * impl->settings.fps));

  drainEncoder(impl->videoCtx, impl->videoStream, impl->fmtCtx,
               impl->videoFrame);
}

void FFmpegMuxer::writeAudioSamples(const juce::AudioBuffer<float> &buffer,
                                    double timestampSeconds) {
  if (!impl->opened || !impl->audioCtx || !impl->audioFifo)
    return;

  const int numSamples = buffer.getNumSamples();
  const int numChannels =
      std::min(buffer.getNumChannels(), impl->settings.numChannels);

  // float planar 入力ポインタ配列を作成
  const uint8_t *inPtrs[8] = {};
  for (int ch = 0; ch < numChannels; ++ch)
    inPtrs[ch] = reinterpret_cast<const uint8_t *>(buffer.getReadPointer(ch));

  // swr_convert 結果を受け取る一時フレーム
  AVFrame *tmp = av_frame_alloc();
  if (!tmp)
    return;
  tmp->nb_samples = swr_get_out_samples(impl->swrCtx, numSamples);
  tmp->format = impl->audioCtx->sample_fmt;
  tmp->sample_rate = impl->audioCtx->sample_rate;
  av_channel_layout_copy(&tmp->ch_layout, &impl->audioCtx->ch_layout);
  if (av_frame_get_buffer(tmp, 0) < 0) {
    av_frame_free(&tmp);
    return;
  }

  tmp->nb_samples =
      swr_convert(impl->swrCtx, tmp->data, tmp->nb_samples, inPtrs, numSamples);

  av_audio_fifo_write(impl->audioFifo, reinterpret_cast<void **>(tmp->data),
                      tmp->nb_samples);
  av_frame_free(&tmp);

  // 音声と映像の時刻原点を一度だけ揃える。
  // - firstTs   : 必ず映像の最初のフレームで決定（writeVideoFrame でセット）
  // - audioStartSamples : 最初の音声チャンク先頭が、firstTs を 0
  // とした時に何サンプル目か pump 側が送る timestampSeconds
  // は連続サンプルストリームの「先頭」を指すので、
  // 一度オフセットを決めれば以降は audioPts（FIFO から emit
  // したサンプル数）を加算するだけで 単調増加・等間隔の PTS
  // になる（二重カウントを避ける）。
  //
  // 映像がまだ到着していない間は firstTs が未確定のため FIFO に溜めておくだけで
  // エンコードはしない。これによりトラック先頭の映像 PTS = 0 を保て、
  // プレイヤーが冲頭に黒画面を挿入しない。
  if (impl->firstTs < 0.0)
    return; // 映像の最初のフレームを待つ
  if (!impl->audioStartAligned) {
    impl->audioStartSamples = static_cast<int64_t>(std::round(
        (timestampSeconds - impl->firstTs) * impl->audioCtx->sample_rate));
    if (impl->audioStartSamples < 0)
      impl->audioStartSamples = 0; // 負の PTS は不可
    impl->audioStartAligned = true;
  }

  // FIFO に frame_size 分たまったらエンコード
  const int frameSize =
      impl->audioCtx->frame_size > 0 ? impl->audioCtx->frame_size : 1024;

  while (av_audio_fifo_size(impl->audioFifo) >= frameSize) {
    if (av_frame_make_writable(impl->audioFrame) < 0)
      break;
    impl->audioFrame->nb_samples = frameSize;
    av_audio_fifo_read(impl->audioFifo,
                       reinterpret_cast<void **>(impl->audioFrame->data),
                       frameSize);

    impl->audioFrame->pts = impl->audioStartSamples + impl->audioPts;
    impl->audioPts += frameSize;

    drainEncoder(impl->audioCtx, impl->audioStream, impl->fmtCtx,
                 impl->audioFrame);
  }
}
