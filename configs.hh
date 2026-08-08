#pragma once
#include <unordered_set>
#include <string_view>

#define DEFAULT_RECV_TIMEOUT_SEC 3 // time out limit for the client
#define DEFAULT_SEND_TIMEOUT_SEC 2

#define REQUEST_TIMEOUT 8  // 8 seconds to receive the request from the client
#define MAX_REQUEST_SIZE 1024*1024     // 1 MB
#define POST_REQ_MAX_BODY_SIZE 1024*1024 *2   //  2 MB

#define MAX_FILE_SIZE_TO_SERVER 1024*1024*10 // 10 MB

#define POST_ALLOWED_CONTENT_TYPES {"text/plain"}
 // now we will only accept POST requests with content-type  present and is text/plain

// #define POST_REQUIRE_CONTENT_LENGTH  true
#define ROOT_FOLDER "Resources/"    
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


