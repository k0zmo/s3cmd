#pragma once

#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>

namespace s3cmd {

class CredentialsProviderChain final : public Aws::Auth::AWSCredentialsProviderChain
{
public:
    explicit CredentialsProviderChain(const Aws::Client::ClientConfiguration& configuration);
};

} // namespace s3cmd
