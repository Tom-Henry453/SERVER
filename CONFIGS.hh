#pragma once
#define MAX_SIZE_REQUEST          (1024 * 1024)        // 1 MB — total accumulated request
#define MAX_REQUEST_SIZE          (1024 * 1024)        // alias used in receive_request
#define POST_REQ_MAX_BODY_SIZE    (1 * 1024 * 1024)    // 1 MB — A4 body cap
#define MAX_FILE_SIZE_TO_SERVER   (10 * 1024 * 1024)   // 10 MB — A3 file size cap
#define ROOT_FOLDER               "Resources/"