#ifndef CLIB_VECTOR_H
#define CLIB_VECTOR_H
#include "../definitions.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include <stdint.h>
#include <stdlib.h>

// Return a replacement only on success; callers keep the old vector on failure.
static inline void *clib__vector_reserve(void *vector, uint64 element_size, uint64 minimum)
{
    cvector_metadata_t *replacement = NULL_POINTER;
    cvector_metadata_t *header = vector == NULL ? NULL : cvector_vec_to_base(vector);

    if (header != NULL && header->capacity >= minimum)
    {
        return vector;
    }
    if (element_size == 0 || minimum > (SIZE_MAX - sizeof(*header)) / element_size)
    {
        return NULL;
    }
    replacement = realloc(header, sizeof(*header) + minimum * element_size);
    if (replacement == NULL)
    {
        return NULL;
    }
    if (vector == NULL)
    {
        replacement->size = 0;
        replacement->elem_destructor = NULL;
    }
    replacement->capacity = minimum;
    return cvector_base_to_vec(replacement);
}

static inline boole clib__int_vector_reserve(int **vector, uint64 minimum)
{
    int *replacement = clib__vector_reserve(*vector, sizeof(int), minimum);

    if (replacement == NULL)
    {
        return FALSE;
    }
    *vector = replacement;
    return TRUE;
}

static inline boole clib__int_vector_push(int **vector, int value)
{
    uint64 count = cvector_size(*vector);

    if (count == SIZE_MAX || !clib__int_vector_reserve(vector, count + 1))
    {
        return FALSE;
    }
    (*vector)[count] = value;
    cvector_set_size(*vector, count + 1);
    return TRUE;
}
#endif
