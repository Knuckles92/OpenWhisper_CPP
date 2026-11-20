# fv_bench: the developer benchmarks (OpenWhisper's benchmarks/ and scripts/benchmark_*), as a
# library so tests/bench can link them; openwhisper-cpp-bench (src/bench_cli) is its console front.
fv_module(KIND core
          DEPS fv_base fv_pyport fv_dsp fv_audio fv_engine fv_speech fv_text fv_llm fv_focus
               fv_dictation fv_meeting_capture fv_meeting_state fv_process fv_settings
               fv_credentials fv_http
          PRIVATE_THIRD_PARTY pugixml::pugixml
          WIN32_LIBS psapi)
fv_embed(data/cleanup_golden.jsonl)
