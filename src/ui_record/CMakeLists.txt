fv_module(KIND feature
          DEPS fv_base fv_pyport fv_log fv_settings fv_appcore fv_audio fv_recorder fv_history
               fv_models fv_speech fv_dictation fv_hotkeys fv_appkit fv_ui_theme fv_ui_widgets
               fv_ui_shell fv_focus fv_clipboard fv_credentials fv_http fv_text fv_llm fv_remote)
fv_feature(ID record CLASS fv::RecordFeature HEADER ui_record/record_feature.h)
