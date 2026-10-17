#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include <sodium.h>
using Json = nlohmann::json;
using UserId = std::int64_t;

struct Credentials {
    bool registration = false;
    std::string username, password, name, dob, address;
    ~Credentials() {
        if (!password.empty()) sodium_memzero(password.data(), password.size());
    }
};

struct AuthResult {
    UserId user_id = 0;
    std::string username;
    Json response;
};

AuthResult database_authenticate(const Credentials& credentials,
                                 const std::string& url,
                                 const std::string& dummy_hash);
Json database_register_device(UserId user, const std::string& id,
                              const std::string& name, const std::string& url);
