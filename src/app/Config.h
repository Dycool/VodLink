#pragma once

#include <QString>

// Deployment configuration baked into the binary at build time. The values come
// from CMake (VODLINK_GOOGLE_CLIENT_ID / VODLINK_WORKER_URL compile definitions),
// which default to the project's own values but can be overridden by environment
// variables of the same name during the build — populated from GitHub secrets in
// CI. No runtime config file is read.
//
// Desktop OAuth uses PKCE; Google still requires the client secret in the code
// exchange for Desktop-app clients, so the client ID, client secret, and Worker
// URL are all baked in.
class Config
{
public:
    [[nodiscard]] QString googleClientId() const;
    // Optional. Google's token endpoint requires the client secret for Desktop-app
    // clients even with PKCE; empty means PKCE-only exchange.
    [[nodiscard]] QString googleClientSecret() const;
    [[nodiscard]] QString workerUrl() const;
};
