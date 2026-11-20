fv_module(KIND feature
          DEPS fv_base fv_pyport fv_settings fv_history fv_meeting_state fv_meeting fv_appkit
               fv_ui_theme fv_ui_widgets fv_ui_shell fv_ui_history fv_ui_settings fv_audio
               fv_models fv_llm fv_ui_downloads fv_appcore fv_clipboard fv_dictation fv_http
               fv_log fv_automation fv_meeting_web fv_net fv_speech fv_meeting_agent fv_meeting_intel fv_remote_host fv_remote fv_agents fv_credentials fv_process fv_ui_controller)
fv_feature(ID meeting CLASS fv::MeetingFeature HEADER ui_meeting/meeting_feature.h)
