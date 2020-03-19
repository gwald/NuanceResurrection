#ifndef PROPAGATE_CONSTANTS_H
#define PROPAGATE_CONSTANTS_H

#define PROPAGATE_CONSTANTS_STATUS_ECU_OK (0)
#define PROPAGATE_CONSTANTS_STATUS_RCU_OK (1)
#define PROPAGATE_CONSTANTS_STATUS_ALU_OK (2)
#define PROPAGATE_CONSTANTS_STATUS_MUL_OK (3)
#define PROPAGATE_CONSTANTS_STATUS_MEM_OK (4)
#define PROPAGATE_CONSTANTS_STATUS_ECU_NOP (5)
#define PROPAGATE_CONSTANTS_STATUS_HALT (6)
#define PROPAGATE_CONSTANTS_STATUS_BRANCH_ALWAYS_DIRECT (7)
#define PROPAGATE_CONSTANTS_STATUS_BRANCH_CONDITIONAL_DIRECT (8)
#define PROPAGATE_CONSTANTS_STATUS_BRANCH_ALWAYS_INDIRECT (9)
#define PROPAGATE_CONSTANTS_STATUS_BRANCH_CONDITIONAL_INDIRECT (10)
#define PROPAGATE_CONSTANTS_STATUS_JSR_ALWAYS_DIRECT (11)
#define PROPAGATE_CONSTANTS_STATUS_JSR_CONDITIONAL_DIRECT (12)
#define PROPAGATE_CONSTANTS_STATUS_JSR_ALWAYS_INDIRECT (13)
#define PROPAGATE_CONSTANTS_STATUS_JSR_CONDITIONAL_INDIRECT (14)
#define PROPAGATE_CONSTANTS_STATUS_RTS_ALWAYS (15)
#define PROPAGATE_CONSTANTS_STATUS_RTS_CONDITIONAL (16)
#define PROPAGATE_CONSTANTS_STATUS_RTI1_ALWAYS (17)
#define PROPAGATE_CONSTANTS_STATUS_RTI1_CONDITIONAL (18)
#define PROPAGATE_CONSTANTS_STATUS_RTI2_ALWAYS (19)
#define PROPAGATE_CONSTANTS_STATUS_RTI2_CONDITIONAL (20)
#define PROPAGATE_CONSTANTS_STORE_MISC_REGISTER_CONSTANT (0xFFFFFFFEUL)
#define PROPAGATE_CONSTANTS_STORE_SCALAR_REGISTER_CONSTANT (0xFFFFFFFFUL)

#include "basetypes.h"

class SuperBlockConstants;

typedef void (* PropagateConstantsHandler)(SuperBlockConstants &);
typedef void PropagateConstantsHandlerProto(SuperBlockConstants &);

#endif
