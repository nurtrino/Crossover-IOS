import Foundation
import UIKit

/// Drives an in-process GUI program (default `notepad.exe`) and exposes its
/// lifecycle plus a diagnostic log. The pixels themselves never pass through
/// this class: the wineios.drv display driver registers window surfaces with
/// the C bridge (`WineDisplayHost`), and `GuestScreenView` polls that bridge
/// from a CADisplayLink. This class only starts/stops the guest.
@MainActor
final class WineGuiSession: ObservableObject {
    @Published private(set) var status: Status = .starting
    @Published private(set) var isRunning: Bool = false
    /// The guest's stdout+stderr (Wine diagnostics), for the debug drawer.
    @Published private(set) var log: String = ""

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
    private var logFD: Int32 = -1
    private var readSource: DispatchSourceRead?
    private let readQueue = DispatchQueue(label: "wine.gui.read")
    private var pollTimer: Timer?

    func start(exe: String = "notepad.exe") {
        guard session == nil else { return }
        do {
            let (ntdll, prefix) = try WineConsoleSession.prepareRuntime()

            // The Windows desktop runs at the screen's native PIXEL size and
            // the presenter blits 1:1; report it before the driver asks.
            let native = UIScreen.main.nativeBounds
            wine_display_host_configure(UInt32(native.width), UInt32(native.height))

            var outFD: Int32 = -1
            guard let s = wine_gui_start(ntdll.path, prefix.path, exe, &outFD) else {
                status = .failed("could not start the Wine runtime (dlopen/ntdll)")
                return
            }
            session = s
            logFD = outFD
            isRunning = true
            status = .running
            beginReading(fd: outFD)
            beginPollingExit()
        } catch {
            status = .failed(error.localizedDescription)
        }
    }

    private func beginReading(fd: Int32) {
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        src.setEventHandler { [weak self] in
            var buf = [UInt8](repeating: 0, count: 4096)
            let n = read(fd, &buf, buf.count)
            if n > 0 {
                let chunk = String(decoding: buf[0..<n], as: UTF8.self)
                Task { @MainActor in self?.log += chunk }
            } else {
                src.cancel()
            }
        }
        src.resume()
        readSource = src
    }

    private func beginPollingExit() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkExit() }
        }
    }

    private func checkExit() {
        guard let s = session else { return }
        if wine_console_is_finished(s) != 0 {
            let code = Int(wine_console_exit_code(s))
            isRunning = false
            status = .exited(code: code)
            pollTimer?.invalidate(); pollTimer = nil
            readSource?.cancel(); readSource = nil
        }
    }
}
