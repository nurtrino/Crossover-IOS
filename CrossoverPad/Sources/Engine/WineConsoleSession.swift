import Foundation
import Combine

/// Drives an in-process `cmd.exe` and exposes its output as a live, appendable
/// text buffer plus a way to send typed commands. This is the model behind
/// `ConsoleView`.
///
/// The heavy lifting (dlopen the runtime, run Wine on a background thread, pipe
/// stdio) is in the C bridge `WineHost`. This class owns the app-side pipe fds:
/// it reads stdout on a background queue and appends to `output` on the main
/// actor, and writes typed lines to stdin.
@MainActor
final class WineConsoleSession: ObservableObject {
    /// Everything the guest has printed so far.
    @Published private(set) var output: String = ""
    /// User-facing status line.
    @Published private(set) var status: Status = .starting
    /// True while the guest is alive and can accept input.
    @Published private(set) var isRunning: Bool = false

    enum Status: Equatable {
        case starting
        case running
        case exited(code: Int)
        case failed(String)

        var label: String {
            switch self {
            case .starting: return "Starting…"
            case .running: return "Running"
            case .exited(let c): return "Exited (\(c))"
            case .failed(let m): return "Failed: \(m)"
            }
        }
    }

    private var session: OpaquePointer?
    private var stdinFD: Int32 = -1
    private var stdoutFD: Int32 = -1
    private var readSource: DispatchSourceRead?
    private let readQueue = DispatchQueue(label: "wine.console.read")
    private var pollTimer: Timer?

    /// Locate the embedded runtime and a writable prefix, then start cmd.exe.
    func start() {
        guard session == nil else { return }
        do {
            let (ntdll, prefix) = try WineConsoleSession.prepareRuntime()
            var inFD: Int32 = -1
            var outFD: Int32 = -1
            guard let s = wine_console_start(ntdll.path, prefix.path, &inFD, &outFD) else {
                status = .failed("could not start the Wine runtime (dlopen/ntdll)")
                return
            }
            session = s
            stdinFD = inFD
            stdoutFD = outFD
            isRunning = true
            status = .running
            appendSystem("CrossoverPad console — cmd.exe (in-process Wine)\n")
            beginReading(fd: outFD)
            beginPollingExit()
        } catch {
            status = .failed(error.localizedDescription)
        }
    }

    /// Send a command line to the guest (a newline is appended if missing).
    func send(_ line: String) {
        guard isRunning, stdinFD >= 0 else { return }
        var text = line
        if !text.hasSuffix("\n") { text += "\r\n" }
        // Echo locally so the user sees what they typed (piped cmd doesn't echo).
        appendSystem(text)
        let bytes = Array(text.utf8)
        let fd = stdinFD
        readQueue.async {
            bytes.withUnsafeBytes { raw in
                var off = 0
                while off < raw.count {
                    let n = write(fd, raw.baseAddress!.advanced(by: off), raw.count - off)
                    if n <= 0 { break }
                    off += n
                }
            }
        }
    }

    /// Ask cmd to exit by closing its stdin (also handles the "exit" command).
    func stop() {
        if stdinFD >= 0 { close(stdinFD); stdinFD = -1 }
    }

    // MARK: - reading

    private func beginReading(fd: Int32) {
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        src.setEventHandler { [weak self] in
            var buf = [UInt8](repeating: 0, count: 4096)
            let n = read(fd, &buf, buf.count)
            if n > 0 {
                let chunk = String(decoding: buf[0..<n], as: UTF8.self)
                Task { @MainActor in self?.appendGuest(chunk) }
            } else {
                // EOF: guest closed stdout. Cancel via the captured source so we
                // don't touch a main-actor property off the main actor.
                src.cancel()
            }
        }
        src.resume()
        readSource = src
    }

    private func beginPollingExit() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkExit() }
        }
    }

    private func checkExit() {
        guard let s = session else { return }
        if wine_console_is_finished(s) != 0 {
            let code = Int(wine_console_exit_code(s))
            isRunning = false
            status = .exited(code: code)
            appendSystem("\n[cmd.exe exited with code \(code)]\n")
            pollTimer?.invalidate(); pollTimer = nil
            readSource?.cancel(); readSource = nil
        }
    }

    private func appendGuest(_ s: String) { output += s }
    private func appendSystem(_ s: String) { output += s }

    // MARK: - runtime staging

    private struct RuntimeError: LocalizedError { let msg: String; var errorDescription: String? { msg } }

    /// Returns (ntdll.so URL, writable prefix URL), copying the read-only bundled
    /// prefix into Documents on first use.
    private static func prepareRuntime() throws -> (path: URL, prefix: URL) {
        guard let rtRoot = Bundle.main.url(forResource: "WineRuntime", withExtension: nil) else {
            throw RuntimeError(msg: "WineRuntime is not embedded in this build.")
        }
        let ntdll = rtRoot.appendingPathComponent("rt/dlls/ntdll/ntdll.so")
        guard FileManager.default.fileExists(atPath: ntdll.path) else {
            throw RuntimeError(msg: "runtime incomplete: ntdll.so missing.")
        }
        let bundledPrefix = rtRoot.appendingPathComponent("prefix")
        let writable = URL.documentsDirectory.appendingPathComponent("wineprefix", isDirectory: true)
        let fm = FileManager.default
        if !fm.fileExists(atPath: writable.appendingPathComponent("drive_c").path) {
            try? fm.removeItem(at: writable)
            guard fm.fileExists(atPath: bundledPrefix.path) else {
                throw RuntimeError(msg: "runtime incomplete: prefix missing.")
            }
            try fm.copyItem(at: bundledPrefix, to: writable)
        }
        return (ntdll, writable)
    }
}
