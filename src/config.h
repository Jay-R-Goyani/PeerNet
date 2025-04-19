#ifndef CONFIGS_H
#define CONFIGS_H

#include <string>
#include <utility>

namespace Config {

    struct Directory {
        static constexpr const char* LOGS_DIR = "logs/";          // Directory for storing log files
        static constexpr const char* NODE_FILES_DIR = "node_files/"; // Directory for storing shared files
        static constexpr const char* TRACKER_DB_DIR = "tracker_db/"; // Directory for tracker database
    };

    /**
     * @struct Constants
     * @brief System-wide constants and configuration parameters
     *
     * Defines network settings, buffer sizes, and operational parameters
     */
    struct Constants {
        static constexpr int AVAILABLE_PORT_MIN = 1024;   // Minimum available port number
        static constexpr int AVAILABLE_PORT_MAX = 65535;  // Maximum available port number

        static constexpr const char* TRACKER_IP = "127.0.0.1"; // Tracker server IP address
        static constexpr int TRACKER_PORT = 12345;        // Tracker server port number

        static constexpr int MAX_UDP_SEGMENT_DATA_SIZE = 65527;  // Maximum UDP segment size
        static constexpr int BUFFER_SIZE = 92169;        // Buffer size (based on MacOS UDP MTU)
        static constexpr int CHUNK_PIECES_SIZE = 9216 - 2000;  // Size of individual chunk pieces

        static constexpr int MAX_SPLITTNES_RATE = 10;    // Maximum split rate for file chunks
        
        static constexpr int NODE_TIME_INTERVAL = 30;    // Node update interval
        static constexpr int TRACKER_TIME_INTERVAL = 45; // Tracker update interval
    };

    /**
     * @struct TrackerRequestsMode
     * @brief Enumeration of tracker request types
     *
     * Defines the various modes of interaction between nodes and tracker
     */
    struct TrackerRequestsMode {
        static constexpr int REGISTER = 0;   // Node registration request
        static constexpr int OWN = 1;        // File ownership declaration
        static constexpr int NEED = 2;       // File request
        static constexpr int UPDATE = 3;     // Status update
        static constexpr int EXIT = 4;       // Node disconnection
        static constexpr int HEARTBEAT = 5;  // Keep-alive signal
    };

}

#endif // CONFIGS_H
