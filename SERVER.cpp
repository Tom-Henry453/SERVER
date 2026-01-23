#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <mutex>
#include <thread>
#include "Logger.hh"
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <algorithm>
 #include <fcntl.h>
#define DEBUG_MODE 0
#define MAX_SIZE_REQUEST 1024 * 1024 // 1 MB 
#define ROOT_FOLDER "Resources/"

using namespace std;

enum class STATUS_CODES{
    OK = 200,
    NOT_FOUND = 404,
    BAD_REQUEST = 400,
    FORBIDDEN = 403,
    INTERNAL_SERVER_ERROR = 500
};

enum class HTTP_METHODS{
    GET,
    POST,
};

class http_request{
   public:
   std::string method;
   std::string path;
   std::string version;
   std::unordered_map<std::string,std::string> headers;
   std::string body;
};

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    if (start == std::string::npos || end == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}
std::pair<int,string> read_request(int client_fd){
    char buffer[4096] ;// 1 KB Chunk on stack
    std::string request{};
    int header_ending_idx = -1;
    while(true){
        ssize_t bytes_from_socket=recv(client_fd,buffer,sizeof(buffer),0);
        if(bytes_from_socket ==  0){ // Client closed  its TCP connection 
            return {0,request};  // no more bytes will ever reach
        }
        request.append(buffer,bytes_from_socket);
        header_ending_idx = request.find("\r\n\r\n");
        if(header_ending_idx != std::string::npos){ // request from client has been complated
            break; // ignore body for now 
        }
        if(request.size() > MAX_SIZE_REQUEST){ // MAX_SIZE ALLOWED is 
           return{0,request};
        }
    }
    header_ending_idx +=4; // ignore CLRF we have ending indx og header in header-ending _index
    bool has_content_field= false;
    int content_length=0;// check for content length header and find its length
    std::string headers = request.substr(0,header_ending_idx);
    size_t content_field_idx= headers.find("Content-Length:");

    if(content_field_idx != std::string::npos){
        has_content_field = true; 
        // now find starting index of context_len value starting after content length substring
        // extract 
        std::string content= headers.substr(
            content_field_idx+strlen("Content-Length:"),
            headers.find("\r\n",content_field_idx) -(content_field_idx+strlen("Content-Length:")));
        content = trim(content);
            try{
                content_length = std::stoi(content);
            }
            catch(...){ 
                return {0,request};
            }
        // receive that many bytes
        int already_rec_bytes = request.size() - header_ending_idx;

        int remaining_bytes = content_length - already_rec_bytes;
            int received_bytes = 0;
            while(remaining_bytes > 0){
                received_bytes = recv(client_fd,buffer,sizeof(buffer),0);
                if(received_bytes <= 0)
                    return {0,request};
                request.append(buffer,received_bytes);
                remaining_bytes -= received_bytes;

            }
    }
    else{
        return {1,request}; // no body present  
    }

    return {1,request};
}

std::pair<bool,http_request> parse_request(const std::string& raw_request){
        http_request req; 
        ssize_t header_end= raw_request.find("\r\n\r\n");
        if(header_end == std::string::npos){
            return {false,req};
        }
        string header_str =raw_request.substr(0,header_end);
        req.body = raw_request.substr(header_end+4); // skip /r/n/r/n
        ssize_t first_line_idx = header_str.find("\r\n");
        if(first_line_idx==std::string::npos){
            return {false,req};
        }
        string first_line = header_str.substr(0,first_line_idx); // extract request line
        std::stringstream ss(first_line);
        ss >> req.method >> req.path >> req.version; 
        if(req.method.empty() || req.version.empty()){
            return {false,req};
        }

        ssize_t header_pos=first_line_idx + 2; // skip \r\n in request
         while(header_pos < header_str.size()){
            int line_end=header_str.find("\r\n",header_pos);
            if(line_end == std::string::npos){
                break; // we did not find any other header
            }
            std::string header = header_str.substr(header_pos,line_end-header_pos);
            header_pos = line_end+2; // skip \r\n
            ssize_t colon_idx=header.find(":");
            if(colon_idx == std::string::npos){
                continue; // invalid header formate
            }
                std::string key = header.substr(0,colon_idx);
                std::string value = header.substr(colon_idx+1);
                key.erase(0, key.find_first_not_of(" \t"));
                size_t last_not_of = key.find_last_not_of(" \t");
                if (last_not_of != std::string::npos) key.erase(last_not_of + 1);
                
                value.erase(0, value.find_first_not_of(" \t"));
                last_not_of = value.find_last_not_of(" \t");
                if (last_not_of != std::string::npos) value.erase(last_not_of + 1);

                req.headers[key]=value;
         }
        return {true,req};
}

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
    namespace fs = std::filesystem;
    try{
        auto doc_root = fs::weakly_canonical(root_folder);
        auto full = doc_root / requested_path;
        auto full_sanitized = std::filesystem::weakly_canonical(full);
        auto rel = std::filesystem::relative(full_sanitized, doc_root);
        // If relative path starts with "..", it escaped!
        if(!rel.empty() && rel.string().find("..") == 0 || full_sanitized.string().find(doc_root.string()) != 0){
            return {false, ""};
        }
        // Path is safe, return the full sanitized path.
        if(is_directory(full_sanitized)){
            return {true, full_sanitized};
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
    auto [read_status, raw_request] = read_request(client_fd);
    if(!read_status){
        close(client_fd); 
        logs.add_log(9, client_addr, LogLevel::ERROR); 
        return;
    }
    auto [parse_status, req_obj] = parse_request(raw_request);
    if(!parse_status){
        close(client_fd);
        logs.add_log(9, client_addr, LogLevel::ERROR);
        return;
    }
    auto requested_file_path_opt = map_request_to_file(req_obj);
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

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "Logs from your program will appear here!\n";

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create server socket\n";
        return 1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt failed\n";
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET; 
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    server_addr.sin_port = htons(4221); 

    if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
        std::cerr << "Failed to bind to port 4221\n";
        return 1;
    }

    int connection_backlog = 5;
    if (listen(server_fd, connection_backlog) != 0) {
        std::cerr << "listen failed\n";
        return 1;
    }

    std::cout << "Waiting for a client to connect...\n";
    Logger& logs = Logger::getInstance("Logs.txt");
    while(true)
    {
        struct sockaddr_in client{}; 
        socklen_t client_len = sizeof(client);
        int client_fd = accept(server_fd, (struct sockaddr *) &client, (socklen_t *) &client_len);
        if(client_fd < 0){
            logs.add_log(6, client, LogLevel::ERROR);
            continue; 
        }
        else{
            logs.add_log(8, client, LogLevel::INFO); 
            if(DEBUG_MODE){
                std::cout<<"Client connected successfully! IP =  "<<std::to_string(client.sin_addr.s_addr);
            }
        }
        std::thread client_thread(handle_client, client_fd, client, std::ref(logs));
        client_thread.detach(); // Detach the thread to allow independent execution
    }
    close(server_fd);
    // kill all threads
    
    
    return 0;
}