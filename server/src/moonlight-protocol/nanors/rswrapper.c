/**
 * @file src/rswrapper.c
 * @brief Keeps Wolf's rswrapper.h interface on top of upstream nanors.
 * @details nanors now picks its SIMD backend at runtime itself, so the per-ISA builds of rs.c
 * that this file used to do are gone; the pointers just forward to the upstream API.
 */
#include "rswrapper.h"

reed_solomon_new_t reed_solomon_new_fn = reed_solomon_new;
reed_solomon_release_t reed_solomon_release_fn = reed_solomon_release;
reed_solomon_encode_t reed_solomon_encode_fn = reed_solomon_encode;
reed_solomon_decode_t reed_solomon_decode_fn = reed_solomon_decode;
