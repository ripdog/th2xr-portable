#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum th2_parameter_type {
    TH2_PARAMETER_END = 0,
    TH2_PARAMETER_BYTE = 1,
    TH2_PARAMETER_NUMBER = 2,
    TH2_PARAMETER_STRING8 = 3,
    TH2_PARAMETER_STRING16 = 4,
    TH2_PARAMETER_REGISTER = 5,
    TH2_PARAMETER_COMPARE = 6,
    TH2_PARAMETER_ADD = 7,
    TH2_PARAMETER_COUNT = 8,
    TH2_PARAMETER_VOICE_COUNT = 9,
};

size_t th2_event_count(void);
const char* th2_event_name(uint16_t opcode);
const char* th2_event_parameters(uint16_t opcode);
int th2_event_waits(uint16_t opcode);

#ifdef __cplusplus
}
#endif
