#include "endfields.h"
#include "ef_index.h"
#include "ef_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *ef_execute(struct ef_db *db, struct ef_cmd *cmd, const void *aux)
{
    struct ef_slot *slot;
    const uint64_t *next_ptr;
    const uint32_t *status_ptr;
    const uint32_t *hops_ptr;
    const uint8_t *byte_ptr;
    enum ef_err err;

    if (db == NULL || cmd == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }

    switch (cmd->opcode) {
    case EF_OP_GET_SLOT:
        return ef_get_slot(db, cmd->param);
    case EF_OP_CHASE:
        return ef_chase(db, (struct ef_slot *)ef_offset_to_ptr(db, cmd->param));
    case EF_OP_GET_FIELD:
        slot = ef_get_slot(db, cmd->param);
        return (slot == NULL) ? NULL : ef_get_field_ptr(slot, cmd->field_offset);
    case EF_OP_WRITE_PAYLOAD:
        err = ef_write_payload(db, cmd->param, aux, cmd->field_offset);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_SET_NEXT:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        next_ptr = (const uint64_t *)aux;
        err = ef_set_next_offset(db, cmd->param, *next_ptr);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_SET_STATUS:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        status_ptr = (const uint32_t *)aux;
        err = ef_set_status(db, cmd->param, *status_ptr);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_WRITE_FIELD:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        byte_ptr = (const uint8_t *)aux;
        err = ef_write_field(db, cmd->param, cmd->field_offset, *byte_ptr);
        return (err == EF_OK) ? ef_get_field_ptr(ef_get_slot(db, cmd->param), cmd->field_offset) : NULL;
    case EF_OP_ALLOC:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        err = ef_alloc_slot(db, (uint64_t *)aux);
        return (err == EF_OK) ? ef_get_slot(db, *(const uint64_t *)aux) : NULL;
    case EF_OP_FREE:
        err = ef_free_slot(db, cmd->param);
        return (err == EF_OK) ? (void *)1 : NULL;
    case EF_OP_CHASE_N: {
        uint32_t hops_done = 0;
        struct ef_slot *result;
        if (cmd->field_offset == 0) {
            if (aux == NULL) {
                ef_set_error(db, EF_ERR_NULL_ARG);
                return NULL;
            }
            hops_ptr = (const uint32_t *)aux;
            result = ef_chase_n(db, cmd->param, *hops_ptr, &hops_done);
        } else {
            result = ef_chase_n(db, cmd->param, cmd->field_offset, &hops_done);
        }
        cmd->field_offset = (uint8_t)(hops_done & 0xFFU);
        return result;
    }
    case EF_OP_INDEX_PUT:
    case EF_OP_INDEX_GET:
    case EF_OP_INDEX_REMOVE:
    case EF_OP_INDEX_CLEAR:
    case EF_OP_QUEUE_PUSH:
    case EF_OP_QUEUE_POP: {
        const uint8_t *key_aux;
        char *key_buf = NULL;
        const char *key = NULL;
        uint8_t key_len = 0;
        uint64_t *slot_buf;

        switch (cmd->opcode) {
        case EF_OP_INDEX_PUT:
        case EF_OP_INDEX_GET:
        case EF_OP_INDEX_REMOVE:
            if (aux == NULL) {
                ef_set_error(db, EF_ERR_NULL_ARG);
                return NULL;
            }
            key_aux = (const uint8_t *)aux;
            key_len = key_aux[0];
            /* Copy into a heap buffer with trailing NUL so ef_index_* can treat
             * it as a C string. The opcode family does not require aux to be
             * null-terminated by the caller. */
            key_buf = (char *)malloc((size_t)key_len + 1U);
            if (key_buf == NULL) {
                ef_set_error(db, EF_ERR_OOM);
                return NULL;
            }
            memcpy(key_buf, &key_aux[1], key_len);
            key_buf[key_len] = '\0';
            key = key_buf;
            break;
        default:
            break;
        }

        switch (cmd->opcode) {
        case EF_OP_INDEX_PUT:
            err = ef_index_put(db, key, cmd->param);
            free(key_buf);
            return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
        case EF_OP_INDEX_GET: {
            uint64_t found;
            err = ef_index_get(db, key, &found);
            free(key_buf);
            if (err != EF_OK) {
                return NULL;
            }
            slot_buf = (uint64_t *)malloc(sizeof(uint64_t));
            if (slot_buf == NULL) {
                ef_set_error(db, EF_ERR_OOM);
                return NULL;
            }
            *slot_buf = found;
            return slot_buf;
        }
        case EF_OP_INDEX_REMOVE:
            err = ef_index_remove(db, key);
            free(key_buf);
            return (err == EF_OK) ? (void *)1 : NULL;
        case EF_OP_INDEX_CLEAR:
            err = ef_index_clear(db);
            return (err == EF_OK) ? (void *)1 : NULL;
        case EF_OP_QUEUE_PUSH:
            err = ef_queue_push(db, aux, cmd->field_offset);
            return (err == EF_OK) ? (void *)1 : NULL;
        case EF_OP_QUEUE_POP: {
            void *pop_buf = (void *)(uintptr_t)aux;
            err = ef_queue_pop(db, pop_buf, cmd->field_offset, (size_t *)cmd->param);
            return (err == EF_OK) ? pop_buf : NULL;
        }
        default:
            free(key_buf);
            ef_set_error(db, EF_ERR_OPCODE);
            return NULL;
        }
    }
    default:
        ef_set_error(db, EF_ERR_OPCODE);
        return NULL;
    }
}
