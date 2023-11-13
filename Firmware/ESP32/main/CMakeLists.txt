idf_component_register(SRCS "openhaystack_main.c" "uECC.c"
                           
                    INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE  -Wno-format)
