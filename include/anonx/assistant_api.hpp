// AnonXMusic C++ port — Phase 7
// assistant_api.hpp — Abstract seam for assistant lifecycle actions.

#ifndef ANONX_ASSISTANT_API_HPP
#define ANONX_ASSISTANT_API_HPP

#include <cstdint>
#include <string>

namespace anonx {

class AssistantApi {
public:
    virtual ~AssistantApi() = default;

    enum class MemberStatus { Member, Banned, Left, Unknown };

    struct JoinResult {
        bool success = false;
        bool rateLimited = false;
    };

    // Query the assistant's current membership status in the given chat.
    virtual MemberStatus getStatus(std::int64_t chatId) = 0;

    // Attempt to unban the assistant in the given chat.
    virtual bool unban(std::int64_t chatId) = 0;

    // Join a chat via a public username.
    virtual JoinResult joinByUsername(const std::string& username) = 0;

    // Join a chat using a generated invite link.
    virtual JoinResult joinByInviteLink(const std::string& link) = 0;

    // Return the assistant's internal mention (e.g. for logs).
    virtual std::string myMention() const = 0;
};

}  // namespace anonx

#endif  // ANONX_ASSISTANT_API_HPP
