//
//  DevicesListView.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI
import CoreSpotlight
import AppKit

public struct DevicesListView: View {

    @Environment(\.colorScheme)
    private var colorScheme

    @Binding private var devices: [URL]
    @Binding private var selection: Set<URL>
    @State private var rightClickedItems: Set<URL> = []

    @FocusState.Binding private var focusedField: FocusTarget?
    private let dismissWindow: () -> Void

    public init(
        devices: Binding<[URL]>,
        selection: Binding<Set<URL>>,
        focusedField: FocusState<FocusTarget?>.Binding,
        dismissWindow: @escaping () -> Void
    ) {
        self._devices = devices
        self._selection = selection
        self._focusedField = focusedField
        self.dismissWindow = dismissWindow
    }

    private var isFocused: Bool {
        focusedField == .devices
    }

    private var listEmptyView: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(systemName: "cable.connector.slash")
                .aspectRatio(contentMode: .fit)
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 24, weight: .medium))
                .frame(width: 24)
            Text("No Devices connected")
                .font(.body)
                .foregroundColor(.secondary)
            Spacer()
        }
    }

    public var body: some View {
        List(devices, id: \.self, selection: $selection) { device in
            DevicesListItem(devicePath: device)
        }
        .focused($focusedField, equals: .devices)
        .contextMenu(forSelectionType: URL.self) { items in
            if !items.isEmpty {
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting(Array(items))
                }

                Button("Copy path\(items.count > 1 ? "s" : "")") {
                    let pasteBoard = NSPasteboard.general
                    pasteBoard.clearContents()
                    pasteBoard.writeObjects(items.map(\.relativePath) as [NSString])
                }

                Button("Remove from Recents") {
                    removeRecentProjects(urls: Set(items))
                }
            }
        } primaryAction: { items in
//            for url in items {
//                NSDocumentController.shared.openDocument(at: url) {
//                    dismissWindow()
//                }
//            }
        }
        .onCopyCommand {
            selection.map { NSItemProvider(object: $0.path(percentEncoded: false) as NSString) }
        }
        .onDeleteCommand {
            removeRecentProjects()
        }
        .background {
            Button("") {
                selection.forEach { url in
//                    NSDocumentController.shared.openDocument(at: url) {
//                        dismissWindow()
//                    }
                }
            }
            .keyboardShortcut(.defaultAction)
            .hidden()
        }
        .overlay {
            if devices.isEmpty {
                listEmptyView
            }
        }
//        .onReceive(NotificationCenter.default.publisher(for: RecentsStore.didUpdateNotification)) { _ in
//            updateRecentProjects()
//        }
    }

    // MARK: - Actions

    private func removeRecentProjects(urls: Set<URL>? = nil) {
//        let targets = urls ?? selection
//        recentProjects = RecentsStore.removeRecentProjects(targets)
    }

    private func updateRecentProjects() {
//        recentProjects = RecentsStore.recentProjectURLs()
//        if !recentProjects.isEmpty {
//            selection = Set(recentProjects.prefix(1))
//        }
    }
}
