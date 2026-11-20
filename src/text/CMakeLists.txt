fv_module(KIND core
          DEPS fv_settings fv_focus fv_hotkeys fv_pyport fv_base)
fv_embed(data/common_words.json data/cleanup_prompts.json)
