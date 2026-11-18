#ifndef CLIB_VECTOR_H
#define CLIB_VECTOR_H
#include "../definitions.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include <stdint.h>
#include <stdlib.h>

// return a replacement only on success; callers keep the old vector on failure
/**
 * @brief grows a vector while preserving its contents
 * keep the original pointer until this succeeds
 *
 * @param void * vector -> existing vector or NULL
 * @param uint64 element_size -> bytes per element
 * @param uint64 minimum -> required element capacity
 *
 * @return void * -> updated vector or NULL on invalid size or allocation failure
 */
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

/**
 * @brief grows an integer vector and updates its pointer on success
 *
 * @param int ** vector -> address of the vector pointer
 * @param uint64 minimum -> required element capacity
 *
 * @return boole -> TRUE when the requested capacity is available
 */
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

/**
 * @brief appends an integer after reserving space
 *
 * @param int ** vector -> address of the vector pointer
 * @param int value -> integer to append
 *
 * @return boole -> TRUE when appended, otherwise FALSE with the vector unchanged
 */
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
