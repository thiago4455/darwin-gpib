//
//  DevicesListView.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import SwiftUI
import AppKit

public struct DevicesListView: View {

    @Environment(\.colorScheme)
    private var colorScheme

    @EnvironmentObject private var store: DevicesStore

    @Binding private var selection: Set<String>

    @FocusState.Binding private var focusedField: FocusTarget?
    private let dismissWindow: () -> Void

    public init(
        selection: Binding<Set<String>>,
        focusedField: FocusState<FocusTarget?>.Binding,
        dismissWindow: @escaping () -> Void
    ) {
        self._selection = selection
        self._focusedField = focusedField
        self.dismissWindow = dismissWindow
    }

    private var listEmptyView: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(systemName: "cable.connector.slash")
                .aspectRatio(contentMode: .fit)
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 24, weight: .medium))
                .frame(width: 24)
            Text("No adapters connected")
                .font(.body)
                .foregroundColor(.secondary)
            Spacer()
        }
    }

    public var body: some View {
        List(selection: $selection) {
            ForEach(store.adapters) { adapter in
                Section {
                    if !store.isCollapsed(adapter) {
                        adapterContents(adapter)
                    }
                } header: {
                    AdapterSectionHeader(
                        adapter: adapter,
                        isCollapsed: store.isCollapsed(adapter),
                        isRefreshing: store.isRefreshing && adapter.instruments.isEmpty,
                        toggle: { store.toggle(adapter) }
                    )
                }
            }
        }
        .focused($focusedField, equals: .devices)
        .overlay {
            if store.isEmpty {
                listEmptyView
            }
        }
    }

    @ViewBuilder
    private func adapterContents(_ adapter: GPIBAdapter) -> some View {
        if adapter.scanUnavailable {
            statusRow("GPIB stack unreachable",
                      symbol: "exclamationmark.triangle")
        } else if adapter.scanUnreliable {
            statusRow("Address scan unreliable",
                      symbol: "exclamationmark.triangle")
        } else if adapter.instruments.isEmpty {
            statusRow(store.isRefreshing ? "Scanning…" : "No instruments",
                      symbol: "minus.circle")
        } else {
            ForEach(adapter.instruments) { instrument in
                // Selectable and highlighted, but intentionally inert for now:
                // testing actions (VISA commands and friends) land here later.
                DevicesListItem(instrument: instrument)
                    .tag(instrument.id)
            }
        }
    }

    private func statusRow(_ text: String, symbol: String) -> some View {
        HStack(spacing: 6) {
            Image(systemName: symbol)
                .font(.system(size: 11))
                .foregroundStyle(.tertiary)
            Text(text)
                .font(.system(size: 11))
                .foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(height: 24)
        // Status rows report state; they are not things you can pick, and
        // letting them take selection also tints the section header.
        .selectionDisabled()
    }
}
