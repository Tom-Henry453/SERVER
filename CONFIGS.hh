#pragma once
#include <unordered_set>
#include <string_view>

#define MAX_SIZE_REQUEST          (1024 * 1024)        // 1 MB — total accumulated request
#define MAX_REQUEST_SIZE          (1024 * 1024)        // alias used in receive_request
#define POST_REQ_MAX_BODY_SIZE    (1 * 1024 * 1024)    // 1 MB — A4 body cap
#define MAX_FILE_SIZE_TO_SERVER   (10 * 1024 * 1024)   // 10 MB — A3 file size cap
#define ROOT_FOLDER               "Resources/"

#define DEFAULT_RECV_TIMEOUT_SEC 3 // time out limit for the client
#define DEFAULT_SEND_TIMEOUT_SEC 2
#define REQUEST_TIMEOUT 8          // 8 seconds to receive the request from the client
// HTTP Methods and other related constants
inline const std::unordered_set<std::string_view> ALLOWED_METHODS = {"GET"};
enum class SUPPORTED_HTTP_METHODS{
    GET,
};// create an alis ( type alaising) for long enum name
using HTTP_METHOD = SUPPORTED_HTTP_METHODS;
enum class STATUS_CODES{
    OK = 200,
    NOT_FOUND = 404,
    BAD_REQUEST = 400,
    FORBIDDEN = 403,
    INTERNAL_SERVER_ERROR = 500
};

// socket options 
#define BLOCK_SIG_PIPE 1
