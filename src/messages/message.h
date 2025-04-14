#ifndef MESSAGE_H
#define MESSAGE_H

#include <unordered_map>
#include <string>
#include <vector>
#include <any>
#include <sstream>
#include <cstring> 

/**
 * @class Message
 * @brief A flexible container class for serializable messages
 * 
 * The Message class provides a generic interface for creating, manipulating,
 * and serializing/deserializing message objects with dynamic properties.
 * It uses type-erased storage (std::any) for maximum flexibility.
 */
class Message {
protected:
    /** Dynamic storage for message properties */
    std::unordered_map<std::string, std::any> properties;

public:
    /** Default constructor */
    Message() = default;

    /**
     * @brief Serialize the message to binary format
     * @return Vector of bytes representing the encoded message
     * 
     * Binary format:
     * [key length (4 bytes)][key][value length (4 bytes)][value]...
     */
    std::vector<char> encode() const {
        std::vector<char> buffer;
        
        // Process each key-value pair
        for (const auto& [key, value] : properties) {
            // Currently assuming all values are strings for simplicity
            std::string val = std::any_cast<std::string>(value);

            uint32_t keyLen = key.size();
            uint32_t valLen = val.size();

            // Append key length (4 bytes)
            buffer.insert(buffer.end(), 
                          reinterpret_cast<const char*>(&keyLen), 
                          reinterpret_cast<const char*>(&keyLen) + sizeof(uint32_t));

            // Append key
            buffer.insert(buffer.end(), key.begin(), key.end());

            // Append value length (4 bytes)
            buffer.insert(buffer.end(), 
                          reinterpret_cast<const char*>(&valLen), 
                          reinterpret_cast<const char*>(&valLen) + sizeof(uint32_t));

            // Append value
            buffer.insert(buffer.end(), val.begin(), val.end());
        }
        
        return buffer;
    }

    /**
     * @brief Deserialize binary data into a property map
     * @param data Binary data in the format produced by encode()
     * @return Reconstructed property map
     */
    static std::unordered_map<std::string, std::any> decode(const std::vector<char>& data) {
        std::unordered_map<std::string, std::any> result;
        size_t pos = 0;

        // Process data until we reach the end
        while (pos < data.size()) {
            // Read key length (4 bytes)
            uint32_t keyLen;
            std::memcpy(&keyLen, data.data() + pos, sizeof(uint32_t));
            pos += sizeof(uint32_t);

            // Read key
            std::string key(data.data() + pos, keyLen);
            pos += keyLen;

            // Read value length (4 bytes)
            uint32_t valLen;
            std::memcpy(&valLen, data.data() + pos, sizeof(uint32_t));
            pos += sizeof(uint32_t);

            // Read value
            std::string value(data.data() + pos, valLen);
            pos += valLen;

            // Store in result map
            result[key] = value;
        }
        
        return result;
    }

    /**
     * @brief Set a property in the message
     * @param key Property name
     * @param value Property value (type-erased)
     */
    void set_property(const std::string& key, const std::any& value) {
        properties[key] = value;
    }

    /**
     * @brief Get a property from the message
     * @param key Property name to retrieve
     * @return Property value or empty any if not found
     */
    std::any get_property(const std::string& key) const {
        if (properties.find(key) != properties.end()) {
            return properties.at(key);
        }
        return {};
    }
};

#endif // MESSAGE_H
