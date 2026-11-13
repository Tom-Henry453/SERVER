#include <unordered_set>
#include <string_view>
#include <cmath>
#pragma once
#include <cerrno>
#include <sys/socket.h>
#include "CONFIGS.hh"
#include<cmath>
#include<chrono>
#include <expected>
namespace utils{
    inline std::string_view trim(std::string_view s) {
        size_t start = s.find_first_not_of(" \t");
        if ( start == string_view::npos) return {}; // empty view
        size_t end = s.find_last_not_of(" \t");
        if ( end == string_view::npos) return {};
        return s.substr(start, end - start + 1);
    }
    inline void trim(std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        if ( start == string::npos) return; // empty string
        size_t end = s.find_last_not_of(" \t");
        if ( end == string::npos) return;
        s=s.substr(start, end - start + 1);
    }
         /* time out struct contain time out values
        that will be used to stop slowris attacks
        we mitigate by applying timeouts on the client sockets
        */
    bool set_socket_timeout(int client_fd, double recv_timeout_sec, double send_timeout_sec=-1){
        auto to_timeval = [](double s) -> struct timeval {
            double whole, frac = std::modf(s, &whole);
            return { static_cast<time_t>(whole), static_cast<suseconds_t>(frac * 1000000.0) };
        };
        struct timeval rt = to_timeval(recv_timeout_sec);
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt)) !=0 ) return false;
        if (send_timeout_sec >= 0){
            struct timeval st = to_timeval(send_timeout_sec);
            if( setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st)) !=0 ) return false;
        }
        return true;
    }
    bool check_timeout(const auto& start_time,const auto& timeout_duration) {
        if( std::chrono::steady_clock::now()  - start_time >= timeout_duration ) return true ; // timeed out
        return false;
    }
    void configure_socket(int client_fd){
        int block{0};
        if constexpr( BLOCK_SIG_PIPE == 1){
            block=1;
        }
        setsockopt(client_fd,SOL_SOCKET,SO_NOSIGPIPE,&block,sizeof(block));
    }
    std::expected<bool,int> try_send_all(int fd,const char* buffer,size_t buff_size,int total_sending_time){
        size_t offset{};
        // start the timer
        auto start_time = std::chrono::steady_clock::now();
        while( offset < buff_size ){
            if( check_timeout ( start_time , std::chrono::seconds( total_sending_time ) ) )
                return std::unexpected(12);

            ssize_t bytes_copied = send(fd,(buffer + offset),buff_size - offset,0);
            if( bytes_copied > 0) {
                offset+=bytes_copied; continue;
            }
            if( bytes_copied == -1 ){
                    if( errno == EINTR || errno == EAGAIN){ // if signal received in send syscall , jsut continiue
                        continue;
                    }else {
                        return std::unexpected(13);
                    }
            }
        }
        return true;
    }
};

