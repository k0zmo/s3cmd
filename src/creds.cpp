#include "creds.hpp"

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/GeneralHTTPCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>

namespace s3cmd {

CredentialsProviderChain::CredentialsProviderChain(
    const Aws::Client::ClientConfiguration& configuration)
{
    using namespace Aws::Auth;
    const auto& config = configuration.credentialProviderConfig;
    AddProvider(Aws::MakeShared<EnvironmentAWSCredentialsProvider>("s3cmd"));
    AddProvider(
        Aws::MakeShared<ProfileConfigFileAWSCredentialsProvider>("s3cmd", config.profile.c_str()));
    AddProvider(Aws::MakeShared<ProcessCredentialsProvider>("s3cmd", config.profile));
    AddProvider(Aws::MakeShared<STSAssumeRoleWebIdentityCredentialsProvider>("s3cmd", config));
    AddProvider(Aws::MakeShared<SSOCredentialsProvider>(
        "s3cmd", config.profile,
        Aws::MakeShared<Aws::Client::ClientConfiguration>("s3cmd", configuration)));

    const auto relative = Aws::Environment::GetEnv(
        GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_RELATIVE_URI);
    const auto absolute = Aws::Environment::GetEnv(
        GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_FULL_URI);
    if (!relative.empty() || !absolute.empty())
    {
        auto provider = Aws::MakeShared<GeneralHTTPCredentialsProvider>(
            "s3cmd", relative, absolute,
            Aws::Environment::GetEnv(
                GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN),
            Aws::Environment::GetEnv(
                GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE));
        if (provider->IsValid())
            AddProvider(provider);
    }
    else if (!configuration.disableIMDS &&
             Aws::Utils::StringUtils::ToLower(
                 Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED").c_str()) != "true")
    {
        AddProvider(Aws::MakeShared<InstanceProfileCredentialsProvider>("s3cmd", config));
    }
}

} // namespace s3cmd
