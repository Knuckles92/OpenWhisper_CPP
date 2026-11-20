fv_module(KIND core
          DEPS fv_base fv_pyport fv_settings
          PRIVATE_THIRD_PARTY portaudio FFmpeg::avformat FFmpeg::avcodec FFmpeg::swresample
                              FFmpeg::avutil
          WIN32_LIBS WIL::WIL ole32)
