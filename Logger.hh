#ifndef Logger_HH
#define Logger_HH

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <source_location> // C++20 feature for automatic file/line logging
using namespace std;
// Enum to define different levels of logging
enum class LogLevel {
    INFO,
    WARNING,
    ERROR
};

// Helper to convert LogLevel to a string
inline const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
    }
    return "NONE";
}

class Logger {
public:
    // Delete copy constructor and assignment operator to prevent copying
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Static method to get the single instance of the logger (Singleton pattern)
    static Logger& getInstance(const std::string& log_file = "server_logs.txt") {
        static Logger instance(log_file);
        return instance;
    }

    // The main logging function
    // Uses C++20's std::source_location to automatically capture file, line, and function
    void add_log(int error_code, const struct sockaddr_in& client_addr, LogLevel level=LogLevel::INFO, const std::source_location& location = std::source_location::current()) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        // Get current time
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        
        #ifdef _WIN32
            localtime_s(&now_tm, &now_time_t);
        #else
            localtime_r(&now_time_t, &now_tm);
        #endif
        char time_buf[100];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &now_tm);
        std::string message = ERRORS.count(error_code) ? ERRORS[error_code] : "Unknown error code";
        // if(error_code ==8)
        message += " | Client IP: " + std::string(inet_ntoa(client_addr.sin_addr)) + " Port: " + std::to_string(ntohs(client_addr.sin_port));
        // Format the log message
        log_stream_ << "[" << time_buf << "]"
                    << "[" << LogLevelToString(level) << "]"
                    << "[" << location.file_name() << ":" << location.line() << " " << location.function_name() << "]"
                    << " " << message << std::endl; // std::endl also flushes the stream forces to write immediately to file
    }
private:
    // Private constructor to enforce Singleton pattern
    explicit Logger(const std::string& log_file) {
        // The ofstream object is opened here in the constructor
        log_stream_.open(log_file, std::ios::app);
        if (!log_stream_.is_open()) {
            // If the file can't be opened, write an error to the standard error stream
            std::cerr << "FATAL: Could not open log file: " << log_file << std::endl;
        }
    }
    // Destructor is automatically called when the program exits
    // The ofstream destructor will automatically close the file
    ~Logger() {
        if (log_stream_.is_open()) {
            log_stream_.close();
            std::cout<< "Log file closed successfully.\n";
        }
    }
    std::mutex log_mutex_;      // Mutex to ensure thread-safe writes
    std::ofstream log_stream_;  // C++ file stream object. Manages the file automatically.
    static std::unordered_map<int, std::string> ERRORS; // Map to store different error messages
};
std::unordered_map<int, std::string> Logger::ERRORS = {
    {1, "Connection error"},
    {2, "Timeout error"},
    {3, "Data transmission error"},
    {4,"Invalid request"},
    {5,"Resource not found"},
    {6,"Could Not create socket for client(server side)"},
    {7,"Connection closed by server after sending response"},
    {8,"Client Connected Successfully"},
    {9,"INVALID_REQUEST"},
    {10,"Requested Resourse Not found"}
    // Add more error codes and messages as needed
};
// A simple macro to make logging even easier
#define LOG_INFO(msg) Logger::getInstance().log(msg, LogLevel::INFO)
#define LOG_WARNING(msg) Logger::getInstance().log(msg, LogLevel::WARNING)
#define LOG_ERROR(msg) Logger::getInstance().log(msg, LogLevel::ERROR)


#endif // Logger_HH