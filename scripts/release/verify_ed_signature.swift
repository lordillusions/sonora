// COM-4b: checks a Sparkle EdDSA signature the way Sonora does before it unpacks an update.
// usage: swift scripts/release/verify_ed_signature.swift <public key base64> <file> <sparkle:edSignature>
import CryptoKit
import Foundation

let args = CommandLine.arguments
guard args.count == 4,
      let keyData = Data(base64Encoded: args[1]), let signature = Data(base64Encoded: args[3]),
      let key = try? Curve25519.Signing.PublicKey(rawRepresentation: keyData),
      let file = try? Data(contentsOf: URL(fileURLWithPath: args[2])) else {
    print("usage: verify_ed_signature.swift <public key base64> <file> <signature base64>")
    exit(2)
}
let valid = key.isValidSignature(signature, for: file)
print(valid ? "valid" : "INVALID")
exit(valid ? 0 : 1)
