// MetalSharp's backend, as the game app launcher (main.swift) talks to it.

import Foundation
import Darwin

/// HTTP/1.1 to MetalSharp's backend over its owner-only Unix socket (<home>/run/backend.sock).
struct Backend {
    let socketPath: String

    static func standard() -> Backend {
        let env = ProcessInfo.processInfo.environment
        let home: String
        if let configured = env["METALSHARP_HOME"]?.trimmingCharacters(in: .whitespaces), !configured.isEmpty {
            home = configured
        } else {
            home = (NSHomeDirectory() as NSString).appendingPathComponent(".metalsharp")
        }
        return Backend(socketPath: (home as NSString).appendingPathComponent("run/backend.sock"))
    }

    struct Response {
        let status: Int
        let json: [String: Any]
        var ok: Bool { status < 400 && (json["ok"] as? Bool ?? false) }
        var error: String? { json["error"] as? String }
    }

    func request(_ method: String, _ path: String, body: [String: Any]? = nil, timeout: TimeInterval) -> Response? {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var noSigPipe: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, socklen_t(MemoryLayout<Int32>.size))
        var wait = timeval(tv_sec: Int(timeout), tv_usec: Int32((timeout - floor(timeout)) * 1_000_000))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))

        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(socketPath.utf8)
        let capacity = MemoryLayout.size(ofValue: address.sun_path)
        guard pathBytes.count < capacity else { return nil }
        withUnsafeMutableBytes(of: &address.sun_path) { raw in
            raw.copyBytes(from: pathBytes)
            raw[pathBytes.count] = 0
        }
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let connected = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connected == 0 else { return nil }

        var payload = Data()
        if let body = body {
            payload = (try? JSONSerialization.data(withJSONObject: body)) ?? Data("{}".utf8)
        }
        // A native client: no Origin header, a loopback Host, JSON bodies only.
        var head = "\(method) \(path) HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
        if method == "POST" {
            head += "Content-Type: application/json\r\nContent-Length: \(payload.count)\r\n"
        }
        head += "\r\n"
        var message = Data(head.utf8)
        message.append(payload)
        let sent = message.withUnsafeBytes { raw -> Bool in
            var offset = 0
            while offset < raw.count {
                let n = send(fd, raw.baseAddress! + offset, raw.count - offset, 0)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { return false }
                offset += n
            }
            return true
        }
        guard sent else { return nil }

        var received = Data()
        var buffer = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = recv(fd, &buffer, buffer.count, 0)
            if n < 0 && errno == EINTR { continue }
            if n < 0 { return nil }
            if n == 0 { break }
            received.append(buffer, count: n)
        }
        guard let split = received.range(of: Data("\r\n\r\n".utf8)),
              let headText = String(data: received[..<split.lowerBound], encoding: .utf8)
        else { return nil }
        let statusLine = headText.split(separator: "\r\n", maxSplits: 1).first ?? ""
        let parts = statusLine.split(separator: " ")
        guard parts.count >= 2, let status = Int(parts[1]) else { return nil }
        let bodyData = received[split.upperBound...]
        let json = (try? JSONSerialization.jsonObject(with: Data(bodyData))) as? [String: Any] ?? [:]
        return Response(status: status, json: json)
    }

    func answering() -> Bool {
        request("GET", "/status", timeout: 3) != nil
    }
}
