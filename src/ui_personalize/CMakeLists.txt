fv_module(KIND feature
          DEPS fv_agents fv_ui_settings fv_ui_hotkeys fv_ui_widgets fv_ui_theme fv_appkit fv_settings
               fv_uimath fv_text fv_speech fv_llm fv_recorder)
fv_feature(ID personalize CLASS fv::PersonalizeFeature
           HEADER ui_personalize/personalize_feature.h)
