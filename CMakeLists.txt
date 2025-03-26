set(srcs "src/esp_nimble_api.c")
set(include "include")
set(priv_requires bt)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include}
    PRIV_REQUIRES "${priv_requires}"
)