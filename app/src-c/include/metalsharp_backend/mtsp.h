#ifndef METALSHARP_BACKEND_MTSP_BASIC_H
#define METALSHARP_BACKEND_MTSP_BASIC_H

#include <stddef.h>
char* ms_mtsp_pipelines_json(const char* query);
char* ms_mtsp_default_rules_json(void);
char* ms_mtsp_launch_shape_json(const char* query);
char* ms_mtsp_prepare_json(const unsigned char*, size_t, int*);
char* ms_mtsp_recipe_json(const unsigned char*, size_t, int*);
char* ms_mtsp_doctor_json(const unsigned char*, size_t, int*);

#endif
