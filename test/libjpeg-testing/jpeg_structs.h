#ifndef JPEG_STRUCTS_H
#define JPEG_STRUCTS_H

// RLBox struct definitions for libjpeg-turbo types.
// Add rlbox_load_structs_from_library-compatible struct definitions here
// as needed when calling libjpeg-turbo functions across the sandbox boundary.
//
// Matches libjpeg-turbo with JPEG_LIB_VERSION=62 (< 70, < 80).
// Types are lowered to their underlying primitives:
//   boolean  -> int,  JDIMENSION -> unsigned int
//   UINT8    -> unsigned char,  UINT16 -> unsigned short
//   J_COLOR_SPACE / J_DCT_METHOD -> int  (C enums have underlying type int)
// The msg_parm union in jpeg_error_mgr is represented as its largest member:
//   char s[JMSG_STR_PARM_MAX] = char[80].

// clang-format off
#define sandbox_fields_reflection_jpeg_class_jpeg_error_mgr(f, g, ...)         \
  f(void (*)(j_common_ptr), error_exit, FIELD_NORMAL, ##__VA_ARGS__) g()       \
  f(void (*)(j_common_ptr, int), emit_message, FIELD_NORMAL, ##__VA_ARGS__) g() \
  f(void (*)(j_common_ptr), output_message, FIELD_NORMAL, ##__VA_ARGS__) g()   \
  f(void (*)(j_common_ptr, char *), format_message, FIELD_NORMAL, ##__VA_ARGS__) g() \
  f(void (*)(j_common_ptr), reset_error_mgr, FIELD_NORMAL, ##__VA_ARGS__) g()  \
  f(int, msg_code, FIELD_NORMAL, ##__VA_ARGS__) g()                             \
  f(char[80], msg_parm, FIELD_NORMAL, ##__VA_ARGS__) g()                        \
  f(int, trace_level, FIELD_NORMAL, ##__VA_ARGS__) g()                          \
  f(long, num_warnings, FIELD_NORMAL, ##__VA_ARGS__) g()                        \
  f(const char * const *, jpeg_message_table, FIELD_NORMAL, ##__VA_ARGS__) g() \
  f(int, last_jpeg_message, FIELD_NORMAL, ##__VA_ARGS__) g()                    \
  f(const char * const *, addon_message_table, FIELD_NORMAL, ##__VA_ARGS__) g() \
  f(int, first_addon_message, FIELD_NORMAL, ##__VA_ARGS__) g()                  \
  f(int, last_addon_message, FIELD_NORMAL, ##__VA_ARGS__) g()

// jpeg_compress_struct: jpeg_common_fields expanded inline, then
// compression-specific fields.  Version-gated blocks (#if JPEG_LIB_VERSION
// >= 70/80) are omitted because JPEG_LIB_VERSION is 62 for this build.
#define sandbox_fields_reflection_jpeg_class_jpeg_compress_struct(f, g, ...)   \
  /* jpeg_common_fields */                                                       \
  f(struct jpeg_error_mgr *, err, FIELD_NORMAL, ##__VA_ARGS__) g()              \
  f(struct jpeg_memory_mgr *, mem, FIELD_NORMAL, ##__VA_ARGS__) g()             \
  f(struct jpeg_progress_mgr *, progress, FIELD_NORMAL, ##__VA_ARGS__) g()      \
  f(void *, client_data, FIELD_NORMAL, ##__VA_ARGS__) g()                       \
  f(int, is_decompressor, FIELD_NORMAL, ##__VA_ARGS__) g()                      \
  f(int, global_state, FIELD_NORMAL, ##__VA_ARGS__) g()                         \
  /* destination and source image description */                                 \
  f(struct jpeg_destination_mgr *, dest, FIELD_NORMAL, ##__VA_ARGS__) g()       \
  f(unsigned int, image_width, FIELD_NORMAL, ##__VA_ARGS__) g()                 \
  f(unsigned int, image_height, FIELD_NORMAL, ##__VA_ARGS__) g()                \
  f(int, input_components, FIELD_NORMAL, ##__VA_ARGS__) g()                     \
  f(int, in_color_space, FIELD_NORMAL, ##__VA_ARGS__) g()                       \
  f(double, input_gamma, FIELD_NORMAL, ##__VA_ARGS__) g()                       \
  /* compression parameters */                                                   \
  f(int, data_precision, FIELD_NORMAL, ##__VA_ARGS__) g()                       \
  f(int, num_components, FIELD_NORMAL, ##__VA_ARGS__) g()                       \
  f(int, jpeg_color_space, FIELD_NORMAL, ##__VA_ARGS__) g()                     \
  f(jpeg_component_info *, comp_info, FIELD_NORMAL, ##__VA_ARGS__) g()          \
  f(JQUANT_TBL *[4], quant_tbl_ptrs, FIELD_NORMAL, ##__VA_ARGS__) g()          \
  f(JHUFF_TBL *[4], dc_huff_tbl_ptrs, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(JHUFF_TBL *[4], ac_huff_tbl_ptrs, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(unsigned char[16], arith_dc_L, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(unsigned char[16], arith_dc_U, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(unsigned char[16], arith_ac_K, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(int, num_scans, FIELD_NORMAL, ##__VA_ARGS__) g()                            \
  f(const jpeg_scan_info *, scan_info, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(int, raw_data_in, FIELD_NORMAL, ##__VA_ARGS__) g()                          \
  f(int, arith_code, FIELD_NORMAL, ##__VA_ARGS__) g()                           \
  f(int, optimize_coding, FIELD_NORMAL, ##__VA_ARGS__) g()                      \
  f(int, CCIR601_sampling, FIELD_NORMAL, ##__VA_ARGS__) g()                     \
  f(int, smoothing_factor, FIELD_NORMAL, ##__VA_ARGS__) g()                     \
  f(int, dct_method, FIELD_NORMAL, ##__VA_ARGS__) g()                           \
  f(unsigned int, restart_interval, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(int, restart_in_rows, FIELD_NORMAL, ##__VA_ARGS__) g()                      \
  /* JFIF / Adobe marker emission */                                             \
  f(int, write_JFIF_header, FIELD_NORMAL, ##__VA_ARGS__) g()                    \
  f(unsigned char, JFIF_major_version, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(unsigned char, JFIF_minor_version, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(unsigned char, density_unit, FIELD_NORMAL, ##__VA_ARGS__) g()               \
  f(unsigned short, X_density, FIELD_NORMAL, ##__VA_ARGS__) g()                 \
  f(unsigned short, Y_density, FIELD_NORMAL, ##__VA_ARGS__) g()                 \
  f(int, write_Adobe_marker, FIELD_NORMAL, ##__VA_ARGS__) g()                   \
  /* state */                                                                    \
  f(unsigned int, next_scanline, FIELD_NORMAL, ##__VA_ARGS__) g()               \
  f(int, progressive_mode, FIELD_NORMAL, ##__VA_ARGS__) g()                     \
  f(int, max_h_samp_factor, FIELD_NORMAL, ##__VA_ARGS__) g()                    \
  f(int, max_v_samp_factor, FIELD_NORMAL, ##__VA_ARGS__) g()                    \
  f(unsigned int, total_iMCU_rows, FIELD_NORMAL, ##__VA_ARGS__) g()             \
  /* per-scan state */                                                           \
  f(int, comps_in_scan, FIELD_NORMAL, ##__VA_ARGS__) g()                        \
  f(jpeg_component_info *[4], cur_comp_info, FIELD_NORMAL, ##__VA_ARGS__) g()   \
  f(unsigned int, MCUs_per_row, FIELD_NORMAL, ##__VA_ARGS__) g()                \
  f(unsigned int, MCU_rows_in_scan, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(int, blocks_in_MCU, FIELD_NORMAL, ##__VA_ARGS__) g()                        \
  f(int[10], MCU_membership, FIELD_NORMAL, ##__VA_ARGS__) g()                   \
  f(int, Ss, FIELD_NORMAL, ##__VA_ARGS__) g()                                   \
  f(int, Se, FIELD_NORMAL, ##__VA_ARGS__) g()                                   \
  f(int, Ah, FIELD_NORMAL, ##__VA_ARGS__) g()                                   \
  f(int, Al, FIELD_NORMAL, ##__VA_ARGS__) g()                                   \
  /* compression subobject pointers */                                           \
  f(struct jpeg_comp_master *, master, FIELD_NORMAL, ##__VA_ARGS__) g()         \
  f(struct jpeg_c_main_controller *, main, FIELD_NORMAL, ##__VA_ARGS__) g()     \
  f(struct jpeg_c_prep_controller *, prep, FIELD_NORMAL, ##__VA_ARGS__) g()     \
  f(struct jpeg_c_coef_controller *, coef, FIELD_NORMAL, ##__VA_ARGS__) g()     \
  f(struct jpeg_marker_writer *, marker, FIELD_NORMAL, ##__VA_ARGS__) g()       \
  f(struct jpeg_color_converter *, cconvert, FIELD_NORMAL, ##__VA_ARGS__) g()   \
  f(struct jpeg_downsampler *, downsample, FIELD_NORMAL, ##__VA_ARGS__) g()     \
  f(struct jpeg_forward_dct *, fdct, FIELD_NORMAL, ##__VA_ARGS__) g()           \
  f(struct jpeg_entropy_encoder *, entropy, FIELD_NORMAL, ##__VA_ARGS__) g()    \
  f(jpeg_scan_info *, script_space, FIELD_NORMAL, ##__VA_ARGS__) g()            \
  f(int, script_space_size, FIELD_NORMAL, ##__VA_ARGS__) g()

#define sandbox_fields_reflection_jpeg_allClasses(f, ...)                       \
  f(jpeg_error_mgr, jpeg, ##__VA_ARGS__)                                        \
  f(jpeg_compress_struct, jpeg, ##__VA_ARGS__)
// clang-format on

#endif
