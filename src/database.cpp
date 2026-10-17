#include "database.hpp"
#include <pqxx/pqxx>
#include <iostream>
#include <stdexcept>

namespace {
AuthResult success(UserId id, const std::string& username, bool created) {
    return {id, username, {{"type", "auth.success"}, {"registered", created},
        {"user", {{"id", id}, {"username", username}}}}};
}
}

// Ubuntu's libpqxx 7.8 binary is built with the C++17 exception ABI.
// Keep only this translation unit at C++17; the WebSocket app stays C++23.
AuthResult database_authenticate(const Credentials& c,
                                const std::string& url_,
                                const std::string& dummy_hash_)
{
    // This function runs only on the dedicated worker, never the IO loop.
    try {
        pqxx::connection db{url_};
        pqxx::work tx{db};
        tx.exec("SET LOCAL statement_timeout = '10s'");
        if (c.registration) {
            char hash[crypto_pwhash_STRBYTES];
            if (crypto_pwhash_str(hash, c.password.data(), c.password.size(),
                    crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
                throw std::runtime_error("Password hashing failed");
            auto rows = tx.exec_params(
                "INSERT INTO deskio_users "
                "(username,password_hash,person_name,dob,address) "
                "VALUES ($1,$2,$3,$4::date,$5) "
                "ON CONFLICT (username) DO NOTHING RETURNING id",
                c.username, std::string(hash), c.name, c.dob, c.address);
            tx.commit();
            if (rows.empty())
                return {0, {}, {{"type", "auth.error"},
                    {"code", "username_unavailable"}}};
            return success(rows[0][0].as<UserId>(), c.username, true);
        }
        auto rows = tx.exec_params(
            "SELECT id,password_hash FROM deskio_users WHERE username=$1",
            c.username);
        const std::string hash = rows.empty() ? dummy_hash_ :
                                   rows[0][1].as<std::string>();
        const bool valid = crypto_pwhash_str_verify(hash.c_str(),
                            c.password.data(), c.password.size()) == 0;
        if (rows.empty() || !valid)
            return {0, {}, {{"type", "auth.error"},
                {"code", "invalid_credentials"}}};
        return success(rows[0][0].as<UserId>(), c.username, false);
    } catch (const pqxx::broken_connection&) {
    std::cerr << "[AUTH] PostgreSQL connection failed"
              << std::endl;

    return {0, {}, {
        {"type", "auth.error"},
        {"code", "service_unavailable"}
    }};
}
catch (const pqxx::sql_error& error) {
    // Log only SQLSTATE—not SQL text or personal data.
    std::cerr << "[AUTH] PostgreSQL SQLSTATE="
              << error.sqlstate()
              << std::endl;

    return {0, {}, {
        {"type", "auth.error"},
        {"code", "service_unavailable"}
    }};
}
catch (const std::exception&) {
    std::cerr << "[AUTH] Non-SQL failure during authentication"
              << std::endl;

    return {0, {}, {
        {"type", "auth.error"},
        {"code", "service_unavailable"}
    }};
}
}
