fv_module(KIND core
          DEPS fv_base fv_pyport fv_settings
          WIN32_LIBS user32)
fv_embed(data/keyboard_names.json)
