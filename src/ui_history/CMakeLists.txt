fv_module(KIND feature
          DEPS fv_agents fv_net fv_base fv_pyport fv_log fv_settings fv_history fv_audio fv_speech fv_appkit
               fv_ui_theme fv_ui_widgets fv_ui_shell fv_remote)
fv_feature(ID history CLASS fv::HistoryFeature HEADER ui_history/history_feature.h)
