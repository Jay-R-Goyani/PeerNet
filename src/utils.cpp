#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <set>
#include <random>
#include <ctime>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>
#include "config.h"  // Include your configuration header file

// Global variables
std::set<int> used_ports;  // To track used ports

// Function to create and bind a UDP socket
int set_socket(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Error: Could not create socket\n";
        return -1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Allow address reuse
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error: Could not set socket options\n";
        close(sock);
        return -1;
    }

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Error: Could not bind socket to port " << port << "\n";
        close(sock);
        return -1;
    }

    used_ports.insert(port);
    return sock;
}

// Function to free (close) a socket
void free_socket(int sock) {
    if (sock >= 0) {
        used_ports.erase(sock);
        close(sock);
    }
}

// Function to generate a random, unused port
int generate_random_port() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(Config::Constants::AVAILABLE_PORT_MIN, Config::Constants::AVAILABLE_PORT_MAX);

    int max_attempts = 1000;  // Prevent infinite loops
    int port;
    int attempts = 0;

    do {
        port = dist(gen);
        attempts++;
        if (attempts > max_attempts) {
            std::cerr << "Error: Could not find an available port after " << max_attempts << " attempts\n";
            return -1;
        }
    } while (used_ports.find(port) != used_ports.end());

    return port;
}

// Function to parse command
std::pair<std::string, std::string> parse_command(const std::string& command) {
    std::istringstream iss(command);
    std::vector<std::string> parts;
    std::string part;

    while (iss >> part) {
        parts.push_back(part);
    }

    if (parts.size() == 2) {
        return {parts[0], parts[1]};
    } else if (parts.size() == 1) {
        return {parts[0], ""};
    } else {
        std::cerr << "Warning: INVALID COMMAND ENTERED. TRY ANOTHER!\n";
        return {"", ""};
    }
}

// Function to create directory if it doesn't exist
bool create_directory(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        if (mkdir(path.c_str(), 0777) != 0) {
            std::cerr << "Error: Could not create directory " << path << "\n";
            return false;
        }
    }
    return true;
}

// Function to log messages
void log(int node_id, const std::string& content, bool is_tracker = false) {
    if (!create_directory(Config::Directory::LOGS_DIR)) {
        std::cerr << "Error: Log directory could not be created\n";
        return;
    }

    // Get current time
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    char time_str[9];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", local_time);

    std::string formatted_content;
    if (content != "I informed the tracker that I'm still alive in the torrent!") {
        formatted_content = "[" + std::string(time_str) + "]  " + content + "\n";
        std::cout << formatted_content;
    }

    // Determine log file name
    std::string log_dir = Config::Directory::LOGS_DIR;
    std::string log_file = is_tracker ? (log_dir + "_tracker.log")
                                      : (log_dir + "node" + std::to_string(node_id) + ".log");

    std::ofstream log_stream(log_file, std::ios::app);
    if (log_stream) {
        log_stream << formatted_content;
    } else {
        std::cerr << "Error: Could not open log file " << log_file << "\n";
    }
}