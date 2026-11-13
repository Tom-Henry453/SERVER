#include<iostream>
#include <cstdlib>
#include <string>
#include<cstring>
#include<unistd.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<netdb.h>
#include <mutex>
#include<thread>
#include"Logger.hh"
#include<sstream>
#include<fstream>
#include<unordered_map>
#include<filesystem>
#include<expected>
#include<algorithm>
#include<fcntl.h>
#include<string_view>
#include<atomic>
#include<ctime>
#define DEBUG_MODE 0
#include"CONFIGS.hh"
#include"HTTP_request.hh"
#include"thread_pool.hh"
using namespace std;

std::string get_mime_type(const std::filesystem::path& p){
    static const std::unordered_map<std::string, std::string> mime_map = {
        {".html", "text/html"},
        {".css",  "text/css"},
        {".js",   "application/javascript"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".txt",  "text/plain"}
    };
    auto ext = p.extension().string();  // ie .html
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower); // case insensitive
    auto it = mime_map.find(ext);
    if (it != mime_map.end()) {
        return it->second;
    }
    return "application/octet-stream";
}

std::pair<bool,std::filesystem::path> is_path_safe(const std::filesystem::path& requested_path, const std::filesystem::path& root_folder=ROOT_FOLDER){
    namespace fs = std::filesystem; // for easier access
    //canonical path: an absolute path that includes no symlinks, "." or ".." elements.
    try{
        auto doc_root = fs::canonical(root_folder); // get absolute path of root folder and resolve any symlinksm and possibly
        auto full = doc_root / requested_path; // concatenate requested path with root folder
        auto full_sanitized = std::filesystem::canonical(full); //
        auto rel = std::filesystem::relative(full_sanitized, doc_root); //
        // If relative path starts with "..", it escaped!
        if( rel.string().starts_with("..") ){  // escapde the root folder probably unsafe to server
            return {false, ""};
        }
        // Path is safe, return the full sanitized path.
        if(is_directory(full_sanitized)){
            return {false, full_sanitized};
        }
        else{
            return {true, full_sanitized};
        }
    }
    catch(...){
        return {false, ""};
    }
}
bool open_file(std::filesystem::path path, string &buffer){
    using namespace std;
    ifstream file(path, ios::binary);
    if(!file.is_open()){
        return false;
    }
    file.seekg(0, ios::end);
    size_t file_size = file.tellg();// give the location of file pointer
    if (file_size > MAX_FILE_SIZE_TO_SERVER){
        return false;
    }
    file.seekg(0, ios::beg);
    buffer.resize(file_size);
    file.read(buffer.data(), file_size);
    return true;
}

