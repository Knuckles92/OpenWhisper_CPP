fv_module(KIND core
          DEPS fv_base fv_pyport fv_settings
          WIN32_LIBS WIL::WIL ole32 oleaut32 user32)
fv_embed(data/app_catalog.json)
