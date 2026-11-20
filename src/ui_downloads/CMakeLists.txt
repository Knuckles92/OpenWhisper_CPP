fv_module(KIND feature
          DEPS fv_base fv_settings fv_credentials fv_http fv_speech fv_models fv_history fv_uimath
               fv_appkit fv_ui_theme fv_ui_widgets fv_ui_settings fv_ui_record
               fv_ui_shell fv_remote fv_remote_host)
fv_feature(ID downloads CLASS fv::DownloadsFeature HEADER ui_downloads/downloads_feature.h)
