fv_module(KIND feature
          DEPS fv_base fv_pyport fv_settings fv_uimath fv_appkit fv_ui_theme fv_ui_widgets
               fv_speech fv_recorder)
fv_feature(ID overlay CLASS fv::OverlayFeature HEADER ui_overlay/overlay_feature.h)
