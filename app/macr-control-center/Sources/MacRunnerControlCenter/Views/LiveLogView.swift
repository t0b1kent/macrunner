import SwiftUI
import UniformTypeIdentifiers

struct LiveLogView: View {
    @StateObject var streamer = LogStreamer()
    @State private var searchText = ""
    @State private var paused = false
    @State private var selectedLine: LogStreamer.LogLine?
    @State private var showSavePanel = false

    var filteredLines: [LogStreamer.LogLine] {
        if searchText.isEmpty { return streamer.lines }
        return streamer.lines.filter { $0.text.localizedCaseInsensitiveContains(searchText) }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField("Search logs...", text: $searchText)
                    .frame(width: 200)
                Toggle("Auto-scroll", isOn: $streamer.autoScroll)
                Button(paused ? "Resume" : "Pause") { paused.toggle() }
                Button("Clear") { streamer.lines = [] }
                Button("Save") { showSavePanel = true }
                Spacer()
                Text("Elapsed: \(streamer.elapsedMs) ms")
                    .font(.caption)
                    .monospacedDigit()
            }
            .padding(8)

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(filteredLines) { line in
                            Text(line.text)
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(color(for: line.kind))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 4)
                                .background(selectedLine?.id == line.id ? Color.accentColor.opacity(0.2) : Color.clear)
                                .onTapGesture { selectedLine = line }
                                .id(line.id)
                        }
                    }
                    .padding(.vertical, 4)
                    .onChange(of: streamer.lines.count) { _ in
                        if streamer.autoScroll, !paused, let last = filteredLines.last {
                            withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                        }
                    }
                }
            }
            .background(Color(.textBackgroundColor))

            HStack {
                Text("Lines: \(streamer.lines.count) | Filtered: \(filteredLines.count)")
                    .font(.caption)
                Spacer()
                if streamer.isRunning {
                    ProgressView().controlSize(.small)
                }
                if let line = selectedLine {
                    Button("Copy") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(line.text, forType: .string)
                    }
                }
            }
            .padding(8)
        }
        .fileExporter(
            isPresented: $showSavePanel,
            document: LogDocument(text: streamer.lines.map { $0.text }.joined(separator: "\n")),
            contentType: .plainText,
            defaultFilename: "macr-log.txt"
        ) { _ in }
    }

    func color(for kind: LogStreamer.LogKind) -> Color {
        switch kind {
        case .stdout: return .primary
        case .stderr: return .red
        case .warning: return .yellow
        case .error: return .red
        case .pass: return .green
        case .fail: return .orange
        case .info: return .blue
        }
    }
}

struct LogDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.plainText] }
    var text: String
    init(text: String) { self.text = text }
    init(configuration: ReadConfiguration) throws { text = "" }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}
