#include<mutex>
#include<thread>
#include<unordered_map>
#include <unistd.h> // For write() and close()
#include<chrono>
#include<ctime>
// class deleter{
//     public:
//     void operator()
// }
#ifndef LOGS_HH
#define LOGS_HH
template<typename T>
class resource_traits;
// speacialization for file descriptors
template<>
resource_traits<FILE*>{
    public:
    void _write(FILE* file_ptr,const string& msg,const string& ip){
        string time_stamp;
        auto now=chrono::system_clock::now();   
        time_t now_time=chrono::system_clock::to_time_t(now);
        time_stamp=ctime(&now_time);
        time_stamp.pop_back(); // remove the newline character added by ctime
        msg+=" from ip:"+ip;
        msg="["+time_stamp+"]:"+msg;
        fwrite(msg.c_str(),sizeof(char),msg.size(),file_ptr);   
        flush(file_ptr); // call kernal to immediately write data from buffer to file
    }
    void _close(FILE* file_ptr){
        flush(file_ptr); // flush any remaining data in buffer to file
        fclose(file_ptr); //  close the file pointer
    }
    friend class logger<FILE*>;
};
template<typename T>
class logger{
    string file_name;
    unique_ptr<resource_traits<FILE>*> ptr; // it will manage the file pointer resource
    static mutex mtx; // to make sure only one thread writes to log file at a time
    static unordered_map<int,string> ERRORS; // map to store different logger instances for different files
    public:
    logger(const string& log_file="server_logs.txt"):file_name(log_file){
        FILE* file_ptr=fopen(file_name.c_str(),"a+");
        fseek(file_ptr,0,SEEK_END); // move the file pointer to the end of file
        ptr=make_unique<resource_traits<FILE*>>(file_ptr);
    }
    ~logger(){
        ptr->_close(ptr.get());// get the underlying File pointer and close the file;
    }
    void add_log(const sockaddr_in& ip_address,const string& error_message,const &string ip){ // adds the user ip address aalong with error message to logfile
        string error = ERRORS[error_code];
        string ip = inet_ntoa(ip_address.sin_addr);
        lock_guard<mutex> lock(mtx);

        _write(ptr.get(),error_message+"\n",ip);
        // unlock mutux explicitly
        lock_guard.unlock(mtx);
    }
};
mutex logger::mtx;
unordered_map<int,string> logger::ERRORS={
    {1,"Failed to create server socket"},
    {2,"setsockopt failed"},
    {3,"Failed to bind to port 4221"},
    {4,"Failed to listen on port 4221"},
    {5,"Failed to accept incoming connection"},
    {6,"Failed to read from client"},
    {7,"Failed to write to client"},
    {8,"Connection closed by server after sending response"}
};
#endif