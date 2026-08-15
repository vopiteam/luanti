project(aes C)

add_library(aes STATIC aes_ctr.c aes_ctr.h)

target_include_directories(aes INTERFACE .)
