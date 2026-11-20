fv_module(KIND feature
          DEPS fv_agentapi fv_base fv_pyport fv_remote fv_history fv_credentials fv_net fv_ui_widgets
               fv_ui_settings fv_appkit fv_ui_theme fv_appcore fv_remote_host fv_settings fv_speech fv_models fv_db fv_remote_records fv_ui_controller fv_ui_shell fv_dictation fv_ui_history)
fv_feature(ID remote CLASS fv::RemoteFeature HEADER ui_remote/remote_feature.h)