class http_request{
   public:
   std::string method;
   std::string path;
   std::string version;
   std::unordered_map<std::string,std::string> headers;
   std::string body;

public:
    /*
    its job is to receive the request from the client and return it as a string.
    */
    static std::expected<std::string,int> receive_request (const int client_fd){
        char buffer[4096] ;// 1 KB Chunk on stack
        std::string request{};
        size_t header_ending_idx = -1;
        auto start_time = std::chrono::steady_clock::now(); // record start time
        while(true){
            /* recv is a system call that takes client fd to get data and put it in a provided buffer */
            ssize_t bytes_from_socket=recv(client_fd,buffer,sizeof(buffer),0);
            if(bytes_from_socket < 0){
                if(errno == EINTR) continue; // if interrupted by signal, retry
                return std::unexpected(2); // error occurred
            }
            if(bytes_from_socket == 0)  // client closed the connection
                return std::unexpected(1);
            // look for timeout in the whole process of receiving the request
            if( utils::check_timeout(start_time, std::chrono::seconds(REQUEST_TIMEOUT))  ){
                return std::unexpected(11); // timeout error
            }
            request.append(buffer,bytes_from_socket); // append the bytes received to request str
            if(request.size() > MAX_REQUEST_SIZE){ // MAX_SIZE ALLOWED is 1 KB
                return std::unexpected(3); // error occurred
            }
            /* we continuously look for headers(key value pairs ) ending cuz that when we have to start receiving the body of request
            if we  find CRLF we know we have reached the end of headers and go for recving body
            */
            header_ending_idx = request.find("\r\n\r\n");  // look for CRLF
            if(header_ending_idx != std::string::npos){ // if found
                break; // go for checking content length field
            }
        }
        header_ending_idx +=4; // ignore CLRF
        // now we have to find content-lenght key to ge tits value
        int content_length=0;// check for content length header and find its length
        // create the cheap string view over HEADERS( request line + header)
        std::string_view headers (request.data(),(size_t)header_ending_idx);
        // and look fo Content_lenght header
        size_t content_field_idx= headers.find("\r\nContent-Length:"); // find context lenght header
            if(content_field_idx != std::string::npos){
                // now find starting index of context_len value starting after content length substring
                size_t start_idx = content_field_idx +strlen("\r\nContent-Length:");
                size_t end_idx = headers.find("\r\n", start_idx);
                std::string_view content = headers.substr(start_idx, end_idx - start_idx);
                content = utils::trim(content);
                    try{
                        content_length = std::stoi(string{content});
                    }
                    catch(...){
                        return unexpected(4); // malformed content length header
                    }
                    if(content_length < 0 || content_length > POST_REQ_MAX_BODY_SIZE){ // if content length is negative or greater than max allowed
                        return unexpected(3);
                    }
                // receive that many bytes
                int already_rec_bytes = request.size() - header_ending_idx;
                int remaining_bytes = content_length - already_rec_bytes;
                    int received_bytes = 0;
                    while(remaining_bytes > 0){
                        received_bytes = recv(client_fd,buffer,
                            min((size_t)remaining_bytes,sizeof(buffer)),0);
                        if( received_bytes < 0){
                            if( errno == EINTR) continue; // if interrupted by signal, retry
                            return std::unexpected(2);
                        }
                        if(received_bytes ==0)// client closed connection
                            return std::unexpected(1);
                        if( utils::check_timeout(std::chrono::steady_clock::now(), std::chrono::seconds(REQUEST_TIMEOUT)) ){
                            return std::unexpected(11);
                        }
                        request.append(buffer,received_bytes);
                        remaining_bytes -= received_bytes;
                    }
            }
            return std::expected<std::string,int>(request);
    }
    bool is_valid_method(const std::string& method){
        return ALLOWED_METHODS.count(method) > 0;
    }
    /*
    its job is just to parse the request and return a http_request object with all the fields filled in.
    if the request is malformed or has an unsupported method, it will return an error code.
    */
    static std::expected<http_request,int> parse_request(std::string& raw_request){
        http_request req;
        std::string_view raw_request_view(raw_request); // create view over the raw request
        size_t headers_ending_idx= raw_request.find("\r\n\r\n");
        if(headers_ending_idx == std::string::npos){
            return std::unexpected(3); // malformed request
        }
        std::string_view header_str_view =raw_request_view.substr(0,headers_ending_idx); // just a view into headers
        req.body = raw_request.substr(headers_ending_idx+4); // skip /r/n/r/n

        size_t first_line_idx = raw_request_view.find("\r\n");
        if( first_line_idx == std::string::npos ){ // strange we did not find any request line  in the request
            return std::unexpected(3); // malformed request
        }
        std::string_view request_line = raw_request_view.substr(0,first_line_idx); // extract request line
        {
            size_t sp1_idx = request_line.find(' '); // find first space
            if(sp1_idx == std::string::npos)
                return std::unexpected(3); // malformed request
            size_t sp2_idx = request_line.find(' ', sp1_idx + 1); // find second space
            if(sp2_idx == std::string::npos)
                return std::unexpected(3); // malformed request
            req.method = string{request_line.substr(0, sp1_idx)}; // extract method
            req.path = string{request_line.substr(sp1_idx + 1, sp2_idx - sp1_idx - 1)}; // extract path
            req.version = string{request_line.substr(sp2_idx + 1)}; // extract version
            // it should be not really there
            if(!req.is_valid_method(req.method)){
                return std::unexpected(5); // unsupported method
            }
            if(req.method.empty() || req.path.empty() || req.version.empty() ){
                return std::unexpected(3); // malformed request
            }
        }
        // header parsing
        size_t header_idx=first_line_idx + 2; // skip \r\n in request
         while(header_idx < header_str_view.size()){
            size_t line_ending_idx=header_str_view.find("\r\n",header_idx);
            if(line_ending_idx == std::string::npos){ // since we skipped the CRLF now
                line_ending_idx = header_str_view.size(); // last header without CRLF at end of headers
            }
            string_view header = header_str_view.substr(header_idx,line_ending_idx-header_idx);
            header_idx = line_ending_idx+2; // skip \r\n and set header index to next header
            size_t colon_idx=header.find(':');
            if(colon_idx ==  string_view::npos){
                return std::unexpected(3); // malformed header no colon found
            }
            string_view key = utils::trim(header.substr(0,colon_idx));
            string_view value = utils::trim(header.substr(colon_idx+1));
            if(key.empty() || value.empty()){
                return std::unexpected(3); // malformed header
            }
            req.headers[string{key}]=string{value};
         }
        return req;
    }
};


// class tcp_connection{
//     struct
// }
