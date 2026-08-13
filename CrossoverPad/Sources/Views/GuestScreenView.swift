import SwiftUI
import UIKit

/// The iOS screen of a GUI bottle: presents the guest's frontmost window
/// surface and turns touches/keys into Windows input.
///
/// Thread contract (mirrors wineios_host.h): everything UIKit happens here on
/// the main thread; the only communication with Wine is the lock-protected
/// `wine_display_*` C bridge — polling pixels out on a CADisplayLink, pushing
/// input events into a queue a Wine thread drains. No UIKit is ever called
/// from a Wine thread and no Wine call is ever made from the main thread.
struct GuestScreenView: View {
    let bottle: Bottle
    var exe: String = "notepad.exe"
    @StateObject private var session = WineGuiSession()
    @State private var showLog = false

    var body: some View {
        VStack(spacing: 0) {
            statusBar
            Divider()
            GuestSurfaceView()
                .background(Color.black)
            if showLog {
                Divider()
                ScrollView {
                    Text(session.log.isEmpty ? "(no runtime output)" : session.log)
                        .font(.system(.caption2, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                }
                .frame(maxHeight: 160)
            }
        }
        .navigationTitle("\(exe) — \(bottle.name)")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { session.start(exe: exe) }
    }

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(session.isRunning ? Color.green : Color.orange)
                .frame(width: 8, height: 8)
            Text(session.status.label)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
            Spacer()
            Button {
                showLog.toggle()
            } label: {
                Image(systemName: "doc.plaintext")
            }
            .font(.caption)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial)
    }
}

/// SwiftUI wrapper for the UIKit presenter view.
private struct GuestSurfaceView: UIViewRepresentable {
    func makeUIView(context: Context) -> GuestSurfaceUIView { GuestSurfaceUIView() }
    func updateUIView(_ uiView: GuestSurfaceUIView, context: Context) {}
}

/// UIKit presenter: a CADisplayLink polls the display bridge and blits the
/// frontmost guest surface into the view's layer; touches map to mouse input,
/// and the view is a UIKeyInput so the system keyboard types into the guest.
final class GuestSurfaceUIView: UIView, UIKeyInput {
    private var displayLink: CADisplayLink?
    private var lastChangeSeq: UInt64 = 0
    private var lastPixelSeq: UInt64 = 0

    /// The surface currently on screen (frontmost visible, else newest).
    private var current: wine_gui_surface_info?
    private var pixelBuffer: UnsafeMutableRawPointer?
    private var pixelBufferSize: Int = 0

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = false
        layer.contentsGravity = .resizeAspect
        layer.magnificationFilter = .nearest
        backgroundColor = .black
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        pixelBuffer?.deallocate()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            let link = CADisplayLink(target: self, selector: #selector(tick))
            link.add(to: .main, forMode: .common)
            displayLink = link
            becomeFirstResponder()
        } else {
            displayLink?.invalidate()
            displayLink = nil
        }
    }

    // MARK: - presentation

    @objc private func tick() {
        // Stop drawing when backgrounded; the guest keeps painting into its
        // surface and we catch up on the next foreground frame.
        guard window?.windowScene?.activationState == .foregroundActive
                || window?.windowScene == nil else { return }

        let seq = wine_display_change_seq()
        guard seq != lastChangeSeq else { return }
        lastChangeSeq = seq

        var infos = [wine_gui_surface_info](repeating: wine_gui_surface_info(), count: 32)
        let count = Int(wine_display_list_surfaces(&infos, 32))
        guard count > 0 else {
            current = nil
            layer.contents = nil
            return
        }
        // v1 window management: show the last-created visible surface
        // full-screen; fall back to the newest surface while nothing is
        // marked visible yet.
        let pick = infos[0..<count].last(where: { $0.visible != 0 }) ?? infos[count - 1]

        if let cur = current, cur.id == pick.id, pick.update_seq == lastPixelSeq {
            current = pick
            return
        }
        current = pick

        let needed = Int(pick.width) * Int(pick.height) * 4
        if pixelBufferSize < needed {
            pixelBuffer?.deallocate()
            pixelBuffer = UnsafeMutableRawPointer.allocate(byteCount: needed, alignment: 16)
            pixelBufferSize = needed
        }
        guard let buf = pixelBuffer,
              wine_display_copy_surface(pick.id, buf, needed) != 0 else { return }
        lastPixelSeq = pick.update_seq

        let data = Data(bytes: buf, count: needed)
        guard let provider = CGDataProvider(data: data as CFData),
              let image = CGImage(width: Int(pick.width),
                                  height: Int(pick.height),
                                  bitsPerComponent: 8,
                                  bitsPerPixel: 32,
                                  bytesPerRow: Int(pick.width) * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue
                                                           | CGImageAlphaInfo.premultipliedFirst.rawValue),
                                  provider: provider,
                                  decode: nil,
                                  shouldInterpolate: false,
                                  intent: .defaultIntent) else { return }
        layer.contents = image
    }

    // MARK: - touch -> mouse

    /// Maps a view point onto the displayed surface (aspect-fit) and returns
    /// Windows virtual-desktop pixel coordinates.
    private func desktopPoint(for location: CGPoint) -> (Int32, Int32)? {
        guard let surf = current, surf.width > 0, surf.height > 0 else { return nil }
        let sw = CGFloat(surf.width), sh = CGFloat(surf.height)
        let scale = min(bounds.width / sw, bounds.height / sh)
        guard scale > 0 else { return nil }
        let drawn = CGSize(width: sw * scale, height: sh * scale)
        let origin = CGPoint(x: (bounds.width - drawn.width) / 2,
                             y: (bounds.height - drawn.height) / 2)
        let px = (location.x - origin.x) / scale
        let py = (location.y - origin.y) / scale
        guard px >= 0, py >= 0, px < sw, py < sh else { return nil }
        return (surf.x + Int32(px), surf.y + Int32(py))
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first, let (x, y) = desktopPoint(for: t.location(in: self)),
              let surf = current else { return }
        wine_display_send_mouse_move(surf.id, x, y)
        wine_display_send_mouse_button(surf.id, x, y, 0, 1)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first, let (x, y) = desktopPoint(for: t.location(in: self)),
              let surf = current else { return }
        wine_display_send_mouse_move(surf.id, x, y)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first, let surf = current else { return }
        if let (x, y) = desktopPoint(for: t.location(in: self)) {
            wine_display_send_mouse_button(surf.id, x, y, 0, 0)
        } else {
            // lifted outside the surface: still release the button
            wine_display_send_mouse_button(surf.id, surf.x, surf.y, 0, 0)
        }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let surf = current else { return }
        wine_display_send_mouse_button(surf.id, surf.x, surf.y, 0, 0)
    }

    // MARK: - keyboard (UIKeyInput)

    override var canBecomeFirstResponder: Bool { true }

    var hasText: Bool { true }

    func insertText(_ text: String) {
        for unit in text.utf16 {
            if unit == 0x0A || unit == 0x0D {  // Return
                wine_display_send_key(0x0D, 1)  // VK_RETURN
                wine_display_send_key(0x0D, 0)
            } else {
                wine_display_send_char(UInt32(unit))
            }
        }
    }

    func deleteBackward() {
        wine_display_send_key(0x08, 1)  // VK_BACK
        wine_display_send_key(0x08, 0)
    }
}
