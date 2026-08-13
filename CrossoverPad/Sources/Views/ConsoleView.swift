import SwiftUI

/// A live terminal for a bottle's in-process `cmd.exe`: streamed output on top,
/// a command input at the bottom. This is the visible "view a Windows CMD".
struct ConsoleView: View {
    let bottle: Bottle
    @StateObject private var session = WineConsoleSession()
    @State private var command: String = ""
    @FocusState private var inputFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            statusBar
            Divider()
            terminal
            Divider()
            inputBar
        }
        .navigationTitle("CMD — \(bottle.name)")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { session.start(); inputFocused = true }
    }

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
            Text(session.status.label)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial)
    }

    private var terminal: some View {
        ScrollViewReader { proxy in
            ScrollView {
                Text(session.output.isEmpty ? " " : session.output)
                    .font(.system(.footnote, design: .monospaced))
                    .foregroundStyle(Color.green)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
                    .id("bottom")
            }
            .background(Color.black)
            .onChange(of: session.output) { _, _ in
                withAnimation(.linear(duration: 0.1)) { proxy.scrollTo("bottom", anchor: .bottom) }
            }
        }
    }

    private var inputBar: some View {
        HStack(spacing: 8) {
            Text(">")
                .font(.system(.footnote, design: .monospaced))
                .foregroundStyle(.secondary)
            TextField("type a command (e.g. ver, dir, echo hi)", text: $command)
                .font(.system(.footnote, design: .monospaced))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($inputFocused)
                .onSubmit(run)
                .submitLabel(.send)
            Button(action: run) {
                Image(systemName: "arrow.up.circle.fill")
            }
            .disabled(!session.isRunning || command.isEmpty)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial)
    }

    private var statusColor: Color {
        switch session.status {
        case .running: return .green
        case .starting: return .yellow
        case .exited: return .gray
        case .failed: return .red
        }
    }

    private func run() {
        let cmd = command.trimmingCharacters(in: .whitespaces)
        guard !cmd.isEmpty else { return }
        session.send(cmd)
        command = ""
    }
}

#Preview {
    NavigationStack {
        ConsoleView(bottle: Bottle(name: "Sandbox", state: .ready))
    }
}
