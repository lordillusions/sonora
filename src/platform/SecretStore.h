#pragma once
// COM-7: per-user storage for small secrets (provider API keys), addressed by service + account.
//
//   macOS:  SecretStoreMac.mm  — login Keychain, generic password, "when unlocked, this device only".
//   others: SecretStoreFile.cpp — one JSON file in the user's application data folder
//           (Linux: ~/.config/Sonora/Secrets.json, mode 0600; Windows: %APPDATA%\Sonora\Secrets.json,
//           protected only by the user profile ACL). TODO(COM-7): libsecret / DPAPI backends.
//
// Not realtime-safe. Values are opaque UTF-8 strings.

#include <juce_core/juce_core.h>

namespace sonora::platform
{

struct SecretId
{
    juce::String service, account;
};

struct SecretStatus
{
    enum class Code { Ok, NotFound, Failed };
    Code code = Code::Failed;
    int osStatus = 0; // OSStatus on macOS, errno elsewhere; for messages only
    bool ok() const { return code == Code::Ok; }
};

class SecretStore
{
public:
    // `folder` overrides where the file backend keeps its file (tests). The Keychain backend ignores it.
    explicit SecretStore(juce::File folder = {});

    // True when the secret exists. Never shows UI (the Keychain query disallows interaction).
    bool contains(const SecretId&) const;
    SecretStatus read(const SecretId&, juce::String& value) const;
    SecretStatus write(const SecretId&, const juce::String& value);
    // Removing a missing secret is Ok.
    SecretStatus remove(const SecretId&);

    // "Keychain" or "Secret file", for messages.
    static const char* backendName();

private:
    juce::File folder_;
};

} // namespace sonora::platform
