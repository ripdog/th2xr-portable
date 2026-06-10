#include "event_metadata.h"

#include "../ToHeart2/ScriptEngine/mes/Escr.h"

size_t th2_event_count(void)
{
    return ESC_OPR_MAX - ESC_B;
}

const char* th2_event_name(uint16_t opcode)
{
    if (opcode < ESC_B || opcode >= ESC_OPR_MAX) {
        return NULL;
    }
    return EventOprList[opcode - ESC_B].opr;
}

const char* th2_event_parameters(uint16_t opcode)
{
    if (opcode < ESC_B || opcode >= ESC_OPR_MAX) {
        return NULL;
    }
    return EScroptOpr[opcode - ESC_B].type;
}

int th2_event_waits(uint16_t opcode)
{
    if (opcode < ESC_B || opcode >= ESC_OPR_MAX) {
        return 0;
    }
    return EScroptOpr[opcode - ESC_B].ret == ESC_WAIT;
}
