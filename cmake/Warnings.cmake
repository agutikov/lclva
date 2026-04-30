add_library(acva_warnings INTERFACE)

target_compile_options(acva_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
)

if(ACVA_WERROR)
    target_compile_options(acva_warnings INTERFACE -Werror)
endif()
