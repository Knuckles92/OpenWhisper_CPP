fv_module(KIND core
          DEPS fv_base fv_pyport fv_settings fv_http fv_download fv_process
          WIN32_LIBS advapi32 user32)

fv_embed(data/release_identity.json)
