#ifndef METALSHARP_BACKEND_MTSP_BASIC_H
#define METALSHARP_BACKEND_MTSP_BASIC_H

#include <stddef.h>
#include <stdio.h>
char* ms_mtsp_pipelines_json(const char* query);
char* ms_mtsp_default_rules_json(void);
char* ms_mtsp_default_rules_json_raw(void);
/* This build's rule set file (ms_wine_rules_file), or NULL. */
FILE* ms_mtsp_open_rules_file(void);
/* The Arm64 FEX build's lane for a regular rule's pipeline (configs/mtsp-rules-fex.toml is generated
 * from configs/mtsp-rules.toml with the same translation). */
const char* ms_mtsp_fex_lane(const char* regular);
char* ms_mtsp_launch_shape_json(const char* query);
char* ms_mtsp_prepare_json(const unsigned char*, size_t, int*);
char* ms_mtsp_recipe_json(const unsigned char*, size_t, int*);
char* ms_mtsp_doctor_json(const unsigned char*, size_t, int*);

#endif
