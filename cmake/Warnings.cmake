add_library(lclva_warnings INTERFACE)

target_compile_options(lclva_warnings INTERFACE
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

if(LCLVA_WERROR)
    target_compile_options(lclva_warnings INTERFACE -Werror)
endif()
