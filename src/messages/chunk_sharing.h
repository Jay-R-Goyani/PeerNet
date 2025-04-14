#ifndef CHUNK_SHARING_H
#define CHUNK_SHARING_H

#include "message.h"
#include <string>
#include <vector>
#include <utility>  // for std::pair
#include <stdexcept> // for std::invalid_argument

/**
 * @class ChunkSharing
 * @brief Message class for sharing file chunks between nodes
 * 
 * Handles the transfer of file chunks between nodes in the P2P network.
 * Each message contains metadata about the chunk (source, destination, filename, range)
 * as well as the actual chunk data.
 */
class ChunkSharing : public Message {
public:
    int src_node_id;                // Source node ID sending the chunk
    int dest_node_id;               // Destination node ID receiving the chunk
    std::string filename;           // Name of the file this chunk belongs to
    std::pair<int, int> range;      // Byte range of the chunk in the file (start, end)
    int idx;                        // Index of the chunk in sequence (-1 if not applicable)
    std::vector<char> chunk;        // Raw binary data of the chunk

    /**
     * @brief Constructor for ChunkSharing message
     * @param src Source node ID sending the chunk
     * @param dest Destination node ID receiving the chunk
     * @param file Name of the file being transferred
     * @param r Byte range of the chunk as pair (start, end)
     * @param i Index of chunk in sequence (optional, default -1)
     * @param c Raw chunk data (optional, default empty)
     * @throws std::invalid_argument if range is invalid
     */
    ChunkSharing(int src, int dest, const std::string& file, std::pair<int, int> r, int i = -1, std::vector<char> c = {})
        : src_node_id(src), dest_node_id(dest), filename(file), range(r), idx(i), chunk(std::move(c)) {
        // Validate that range values are logical
        if (range.first < 0 || range.second < range.first) {
            throw std::invalid_argument("Invalid range: start must be >= 0 and end must be >= start");
        }

        // Set message properties for serialization
        set_property("src_node_id", std::to_string(src_node_id));
        set_property("dest_node_id", std::to_string(dest_node_id));
        set_property("filename", filename);
        set_property("range_start", std::to_string(range.first));
        set_property("range_end", std::to_string(range.second));
        set_property("idx", std::to_string(idx));

        // Convert chunk data to string for storage in properties
        // Note: std::vector<char> cannot be stored directly in std::any
        set_property("chunk", std::string(chunk.begin(), chunk.end()));
    }

    /**
     * @brief Serializes the message into binary format
     * @return Vector of bytes representing the encoded message
     * 
     * Utilizes base Message class encoding functionality to convert
     * all properties into a transmittable binary format
     */
    std::vector<char> encode() const {
        return Message::encode();
    }

    /**
     * @brief Reconstructs a ChunkSharing object from binary data
     * @param data Binary data produced by encode()
     * @return Reconstructed ChunkSharing object
     * @throws std::invalid_argument if required properties are missing
     * 
     * Decodes the binary data and validates all required properties are present
     * before reconstructing the ChunkSharing message object
     */
    static ChunkSharing decode(const std::vector<char>& data) {
        auto properties = Message::decode(data);

        // Ensure all required properties exist in decoded data
        if (properties.find("src_node_id") == properties.end() ||
            properties.find("dest_node_id") == properties.end() ||
            properties.find("filename") == properties.end() ||
            properties.find("range_start") == properties.end() ||
            properties.find("range_end") == properties.end() ||
            properties.find("idx") == properties.end() ||
            properties.find("chunk") == properties.end()) {
            throw std::invalid_argument("Missing required properties in decoded message");
        }

        // Extract and convert properties to appropriate types
        int src = std::stoi(std::any_cast<std::string>(properties.at("src_node_id")));
        int dest = std::stoi(std::any_cast<std::string>(properties.at("dest_node_id")));
        std::string file = std::any_cast<std::string>(properties.at("filename"));
        int start = std::stoi(std::any_cast<std::string>(properties.at("range_start")));
        int end = std::stoi(std::any_cast<std::string>(properties.at("range_end")));
        int idx = std::stoi(std::any_cast<std::string>(properties.at("idx")));
        std::string chunk_str = std::any_cast<std::string>(properties.at("chunk"));

        // Convert chunk data back to vector<char>
        std::vector<char> chunk_vec(chunk_str.begin(), chunk_str.end());

        return ChunkSharing(src, dest, file, {start, end}, idx, std::move(chunk_vec));
    }
};

#endif // CHUNK_SHARING_H