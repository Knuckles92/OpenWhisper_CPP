# Stands in for src/appkit: the registry includes the real appkit/feature.h from FV_REAL_SRC.
fv_module(KIND qt
          QT Widgets)
target_include_directories(fv_appkit INTERFACE "${FV_REAL_SRC}")
