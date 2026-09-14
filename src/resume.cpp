#include "resume.hpp"
#include "config.hpp"
#include "toml.hpp"
#include "utils.hpp"

#include <toml++/toml.hpp>

#include <filesystem>
#include <mutex>

namespace s3cmd {

namespace {

constexpr std::string_view download_suffix = ".s3cmddownload";

// Process-local lock. Might be a problem if we use multiple instances of totalcmd/doublecmd
std::mutex resume_mtx;

const std::filesystem::path& resume_path()
{
    static const auto value = config_directory_path() / "resume.toml";
    return value;
}

std::string resume_key(const std::filesystem::path& path)
{
    const auto normalized = std::filesystem::absolute(path).lexically_normal();
#ifdef _WIN32
    return to_utf8(normalized.native());
#else
    return normalized.native();
#endif
}

} // namespace

std::filesystem::path ResumeFile::download_path() const
{
    auto result = local_;
    result += download_suffix;
    return result;
}

std::optional<ResumeFile::ResumeState>
    ResumeFile::resume_state(std::optional<std::uint64_t> listed_size, std::error_code& ec) const
{
    const auto download = download_path();
    ec.clear();
    if (!std::filesystem::is_regular_file(download, ec) || ec)
        return std::nullopt;

    const auto offset = std::filesystem::file_size(download, ec);
    if (ec)
        return std::nullopt;
    auto record = read_resume_record();
    if (!record || offset >= record->size || (listed_size && *listed_size != record->size))
        return std::nullopt;

    return ResumeState{std::move(*record), offset};
}

std::optional<ResumeRecord> ResumeFile::read_resume_record() const
try
{
    std::scoped_lock lock(resume_mtx);
    const auto document = read_document(resume_path());
    if (!document)
        return std::nullopt;

    const auto* downloads = document->get_as<toml::table>("downloads");
    if (!downloads)
        return std::nullopt;

    const auto* entry = downloads->get_as<toml::table>(resume_key(local_));
    if (!entry)
        return std::nullopt;

    const auto stored_remote = (*entry)["remote"].value<std::string>();
    const auto etag = (*entry)["etag"].value<std::string>();
    const auto size = (*entry)["size"].value<std::int64_t>();
    if (!stored_remote || *stored_remote != remote_ || 
        !etag || etag->empty() ||
        !size || *size <= 0)
    {
        return std::nullopt;
    }
    return ResumeRecord{*etag, static_cast<std::uint64_t>(*size)};
}
catch (...)
{
    return std::nullopt;
}

bool ResumeFile::write_resume_record(ResumeRecord record)
try
{
    std::scoped_lock lock(resume_mtx);
    auto document = read_document(resume_path()).value_or(toml::table{});
    auto* downloads = document.get_as<toml::table>("downloads");
    if (!downloads)
    {
        document.insert_or_assign("downloads", toml::table{});
        downloads = document.get_as<toml::table>("downloads");
    }
    downloads->insert_or_assign(resume_key(local_),
                                toml::table{{"remote", remote_},
                                            {"etag", record.etag},
                                            {"size", static_cast<std::int64_t>(record.size)}});
    return write_document(document, resume_path());
}
catch (...)
{
    return false;
}

bool ResumeFile::erase_resume_record()
try
{
    std::scoped_lock lock(resume_mtx);
    auto document = read_document(resume_path());
    auto* downloads = document ? document->get_as<toml::table>("downloads") : nullptr;
    if (!downloads || downloads->erase(resume_key(local_)) == 0)
        return true;
    if (downloads->empty())
    {
        std::error_code ec;
        std::filesystem::remove(resume_path(), ec);
        return !ec;
    }
    return write_document(*document, resume_path());
}
catch (...)
{
    return false;
}

bool ResumeFile::finish_download(std::uint64_t expected_size)
{
    const auto download = download_path();
    std::error_code ec;
    if (std::filesystem::file_size(download, ec) != expected_size || ec)
        return false;
    std::filesystem::rename(download, local_, ec);
    if (ec)
        return false;
    erase_resume_record();
    return true;
}

} // namespace s3cmd
