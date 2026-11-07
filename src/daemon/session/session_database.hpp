#ifndef SOS_SESSION_DATABASE_HPP
#define SOS_SESSION_DATABASE_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <cstdint>

struct sqlite3;
struct sqlite3_stmt;

// Small ownership layer. Connections are serialized by their session owner.
class SessionDatabase {
public:
    explicit SessionDatabase(const std::filesystem::path& path);
    ~SessionDatabase();
    SessionDatabase(const SessionDatabase&) = delete;
    SessionDatabase& operator=(const SessionDatabase&) = delete;
    void execute(const char* sql);
    sqlite3* handle() const { return connection; }

    class Statement {
    public:
        Statement(SessionDatabase& db, const char* sql);
        ~Statement();
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;
        void bind(int index, std::int64_t value);
        void bind(int index, std::string_view value);
        bool step();
        std::int64_t integer(int column) const;
        std::string text(int column) const;
    private:
        sqlite3_stmt* statement = nullptr;
    };

    class Transaction {
    public:
        explicit Transaction(SessionDatabase& db);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        void commit();
    private:
        SessionDatabase& db;
        bool committed = false;
    };
private:
    sqlite3* connection = nullptr;
};

#endif // SOS_SESSION_DATABASE_HPP
