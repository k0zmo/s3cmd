#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace s3cmd {

struct ResumeRecord
{
    std::string etag;
    std::uint64_t size{};
};

class ResumeFile
{
public:
    ResumeFile(std::filesystem::path local, std::string remote)
        : local_{std::move(local)}, remote_{std::move(remote)}
    {
    }

    struct ResumeState
    {
        ResumeRecord record;
        std::uint64_t offset;
    };

    std::optional<ResumeState> resume_state(std::optional<std::uint64_t> listed_size,
                                            std::error_code& ec) const;

    std::filesystem::path download_path() const;
    bool write_resume_record(ResumeRecord record);
    bool erase_resume_record();
    bool finish_download(std::uint64_t expected_size, bool replace);

private:
    std::optional<ResumeRecord> read_resume_record() const;

private:
    std::filesystem::path local_;
    std::string remote_;
};

} // namespace s3cmd