std::optional<std::filesystem::path> map_request_to_file(http_request& request){
    namespace fs = std::filesystem;
    // Normalize the path to handle oddities like "//" or "/./"
    fs::path requested_path= fs::path(request.path).lexically_normal();
    // Make it a purely relative path by removing any leading slash.
    requested_path = requested_path.relative_path(); // remove leading slash like /index.html to index.html
    // If the path is nowempty or just a dot, it's a request for the root. Serve index.html.
    if (requested_path.empty() || requested_path == ".") {
        requested_path = "index.html";
    }
    auto [is_safe, local_file_path] = is_path_safe(requested_path);
    if(!is_safe)
        return std::nullopt;
    return local_file_path;
}
std::string build_http_response(STATUS_CODES status= STATUS_CODES::OK, const std::string& file_buffer="", const std::string& mime_type="text/html"){
    std::string response;
    std::string status_line;
    switch(status){
        case STATUS_CODES::OK:
            status_line = "HTTP/1.1 200 OK\r\n";
            break;
        case STATUS_CODES::NOT_FOUND:
            status_line = "HTTP/1.1 404 Not Found\r\n";
            break;
        case STATUS_CODES::BAD_REQUEST:
            status_line = "HTTP/1.1 400 Bad Request\r\n";
            break;
        case STATUS_CODES::FORBIDDEN:
            status_line = "HTTP/1.1 403 Forbidden\r\n";
            break;
        case STATUS_CODES::INTERNAL_SERVER_ERROR:
            status_line = "HTTP/1.1 500 Internal Server Error\r\n";
            break;
        default:
            status_line = "HTTP/1.1 500 Internal Server Error\r\n";
            break;
    }
    response += status_line;
    response += "Content-Type: " + mime_type + "\r\n";
    response += "Content-Length: " + std::to_string(file_buffer.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += file_buffer;
    return response;
}

void handle_client(int client_fd, const struct sockaddr_in& client_addr, Logger& logs){
    // set timeout on the client socket to mitigate slowris attacks
     utils::set_socket_timeout(client_fd,DEFAULT_RECV_TIMEOUT_SEC,DEFAULT_SEND_TIMEOUT_SEC);
     utils::configure_socket(client_fd);
    auto raw_request = http_request::receive_request(client_fd);
    if(!raw_request){
        close(client_fd);
        //get the error code from the unexpected result
        int error_code = raw_request.error();
        logs.add_log(error_code, client_addr, LogLevel::ERROR);
        return;
    }

    auto parsed_request = http_request::parse_request(raw_request.value());
    if(!parsed_request){
        int error_code = parsed_request.error();
        logs.add_log(error_code, client_addr, LogLevel::ERROR);
        // unsupported method (code 5) -> 404 until F1 introduces 405; anything else is malformed -> 400
        bool bad_method = (error_code == 5);
        std::string err_resp = build_http_response(
            bad_method ? STATUS_CODES::NOT_FOUND : STATUS_CODES::BAD_REQUEST,
            bad_method ? "404 Not Found" : "400 Bad Request",
            "text/plain");
        send(client_fd, err_resp.c_str(), err_resp.size(), 0);
        close(client_fd);
        return;
    }

    auto requested_file_path_opt = map_request_to_file(parsed_request.value());
    if(!requested_file_path_opt){
        logs.add_log(10, client_addr, LogLevel::ERROR);
        std::string err_resp = build_http_response(STATUS_CODES::NOT_FOUND, "404 Not Found", "text/plain");
        send(client_fd, err_resp.c_str(), err_resp.size(), 0);
        close(client_fd);
        return;
    }
    std::string file_buffer;
    if(!open_file(requested_file_path_opt.value(), file_buffer)){
        logs.add_log(10, client_addr, LogLevel::ERROR);
        std::string err_resp = build_http_response(STATUS_CODES::NOT_FOUND, "404 Not Found", "text/plain");
        send(client_fd, err_resp.c_str(), err_resp.size(), 0);
        close(client_fd);
        return;
    }
    std::string mime_type = get_mime_type(requested_file_path_opt.value());
    string final_response = build_http_response(STATUS_CODES::OK, file_buffer, mime_type);
    send(client_fd, final_response.c_str(), final_response.size(), 0);
    close(client_fd);
    return ;
}
void start_server(struct sockaddr_in& server_addr, int& server_fd){
    server_fd = socket(AF_INET, SOCK_STREAM, 0); // create end of communication channel
    if(server_fd < 0){
        throw std::runtime_error("socket creation failed");
    }
    // set socket options to reuse address and port
    int  reuse = true;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
        throw std::runtime_error("setsockopt failed");
    }
    // now we need to bind the socket to a specific port and address so that it can listen for incoming connections.
    //create a sockaddr_in structure to hold the address and port information
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // listen on all available interfaces
    server_addr.sin_port = htons(4221); // port number in network byte order
    if( ::bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr)) != 0){
        throw std::runtime_error("bind failed");
    }
    // now call listen on the server socket to start listening for incoming connections. The second argument specifies the maximum number of pending connections that can be queued up before the kernel starts rejecting new ones.
    int connection_backlog =  4096 ; // maximum number of pending connections
    if(listen(server_fd, connection_backlog) != 0){
        throw std::runtime_error("listen failed");
    }
}
int main() {
    #if DEBUG_MODE
    std::cout << "Logs from your program will appear here!\n";
    #endif
    struct sockaddr_in server_addr; int server_fd;
    try{
        start_server(server_addr, server_fd);
    }
    catch(const std::exception& e){
        cerr << "Error starting server: "<<e.what()<< endl;
        return 1;
    }
    #if DEBUG_MODE
    std::cout<<"Waiting for a client to connect...\n";
    #endif
    Logger& logs = Logger::getInstance("Logs.txt");
    thread_pool<LogEntry> log_queue(1,2000);
    thread_pool<task> worker_pool(4,8000);
    while(true)
    {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd =accept(server_fd,(struct sockaddr *)&client_addr,(socklen_t *)&client_len);
        if(client_fd < 0){
            log_queue.enqueue(LogEntry{6, client_addr, LogLevel::ERROR});
            continue;
        }
        log_queue.enqueue(LogEntry{8, client_addr, LogLevel::INFO});
            #if DEBUG_MODE
            std::cout<<"Client connected successfully! IP =  "<<std::to_string(client_addr.sin_addr.s_addr);
            #endif
            auto closure = [ client_fd , client_addr , log=std::ref(logs) ] (){
                handle_client(client_fd, client_addr, log.get() );
            };
            // add the task
            worker_pool.enqueue( task{ std::move(closure) } );
    }
    close(server_fd);

    return 0;
}
