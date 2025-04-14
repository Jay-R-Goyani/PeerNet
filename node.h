#ifndef NODE_H
#define NODE_H

// Standard Library Headers
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <pthread.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <algorithm>
#include <mutex>
#include <dirent.h>
#include <cerrno>      

// Project Headers
#include "config.h" 
#include "utils.cpp"
#include "segment.h"
#include "messages/node2node.h"
#include "messages/tracker2node.h"
#include "messages/node2tracker.h"
#include "messages/chunk_sharing.h"

class Node {
    public:
    static int node_id;                                                                   // Node ID
    static int send_socket;                                                               // Socket for sending messages
    static std::unordered_set<std::string> files;                                         // Set of files owned by this node
    static std::unordered_map<std::string, std::vector<ChunkSharing>> downloaded_files;   // Map of downloaded files and their chunks
    static bool is_in_send_mode;                                                         // Flag to indicate if the node is in send mode

    // Initialize the node with the given ID, receive port, and send port
    static void init(int id, int rcv_port, int send_port) {
        node_id = id; // Set the node ID
        send_socket = set_socket(send_port); // Create and bind the send socket
        files = fetch_owned_files(); // Fetch the list of files owned by this node
        is_in_send_mode = false; // Set the initial mode to not in send mode
    }
    
    // Fetch the list of files owned by this node
    static std::unordered_set<std::string> fetch_owned_files() {
        std::unordered_set<std::string> files; // Set to store file names
        std::string node_files_dir = std::string(Config::Directory::NODE_FILES_DIR) + "node" + std::to_string(node_id);
    
        // Check if the directory exists using stat
        struct stat st;
        if (stat(node_files_dir.c_str(), &st) != 0) {
            // If the directory doesn't exist, create it
            if (mkdir(node_files_dir.c_str(), 0755) != 0) {
                std::cerr << "Error creating directory " << node_files_dir 
                          << ": " << strerror(errno) << std::endl;
                return files; // Return empty set if directory creation fails
            }
            return files; // Return empty set for a newly created directory
        } else if (!S_ISDIR(st.st_mode)) {
            // If the path exists but is not a directory, log an error
            std::cerr << "Error: " << node_files_dir << " exists but is not a directory" << std::endl;
            return files;
        }
    
        // Open the directory for reading
        DIR* dir = opendir(node_files_dir.c_str());
        if (!dir) {
            // Log an error if the directory cannot be opened
            std::cerr << "Error opening directory " << node_files_dir 
                      << ": " << strerror(errno) << std::endl;
            return files;
        }
    
        // Read directory entries
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            // Skip the special entries "." and ".."
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
    
            // Build the full path to check the file type
            std::string full_path = node_files_dir + "/" + entry->d_name;
            
            // Check if the entry is a regular file
            if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                files.insert(entry->d_name); // Add the file name to the set
            }
        }
    
        closedir(dir); // Close the directory
        return files; // Return the set of file names
    }

};

#endif