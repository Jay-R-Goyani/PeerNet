#ifndef NODE2TRACKER_H
#define NODE2TRACKER_H

#include "message.h"
#include <string>

/**
 * @class Node2Tracker
 * @brief Message class for node requests to tracker
 * 
 * Handles communication from nodes to tracker, including registration,
 * file ownership declarations, file requests, status updates, and disconnection notices.
 */
class Node2Tracker : public Message {
public:
    int node_id;        // Unique identifier for the requesting node
    int mode;           // Request mode (see TrackerRequestsMode in config.h)
    std::string filename;// Name of file being operated on

    /**
     * @brief Constructor for Node2Tracker message
     * @param node Node ID of the sender
     * @param m Mode of the request (register/own/need/update/exit/heartbeat)
     * @param file Name of the file (if applicable to request mode)
     * 
     * Creates a new message and sets its properties for transmission
     */
    Node2Tracker(int node, int m, const std::string& file)
        : node_id(node), mode(m), filename(file) {
        set_property("node_id", std::to_string(node_id));
        set_property("mode", std::to_string(mode));
        set_property("filename", filename);
    }

    /**
     * @brief Serializes the message into binary format
     * @return Vector of bytes representing the encoded message
     * 
     * Utilizes base Message class encoding functionality
     */
    std::vector<char> encode() const {
        return Message::encode();
    }

    /**
     * @brief Reconstructs a Node2Tracker object from binary data
     * @param data Binary data produced by encode()
     * @return Reconstructed Node2Tracker object
     * 
     * Decodes the binary data and extracts properties to create a new message object
     */
    static Node2Tracker decode(const std::vector<char>& data) {
        auto properties = Message::decode(data);

        int node = std::stoi(std::any_cast<std::string>(properties["node_id"]));
        int m = std::stoi(std::any_cast<std::string>(properties["mode"]));
        std::string file = std::any_cast<std::string>(properties["filename"]);

        return Node2Tracker(node, m, file);
    }
};

#endif // NODE2TRACKER_H
