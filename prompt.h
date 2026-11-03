#ifndef PROMPT_H
#define PROMPT_H

typedef enum { V_OK, V_DENIED, V_GONE, V_ABORT } VerifyResult;

// Ask for the password (GUI first, terminal as fallback), up to MAX_ATTEMPTS tries.
VerifyResult verifyPassword(const char* disk);

#endif
