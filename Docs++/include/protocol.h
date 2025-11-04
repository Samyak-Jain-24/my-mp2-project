#ifndef PROTOCOL_H
#define PROTOCOL_H

// Message codes between components

enum MsgType {
    MSG_REGISTER_SS = 1,
    MSG_REGISTER_CLIENT = 2,
    MSG_VIEW = 10,
    MSG_INFO = 11,
    MSG_LIST_USERS = 12,
    MSG_ADD_ACCESS = 13,
    MSG_REM_ACCESS = 14,
    MSG_LOCATE = 15,
    MSG_CREATE = 16,
    MSG_DELETE = 17,
    MSG_EXEC = 18,

    MSG_READ = 30,
    MSG_WRITE_BEGIN = 31,
    MSG_WRITE_UPDATE = 32,
    MSG_WRITE_END = 33,
    MSG_UNDO = 34,
    MSG_STREAM = 35
};

// Error codes

enum ErrorCode {
    OK = 0,
    UNAUTHORIZED = 1,
    NOT_FOUND = 2,
    ALREADY_EXISTS = 3,
    INVALID_REQUEST = 4,
    SERVER_ERROR = 5,
    LOCKED = 6,
    CONFLICT = 7,
    UNAVAILABLE = 8
};

#endif // PROTOCOL_H
