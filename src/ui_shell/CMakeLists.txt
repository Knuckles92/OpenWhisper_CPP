fv_module(KIND feature
          DEPS fv_base fv_pyport fv_log fv_settings fv_credentials fv_uimath fv_engine fv_appkit
               fv_ui_theme fv_ui_widgets fv_appcore fv_audio fv_db fv_http fv_models fv_speech
               fv_backup fv_net fv_agents fv_process fv_agentapi fv_update
          QT Network)
fv_feature(ID shell CLASS fv::ShellFeature HEADER ui_shell/shell_feature.h)
