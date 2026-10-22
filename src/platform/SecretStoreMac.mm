// macOS SecretStore: login Keychain. Compiled with ARC (-fobjc-arc, see src/CMakeLists.txt).
// Moved unchanged from agent/AgentSettings.mm (COM-7).

#include "platform/SecretStore.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

namespace sonora::platform
{
namespace
{
NSDictionary* query(const SecretId& secret)
{
    auto* service = [NSString stringWithUTF8String:secret.service.toRawUTF8()];
    auto* account = [NSString stringWithUTF8String:secret.account.toRawUTF8()];
    if (! service) service = @"";
    if (! account) account = @"";
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: service,
        (__bridge id)kSecAttrAccount: account};
}
SecretStatus status(OSStatus s)
{
    if (s == errSecSuccess) return {SecretStatus::Code::Ok, 0};
    if (s == errSecItemNotFound) return {SecretStatus::Code::NotFound, static_cast<int>(s)};
    return {SecretStatus::Code::Failed, static_cast<int>(s)};
}
} // namespace

SecretStore::SecretStore(juce::File folder) : folder_(std::move(folder)) {}
const char* SecretStore::backendName() { return "Keychain"; }

bool SecretStore::contains(const SecretId& secret) const
{
    NSMutableDictionary* attributes = [query(secret) mutableCopy];
    attributes[(__bridge id)kSecReturnAttributes] = @YES;
    LAContext* context = [[LAContext alloc] init]; context.interactionNotAllowed = YES;
    attributes[(__bridge id)kSecUseAuthenticationContext] = context;
    CFTypeRef found = nullptr;
    const auto s = SecItemCopyMatching((__bridge CFDictionaryRef)attributes, &found);
    if (found) CFRelease(found);
    return s == errSecSuccess;
}

SecretStatus SecretStore::read(const SecretId& secret, juce::String& value) const
{
    value.clear();
    NSMutableDictionary* attributes = [query(secret) mutableCopy]; attributes[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef found = nullptr;
    const auto s = SecItemCopyMatching((__bridge CFDictionaryRef)attributes, &found);
    if (s != errSecSuccess) return status(s);
    auto* data = (__bridge NSData*)found;
    value = juce::String::fromUTF8(static_cast<const char*>(data.bytes), static_cast<int>(data.length));
    CFRelease(found);
    return status(s);
}

SecretStatus SecretStore::write(const SecretId& secret, const juce::String& value)
{
    auto* data = [NSData dataWithBytes:value.toRawUTF8() length:value.getNumBytesAsUTF8()];
    const auto existing = query(secret);
    auto s = SecItemUpdate((__bridge CFDictionaryRef)existing, (__bridge CFDictionaryRef)@{(__bridge id)kSecValueData: data});
    if (s == errSecItemNotFound)
    {
        NSMutableDictionary* item = [existing mutableCopy]; item[(__bridge id)kSecValueData] = data;
        item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
        s = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    }
    return status(s);
}

SecretStatus SecretStore::remove(const SecretId& secret)
{
    const auto s = SecItemDelete((__bridge CFDictionaryRef)query(secret));
    return s == errSecItemNotFound ? SecretStatus{SecretStatus::Code::Ok, 0} : status(s);
}
} // namespace sonora::platform
